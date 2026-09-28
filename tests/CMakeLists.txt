# band3 unit tests: the code that doesn't need the ReXGlue SDK or the game
# (instrument encoding and similar pure logic). Builds on its own:
#
#   cmake -S tests -B out/tests
#   cmake --build out/tests
#   ctest --test-dir out/tests --output-on-failure

cmake_minimum_required(VERSION 3.25)
project(band3_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(BAND3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

add_executable(band3_tests
    main.cpp
    instruments_test.cpp
    ${BAND3_ROOT}/src/Input/instruments.cpp
)
target_include_directories(band3_tests PRIVATE
    ${BAND3_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty
)

if(MSVC)
    target_compile_options(band3_tests PRIVATE /W4)
else()
    target_compile_options(band3_tests PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME band3_tests COMMAND band3_tests)
