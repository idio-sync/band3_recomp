# GameInput 3 reads Xbox One instruments' raw reports on Windows
# (src/Input/gameinput_instruments.cpp). Its header and loader library come from
# Microsoft's Microsoft.GameInput NuGet package, fetched at configure time
# rather than checked in, as Microsoft licenses it apart from band3. The loader
# finds GameInput's runtime when band3 starts, so a PC without one still runs
# band3, just without Xbox One instruments. Without the package (offline, say)
# band3 builds without them, saying so.
set(BAND3_GAMEINPUT_VERSION 3.0.26100.6154)
set(BAND3_GAMEINPUT_SHA256 d9e34e70851e0ac07f3bf30d682543d32a15bfa8bbecaeca1ca4f071e706fd56)

function(band3_setup_gameinput target)
    if(NOT WIN32)
        return()
    endif()
    set(dir "${CMAKE_BINARY_DIR}/_deps/gameinput-${BAND3_GAMEINPUT_VERSION}")
    set(header "${dir}/native/include/GameInput.h")
    set(library "${dir}/native/lib/x64/GameInput.lib")
    if(NOT EXISTS "${header}" OR NOT EXISTS "${library}")
        set(package "${dir}.nupkg")
        string(TOLOWER "${BAND3_GAMEINPUT_VERSION}" version)
        file(DOWNLOAD
            "https://api.nuget.org/v3-flatcontainer/microsoft.gameinput/${version}/microsoft.gameinput.${version}.nupkg"
            "${package}"
            EXPECTED_HASH SHA256=${BAND3_GAMEINPUT_SHA256}
            STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            list(GET status 1 message)
            file(REMOVE "${package}")
            message(WARNING "Couldn't download GameInput ${BAND3_GAMEINPUT_VERSION} (${message}), "
                            "so this build won't read Xbox One instruments")
            return()
        endif()
        # a .nupkg is a zip; only the header and the loader are needed
        file(ARCHIVE_EXTRACT INPUT "${package}" DESTINATION "${dir}"
             PATTERNS native/include/GameInput.h native/lib/x64/GameInput.lib)
        file(REMOVE "${package}")
    endif()
    target_include_directories(${target} SYSTEM PRIVATE "${dir}/native/include")
    target_link_libraries(${target} PRIVATE "${library}")
    target_compile_definitions(${target} PRIVATE BAND3_HAVE_GAMEINPUT)
endfunction()
