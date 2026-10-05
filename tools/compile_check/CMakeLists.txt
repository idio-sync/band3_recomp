# Compiles band3's own sources (src/) against the ReXGlue SDK without running
# codegen, so it needs neither the game's default.xex nor its assets. A
# stand-in for generated/band3_init.h is written at configure time. Nothing is
# linked; this only proves the sources compile.
#
#   cmake -S tools/compile_check -B out/compile_check -G Ninja \
#         -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
#         -DCMAKE_PREFIX_PATH=<path to the rexglue SDK>
#   cmake --build out/compile_check
#
# Pass the build type: Windows toolchains default an unset one to Debug, which
# also turns on the SDK's profiling (Tracy) definitions.

cmake_minimum_required(VERSION 3.25)
project(band3_compile_check LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(BAND3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

find_package(rexglue CONFIG REQUIRED)

# the same sources as the band3 target
file(GLOB_RECURSE BAND3_SOURCES CONFIGURE_DEPENDS
    "${BAND3_ROOT}/src/*.cpp" "${BAND3_ROOT}/src/*.c")

# every recompiled function the sources call as name(ctx, base), leaving out
# member calls and the functions src/ declares or defines itself (hooks, their
# __imp__ originals, local helpers)
file(GLOB_RECURSE BAND3_HEADERS CONFIGURE_DEPENDS "${BAND3_ROOT}/src/*.h")
set(guest_functions "")
set(own_functions "")
foreach(source ${BAND3_SOURCES} ${BAND3_HEADERS})
    file(READ "${source}" contents)
    string(REGEX MATCHALL "[^A-Za-z0-9_.>][A-Za-z_][A-Za-z0-9_]*\\( *ctx *, *base *\\)"
        calls "${contents}")
    foreach(call ${calls})
        string(REGEX REPLACE "^.([A-Za-z0-9_]+)\\(.*" "\\1" name "${call}")
        list(APPEND guest_functions "${name}")
    endforeach()
    string(REGEX MATCHALL "[A-Za-z_][A-Za-z0-9_]*\\( *(const +)?PPCContext" declared "${contents}")
    string(REGEX MATCHALL "REX_[A-Z_]*\\( *[A-Za-z_][A-Za-z0-9_]* *\\)" hooked "${contents}")
    foreach(match ${declared} ${hooked})
        string(REGEX REPLACE "^(REX_[A-Z_]*\\( *)?([A-Za-z0-9_]+).*" "\\2" name "${match}")
        list(APPEND own_functions "${name}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES guest_functions)
if(own_functions)
    list(REMOVE_ITEM guest_functions ${own_functions})
endif()
list(SORT guest_functions)

set(BAND3_GUEST_FUNCTION_DECLS "")
foreach(name ${guest_functions})
    string(APPEND BAND3_GUEST_FUNCTION_DECLS "REX_EXTERN(${name});\n")
endforeach()

set(stub_dir "${CMAKE_CURRENT_BINARY_DIR}/stub")
configure_file(band3_init.h.in "${stub_dir}/generated/band3_init.h" @ONLY)
list(LENGTH guest_functions guest_function_count)
message(STATUS "band3 compile check: ${guest_function_count} recompiled functions declared")

add_library(band3_compile_check OBJECT ${BAND3_SOURCES})
# the stand-in comes first, so it wins over a real generated/ from a local codegen run
target_include_directories(band3_compile_check PRIVATE
    "${stub_dir}"
    "${BAND3_ROOT}"
    "${BAND3_ROOT}/src"
)
target_link_libraries(band3_compile_check PRIVATE rex::runtime)
# as the band3 target defines it, for src/sdk_export.cpp
target_compile_definitions(band3_compile_check PRIVATE
    BAND3_REXRUNTIME_DLL="$<TARGET_FILE_NAME:rex::runtime>")

# the same MIDI backend as the real build
include(${BAND3_ROOT}/cmake/rtmidi.cmake)
band3_setup_rtmidi(band3_compile_check ${BAND3_ROOT}/src/ThirdParty/rtmidi/RtMidi.cpp)

# the same HTTPS client as the real build
include(${BAND3_ROOT}/cmake/web_client.cmake)
band3_setup_web_client(band3_compile_check)

# the same miniupnpc definitions and headers, for src/Net/port_mapping.cpp
include(${BAND3_ROOT}/cmake/miniupnpc.cmake)
band3_setup_miniupnpc(band3_compile_check ${BAND3_ROOT}/src/ThirdParty/miniupnpc)

# the same build tag header, for src/build_tag.cpp
include(${BAND3_ROOT}/cmake/build_tag.cmake)
band3_setup_build_tag(band3_compile_check ${BAND3_ROOT})
