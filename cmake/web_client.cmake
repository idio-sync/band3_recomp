# src/Net/web_client.cpp asks other sites for things over HTTPS (RhythmVerse's
# search and downloads): through WinHTTP on Windows, libcurl elsewhere. Without
# libcurl's development files, every request fails, saying so.
function(band3_setup_web_client target)
    if(WIN32)
        target_link_libraries(${target} PRIVATE winhttp)
    else()
        find_package(CURL)
        if(CURL_FOUND)
            target_link_libraries(${target} PRIVATE CURL::libcurl)
            target_compile_definitions(${target} PRIVATE BAND3_HAVE_CURL)
        else()
            message(WARNING "libcurl development files not found, so RhythmVerse's search and "
                            "downloads won't work (install libcurl4-openssl-dev or libcurl-devel)")
        endif()
    endif()
endfunction()
