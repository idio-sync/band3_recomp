# band3 - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(band3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# rexglue.cmake compiles the generated code with asynchronous EH for guest SEH
# scopes, which band3's codegen doesn't emit (generateExceptionHandlers is off:
# no SEH_TRY in generated/); guest faults resume through the SDK's exception
# handler, not by unwinding. Asynchronous EH makes every memory access a
# possible throw, which keeps clang from reordering or keeping values in
# registers across them, so it goes.
list(REMOVE_ITEM REXGLUE_RECOMP_OPTIONS -fasync-exceptions /EHa)
if(REXGLUE_ENTRYPOINT_GENERATED_SOURCES)
    set_source_files_properties(${REXGLUE_ENTRYPOINT_GENERATED_SOURCES}
        PROPERTIES COMPILE_OPTIONS "${REXGLUE_RECOMP_OPTIONS}")
endif()

# Sources
file(GLOB_RECURSE BAND3_SOURCES src/*.cpp src/*.c)

if(WIN32)
    add_executable(band3 WIN32 ${BAND3_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/band3.rc)
else()
    add_executable(band3 ${BAND3_SOURCES})
endif()

rexglue_setup_target(band3 GPU_PLUGINS xenos)

# the SDK's runtime library by file name (rexruntime[d|rd].dll,
# librexruntime[d|rd].so), for src/sdk_export.cpp
target_compile_definitions(band3 PRIVATE BAND3_REXRUNTIME_DLL="$<TARGET_FILE_NAME:rex::runtime>")
target_link_libraries(band3 PRIVATE ${CMAKE_DL_LIBS})
# the SDK stages its runtime beside band3 on Windows and macOS only; Linux
# looks for it there too ($ORIGIN)
if(UNIX AND NOT APPLE)
    add_custom_command(TARGET band3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:rex::runtime>
                $<TARGET_FILE_DIR:band3>)
    # only the SDK's overrides and C++ names exported: not the guest functions
    # named as a C library's (cmake/linux_exports.map)
    set(BAND3_EXPORTS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux_exports.map)
    target_link_options(band3 PRIVATE "LINKER:--version-script=${BAND3_EXPORTS}")
    set_property(TARGET band3 APPEND PROPERTY LINK_DEPENDS ${BAND3_EXPORTS})
endif()

# renderdoc_app.h, which the SDK's Vulkan headers include
# (rex/ui/renderdoc_api.h) but rex::runtime doesn't bring along: for the
# Vulkan presenter in src/Render/sync_gpu/sync_graphics_system.cpp
if(NOT WIN32 AND TARGET rex::renderdoc)
    target_link_libraries(band3 PRIVATE rex::renderdoc)
endif()

# UDP sockets for the RB3E network events (src/Net/events.cpp)
if(WIN32)
    target_link_libraries(band3 PRIVATE ws2_32)
    # symbol map, to resolve the frames src/crash_trace.cpp logs
    target_link_options(band3 PRIVATE "LINKER:/MAP:${CMAKE_BINARY_DIR}/band3.map")
    # band3.pdb with the public symbols (nothing is compiled with debug info),
    # for WinDbg to name the frames in crash_trace.cpp's minidumps; /DEBUG
    # would otherwise turn off the /OPT:REF and /OPT:ICF a release link has
    target_link_options(band3 PRIVATE "LINKER:/DEBUG" "LINKER:/OPT:REF" "LINKER:/OPT:ICF")
endif()

# Media Foundation, which decodes music videos without FFmpeg (src/Video/video_decoder_mf.cpp)
if(WIN32)
    target_link_libraries(band3 PRIVATE mfplat mfreadwrite mfuuid)
endif()

# FFmpeg, which decodes music videos (src/Video/video_decoder_ffmpeg.cpp)
include(cmake/ffmpeg.cmake)
band3_setup_ffmpeg(band3)

# MIDI input for MIDI drum kits (src/Input/midi_drums_driver.cpp)
include(cmake/rtmidi.cmake)
band3_setup_rtmidi(band3 ${CMAKE_CURRENT_SOURCE_DIR}/src/ThirdParty/rtmidi/RtMidi.cpp)

# Xbox One instruments through GameInput (src/Input/gameinput_instruments.cpp)
include(cmake/gameinput.cmake)
band3_setup_gameinput(band3)

# HTTPS requests for RhythmVerse's songs (src/Net/web_client.cpp)
include(cmake/web_client.cmake)
band3_setup_web_client(band3)

# UPnP port mapping for Liveless (src/Net/port_mapping.cpp)
include(cmake/miniupnpc.cmake)
band3_setup_miniupnpc(band3 ${CMAKE_CURRENT_SOURCE_DIR}/src/ThirdParty/miniupnpc)

# the build tag src/build_tag.cpp gives the log and the Liveless Rooms server
include(cmake/build_tag.cmake)
band3_setup_build_tag(band3 ${CMAKE_CURRENT_SOURCE_DIR})
