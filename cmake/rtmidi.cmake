# RtMidi (src/ThirdParty/rtmidi) reads MIDI drum kits. This picks its backend
# for the platform and links what that needs. Without one (Linux with no ALSA
# development files), RtMidi builds as a stub that finds no MIDI ports.
#
# Call from the directory that adds rtmidi_source to target, since source file
# properties are set per directory.
function(band3_setup_rtmidi target rtmidi_source)
    if(WIN32)
        set_source_files_properties(${rtmidi_source} PROPERTIES COMPILE_DEFINITIONS __WINDOWS_MM__)
        target_link_libraries(${target} PRIVATE winmm)
    elseif(APPLE)
        set_source_files_properties(${rtmidi_source} PROPERTIES COMPILE_DEFINITIONS __MACOSX_CORE__)
        target_link_libraries(${target} PRIVATE
            "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation")
    else()
        find_package(ALSA)
        if(ALSA_FOUND)
            set_source_files_properties(${rtmidi_source} PROPERTIES COMPILE_DEFINITIONS __LINUX_ALSA__)
            find_package(Threads REQUIRED)
            target_link_libraries(${target} PRIVATE ALSA::ALSA Threads::Threads)
        else()
            message(WARNING "ALSA development files not found, so MIDI drum kits won't be "
                            "read (install libasound2-dev or alsa-lib-devel)")
        endif()
    endif()
endfunction()
