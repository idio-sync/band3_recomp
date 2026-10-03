# miniupnpc (src/ThirdParty/miniupnpc, 2.3.3) asks a UPnP router to forward
# liveless_port to this PC (src/Net/port_mapping.cpp). Its library sources are
# in src/, which the band3 target's glob already takes in; this gives them the
# definitions miniupnpc's own CMakeLists.txt would, and the target its headers.
#
# Call from the directory that adds the sources to target, since source file
# properties are set per directory.
function(band3_setup_miniupnpc target miniupnpc_dir)
    file(GLOB miniupnpc_sources "${miniupnpc_dir}/src/*.c")
    if(WIN32)
        # upstream leaves its socket timeouts off on Windows: SO_RCVTIMEO takes
        # milliseconds there, not the struct timeval it would pass, and it
        # waits with select() instead
        set(definitions _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
    else()
        set(definitions MINIUPNPC_SET_SOCKET_TIMEOUT MINIUPNPC_GET_SRC_ADDR _DEFAULT_SOURCE)
    endif()
    set_source_files_properties(${miniupnpc_sources} PROPERTIES COMPILE_DEFINITIONS "${definitions}")
    if(WIN32 AND CMAKE_C_COMPILER_ID MATCHES "Clang")
        # its sources define WIN32_LEAN_AND_MEAN again after the SDK's -D
        set_source_files_properties(${miniupnpc_sources} PROPERTIES COMPILE_OPTIONS -Wno-macro-redefined)
    endif()
    # static, not a DLL: without it the Windows headers mark every function dllimport
    target_compile_definitions(${target} PRIVATE MINIUPNP_STATICLIB)
    target_include_directories(${target} PRIVATE "${miniupnpc_dir}/include")
    if(WIN32)
        # GetBestRoute (src/Net/gateway_address.cpp) and miniupnpc's own
        # interface lookups
        target_link_libraries(${target} PRIVATE ws2_32 iphlpapi)
    endif()
endfunction()
