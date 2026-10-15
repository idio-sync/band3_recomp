# FFmpeg's libraries, which decode music videos (src/Video/video_decoder_ffmpeg.cpp):
# VP9, AV1, H.264, HEVC and more, in WebM, MKV, MP4 and the rest. Only their
# headers are built against: band3 loads the libraries at run time
# (src/Video/ffmpeg_api.h), so it starts without them.
#
# band3's own trimmed LGPL build (only the decoders and demuxers band3 uses,
# about a tenth the size of a full build): deps/ffmpeg-<version>-band3-win64.zip
# (tools/build_ffmpeg.sh) on Windows, deps/ffmpeg-<version>-band3-linux-amd64.zip
# (tools/build_ffmpeg_linux.sh) on Linux, unpacked into the build folder, or the
# one -DBAND3_FFMPEG_ROOT=<folder> names (its include/, and bin/ or lib/). Its
# libraries are copied beside the target. On Linux band3 carries its own because
# distributions ship different FFmpeg major versions, and band3 runs only with
# the one its headers are.
# Without the zip: on Windows, the decoder falls back to Media Foundation;
# elsewhere, the system's headers, through pkg-config (libavformat-dev,
# libavcodec-dev, libavutil-dev and libswscale-dev on Debian and Ubuntu), and
# at run time the system's libraries of the same major versions (libavformat.so.61
# and so on, which the system's FFmpeg package brings).
#
# -DBAND3_FFMPEG=OFF leaves it out. Without it a target still builds: music
# videos then decode through Media Foundation on Windows, and not at all
# elsewhere. The target gets BAND3_HAVE_FFMPEG when it has it.

option(BAND3_FFMPEG "Decode music videos with FFmpeg" ON)
set(BAND3_FFMPEG_ROOT "" CACHE PATH
    "An FFmpeg shared build (include/, and bin/ or lib/) to use instead of deps/' zip")

# tools/build_ffmpeg.sh's or tools/build_ffmpeg_linux.sh's zip, beside this file's folder
if(WIN32)
    set(BAND3_FFMPEG_ZIP "${CMAKE_CURRENT_LIST_DIR}/../deps/ffmpeg-8.1.3-band3-win64.zip")
else()
    set(BAND3_FFMPEG_ZIP "${CMAKE_CURRENT_LIST_DIR}/../deps/ffmpeg-8.1.3-band3-linux-amd64.zip")
endif()

# the build's folder: BAND3_FFMPEG_ROOT, or the zip unpacked (once per build
# folder, again when the zip changes); empty without one, with a warning on
# Windows (elsewhere the system's FFmpeg stands in)
function(band3_ffmpeg_own_root out)
    if(BAND3_FFMPEG_ROOT)
        set(${out} "${BAND3_FFMPEG_ROOT}" PARENT_SCOPE)
        return()
    endif()
    if(NOT EXISTS "${BAND3_FFMPEG_ZIP}")
        if(WIN32)
            message(WARNING "No ${BAND3_FFMPEG_ZIP}, so music videos decode through Media "
                            "Foundation only (tools/build_ffmpeg.sh makes it)")
        endif()
        set(${out} "" PARENT_SCOPE)
        return()
    endif()
    get_filename_component(name "${BAND3_FFMPEG_ZIP}" NAME_WLE)
    set(dir "${CMAKE_BINARY_DIR}/ffmpeg")
    file(SHA256 "${BAND3_FFMPEG_ZIP}" hash)
    if(NOT EXISTS "${dir}/${name}.sha256" OR NOT EXISTS "${dir}/${name}/include")
        set(old "")
    else()
        file(READ "${dir}/${name}.sha256" old)
    endif()
    if(NOT old STREQUAL hash)
        file(REMOVE_RECURSE "${dir}/${name}")
        file(ARCHIVE_EXTRACT INPUT "${BAND3_FFMPEG_ZIP}" DESTINATION "${dir}")
        file(WRITE "${dir}/${name}.sha256" "${hash}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BAND3_FFMPEG_ZIP}")
    set(${out} "${dir}/${name}" PARENT_SCOPE)
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
    band3_ffmpeg_own_root(root)
    if(root AND NOT EXISTS "${root}/include/libavformat/avformat.h")
        message(WARNING "${root} has no include/libavformat/avformat.h, so it's not used")
        set(root "")
    endif()
    if(NOT root AND WIN32)
        return()
    endif()
    if(root)
        target_include_directories(${target} SYSTEM PRIVATE "${root}/include")
        target_compile_definitions(${target} PRIVATE BAND3_HAVE_FFMPEG)
        if(NOT linked)
            return()
        endif()
        if(NOT WIN32)
            target_link_libraries(${target} PRIVATE ${CMAKE_DL_LIBS})  # dlopen
        endif()
        # the libraries beside the target
        file(GLOB libs "${root}/bin/avformat-*.dll" "${root}/bin/avcodec-*.dll"
                       "${root}/bin/avutil-*.dll" "${root}/bin/swscale-*.dll"
                       "${root}/bin/swresample-*.dll" "${root}/lib/libavformat.so.*"
                       "${root}/lib/libavcodec.so.*" "${root}/lib/libavutil.so.*"
                       "${root}/lib/libswscale.so.*" "${root}/lib/libswresample.so.*")
        foreach(lib ${libs})
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${lib}" $<TARGET_FILE_DIR:${target}>)
        endforeach()
        # the licenses beside them, as one file, for tools/package.py's
        # licenses/ffmpeg.txt
        set(license "")
        foreach(part LICENSE.txt COPYING.LGPLv2.1 COPYING.dav1d)
            if(EXISTS "${root}/${part}")
                file(READ "${root}/${part}" text)
                string(APPEND license "${text}

")
            endif()
        endforeach()
        if(license)
            file(WRITE "${CMAKE_BINARY_DIR}/ffmpeg-license.txt" "${license}")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${CMAKE_BINARY_DIR}/ffmpeg-license.txt" $<TARGET_FILE_DIR:${target}>)
        endif()
        return()
    endif()
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
endfunction()
