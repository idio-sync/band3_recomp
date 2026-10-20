# What src/ needs to compile, beyond its own sources: definitions, third-party
# headers and generated headers. Both the band3 target (CMakeLists.txt) and the
# CI compile check (tools/compile_check) call this, so a step added here reaches
# both; add link-only settings to CMakeLists.txt instead.

include(${CMAKE_CURRENT_LIST_DIR}/ffmpeg.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/rtmidi.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/gameinput.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/web_client.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/miniupnpc.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/embed.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/build_tag.cmake)

# Call from the directory that adds the target's sources.
function(band3_setup_compile target root)
    # the SDK's runtime library by file name (rexruntime[d|rd].dll,
    # librexruntime[d|rd].so), for src/sdk_export.cpp
    target_compile_definitions(${target} PRIVATE
        BAND3_REXRUNTIME_DLL="$<TARGET_FILE_NAME:rex::runtime>")

    # renderdoc_app.h, which the SDK's Vulkan headers include
    # (rex/ui/renderdoc_api.h) but rex::runtime doesn't bring along: for the
    # Vulkan presenter in src/Render/sync_gpu/sync_graphics_system.cpp
    if(NOT WIN32 AND TARGET rex::renderdoc)
        target_link_libraries(${target} PRIVATE rex::renderdoc)
    endif()

    # FFmpeg, which decodes music videos (src/Video/video_decoder_ffmpeg.cpp)
    band3_setup_ffmpeg(${target})

    # MIDI input for MIDI drum kits (src/Input/midi_drums_driver.cpp)
    band3_setup_rtmidi(${target} ${root}/src/ThirdParty/rtmidi/RtMidi.cpp)

    # Xbox One instruments through GameInput (src/Input/gameinput_instruments.cpp)
    band3_setup_gameinput(${target})

    # HTTPS requests for RhythmVerse's songs (src/Net/web_client.cpp)
    band3_setup_web_client(${target})

    # UPnP port mapping for Liveless (src/Net/port_mapping.cpp)
    band3_setup_miniupnpc(${target} ${root}/src/ThirdParty/miniupnpc)

    # the launcher's type, Montserrat (src/Launcher/ui/theme.cpp), compiled in
    band3_embed_file(${target} ${root}/src/ThirdParty/montserrat/Montserrat-SemiBold.ttf
                     kMontserratSemiBold)
    band3_embed_file(${target} ${root}/src/ThirdParty/montserrat/Montserrat-ExtraBold.ttf
                     kMontserratExtraBold)

    # the build tag src/build_tag.cpp gives the log and the Liveless Rooms server
    band3_setup_build_tag(${target} ${root})
endfunction()
