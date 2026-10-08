# FFmpeg's libraries, which decode music videos (src/Video/video_decoder_ffmpeg.cpp):
# VP9, AV1, H.264, HEVC and more, in WebM, MKV, MP4 and the rest. Only their
# headers are built against: band3 loads the libraries at run time
# (src/Video/ffmpeg_api.h), so it starts without them.
#
# Windows: a pinned LGPL shared build from BtbN/FFmpeg-Builds (a month's last
# build, which they keep), downloaded at configure time into the build folder,
# or the one -DBAND3_FFMPEG_ROOT=<folder> names (its include/ and bin/). Its
# DLLs are copied beside the target; without them the decoder falls back to
# Media Foundation.
# Elsewhere: the system's headers, through pkg-config (libavformat-dev,
# libavcodec-dev, libavutil-dev and libswscale-dev on Debian and Ubuntu), and
# at run time the libraries of the same major versions (libavformat.so.61
# and so on, which the system's FFmpeg package brings).
#
# -DBAND3_FFMPEG=OFF leaves it out. Without it a target still builds: music
# videos then decode through Media Foundation on Windows, and not at all
# elsewhere. The target gets BAND3_HAVE_FFMPEG when it has it.

option(BAND3_FFMPEG "Decode music videos with FFmpeg" ON)
set(BAND3_FFMPEG_ROOT "" CACHE PATH
    "A Windows FFmpeg shared build (include/, bin/) to use instead of downloading one")

set(BAND3_FFMPEG_TAG "autobuild-2026-09-30-13-08")
set(BAND3_FFMPEG_NAME "ffmpeg-n8.1.3-9-g29e619e767-win64-lgpl-shared-8.1")
set(BAND3_FFMPEG_SHA256 "3e47bda1607740550141e37c0e49d1e5182b34699f15adfd137ee266d346811a")
# the FFmpeg commit it's built from, for the license file's source line
set(BAND3_FFMPEG_COMMIT "29e619e767")

# the build's folder: BAND3_FFMPEG_ROOT, or the download (fetched once per
# build folder); empty, with a warning, if it can't be had
function(band3_ffmpeg_windows_root out)
    if(BAND3_FFMPEG_ROOT)
        set(${out} "${BAND3_FFMPEG_ROOT}" PARENT_SCOPE)
        return()
    endif()
    set(dir "${CMAKE_BINARY_DIR}/ffmpeg")
    set(root "${dir}/${BAND3_FFMPEG_NAME}")
    if(NOT EXISTS "${root}/include/libavcodec/avcodec.h")
        set(zip "${dir}/${BAND3_FFMPEG_NAME}.zip")
        set(url "https://github.com/BtbN/FFmpeg-Builds/releases/download/${BAND3_FFMPEG_TAG}/${BAND3_FFMPEG_NAME}.zip")
        message(STATUS "Downloading FFmpeg for music videos: ${url}")
        file(DOWNLOAD "${url}" "${zip}" EXPECTED_HASH SHA256=${BAND3_FFMPEG_SHA256}
             STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            list(GET status 1 reason)
            message(WARNING "FFmpeg couldn't be downloaded (${reason}), so music videos decode "
                            "through Media Foundation only. Set BAND3_FFMPEG_ROOT to a "
                            "downloaded ${BAND3_FFMPEG_NAME} to use it anyway.")
            file(REMOVE "${zip}")
            set(${out} "" PARENT_SCOPE)
            return()
        endif()
        file(ARCHIVE_EXTRACT INPUT "${zip}" DESTINATION "${dir}")
        file(REMOVE "${zip}")
    endif()
    set(${out} "${root}" PARENT_SCOPE)
endfunction()

function(band3_setup_ffmpeg target)
    if(NOT BAND3_FFMPEG)
        return()
    endif()
    # an object library (tools/compile_check) only compiles: no copies
    get_target_property(type ${target} TYPE)
    set(linked TRUE)
    if(type STREQUAL "OBJECT_LIBRARY")
        set(linked FALSE)
    endif()
    if(WIN32)
        band3_ffmpeg_windows_root(root)
        if(NOT root)
            return()
        endif()
        if(NOT EXISTS "${root}/include/libavformat/avformat.h")
            message(WARNING "${root} has no include/libavformat/avformat.h, so music videos "
                            "decode through Media Foundation only")
            return()
        endif()
        target_include_directories(${target} SYSTEM PRIVATE "${root}/include")
        target_compile_definitions(${target} PRIVATE BAND3_HAVE_FFMPEG)
        if(NOT linked)
            return()
        endif()
        # the DLLs beside the target (swresample too, which avcodec needs)
        file(GLOB dlls "${root}/bin/avformat-*.dll" "${root}/bin/avcodec-*.dll"
                       "${root}/bin/avutil-*.dll" "${root}/bin/swscale-*.dll"
                       "${root}/bin/swresample-*.dll")
        foreach(dll ${dlls})
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" $<TARGET_FILE_DIR:${target}>)
        endforeach()
        # its license beside the DLLs, for tools/package.py's licenses/ffmpeg.txt
        if(EXISTS "${root}/LICENSE.txt")
            file(READ "${root}/LICENSE.txt" license)
            file(WRITE "${CMAKE_BINARY_DIR}/ffmpeg-license.txt"
                 "FFmpeg (${BAND3_FFMPEG_NAME}), LGPL 2.1 or later: the avcodec, avformat, "
                 "avutil, swresample and swscale DLLs beside band3, unmodified, from "
                 "https://github.com/BtbN/FFmpeg-Builds/releases/tag/${BAND3_FFMPEG_TAG}. "
                 "Their source: https://github.com/FFmpeg/FFmpeg/commit/${BAND3_FFMPEG_COMMIT}

"
                 "${license}")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${CMAKE_BINARY_DIR}/ffmpeg-license.txt" $<TARGET_FILE_DIR:${target}>)
        endif()
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(BAND3_FFMPEG_PC QUIET libavformat libavcodec libavutil libswscale)
        endif()
        if(NOT BAND3_FFMPEG_PC_FOUND)
            message(WARNING "FFmpeg's development files not found, so music videos won't play "
                            "(install libavformat-dev, libavcodec-dev, libavutil-dev and "
                            "libswscale-dev, or ffmpeg-devel)")
            return()
        endif()
        target_include_directories(${target} SYSTEM PRIVATE ${BAND3_FFMPEG_PC_INCLUDE_DIRS})
        target_compile_definitions(${target} PRIVATE BAND3_HAVE_FFMPEG)
        # dlopen
        if(linked)
            target_link_libraries(${target} PRIVATE ${CMAKE_DL_LIBS})
        endif()
    endif()
endfunction()
