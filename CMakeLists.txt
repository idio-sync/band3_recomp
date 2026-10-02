# band3 - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(band3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
file(GLOB_RECURSE BAND3_SOURCES src/*.cpp src/*.c)

if(WIN32)
    add_executable(band3 WIN32 ${BAND3_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/band3.rc)
else()
    add_executable(band3 ${BAND3_SOURCES})
endif()

rexglue_setup_target(band3 GPU_PLUGINS xenos)

# UDP sockets for the RB3E network events (src/Net/events.cpp)
if(WIN32)
    target_link_libraries(band3 PRIVATE ws2_32)
    # the SDK's runtime DLL by file name (rexruntime[d|rd].dll), for src/Content/content_hooks.cpp
    target_compile_definitions(band3 PRIVATE BAND3_REXRUNTIME_DLL="$<TARGET_FILE_NAME:rex::runtime>")
    # symbol map, to resolve the frames src/crash_trace.cpp logs
    target_link_options(band3 PRIVATE "LINKER:/MAP:${CMAKE_BINARY_DIR}/band3.map")
endif()

# MIDI input for MIDI drum kits (src/Input/midi_drums_driver.cpp)
include(cmake/rtmidi.cmake)
band3_setup_rtmidi(band3 ${CMAKE_CURRENT_SOURCE_DIR}/src/ThirdParty/rtmidi/RtMidi.cpp)
