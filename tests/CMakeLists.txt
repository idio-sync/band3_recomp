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
    ps3_instruments_test.cpp
    ps4_instruments_test.cpp
    hid_capture_test.cpp
    midi_drums_test.cpp
    menu_shortcut_test.cpp
    usb_mic_test.cpp
    game_status_test.cpp
    player_slots_test.cpp
    test_commands_test.cpp
    test_inputs_test.cpp
    capture_file_test.cpp
    shade_model_test.cpp
    soft_raster_test.cpp
    sample_model_test.cpp
    frame_compose_test.cpp
    post_params_test.cpp
    post_model_test.cpp
    spot_model_test.cpp
    http_request_test.cpp
    album_art_test.cpp
    paths_test.cpp
    package_scan_test.cpp
    song_id_test.cpp
    ${BAND3_ROOT}/src/Audio/mic_mapping_status.cpp
    ${BAND3_ROOT}/src/Audio/usb_mic.cpp
    ${BAND3_ROOT}/src/Content/package_scan.cpp
    ${BAND3_ROOT}/src/Game/song_id.cpp
    ${BAND3_ROOT}/src/Input/hid_capture.cpp
    ${BAND3_ROOT}/src/Input/hid_instrument_types.cpp
    ${BAND3_ROOT}/src/Input/instrument_kind.cpp
    ${BAND3_ROOT}/src/Input/instruments.cpp
    ${BAND3_ROOT}/src/Input/joypad_lag_status.cpp
    ${BAND3_ROOT}/src/Input/menu_shortcut.cpp
    ${BAND3_ROOT}/src/Input/midi_drums.cpp
    ${BAND3_ROOT}/src/Input/player_slots.cpp
    ${BAND3_ROOT}/src/Input/ps3_instruments.cpp
    ${BAND3_ROOT}/src/Input/ps4_instruments.cpp
    ${BAND3_ROOT}/src/Net/album_art.cpp
    ${BAND3_ROOT}/src/Net/http_request.cpp
    ${BAND3_ROOT}/src/paths.cpp
    ${BAND3_ROOT}/src/Render/capture_file.cpp
    ${BAND3_ROOT}/src/Render/frame_compose.cpp
    ${BAND3_ROOT}/src/Render/post_model.cpp
    ${BAND3_ROOT}/src/Render/shade_model.cpp
    ${BAND3_ROOT}/src/Render/soft_raster.cpp
    ${BAND3_ROOT}/src/Render/spot_model.cpp
    ${BAND3_ROOT}/src/Test/game_state.cpp
    ${BAND3_ROOT}/src/Test/test_commands.cpp
    ${BAND3_ROOT}/src/Test/test_inputs.cpp
)
# instrument captures replayed by hid_capture_test.cpp
target_compile_definitions(band3_tests PRIVATE
    BAND3_CAPTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/captures"
)
# capture_file.cpp saves with fopen, as band3's own build lets it
set_source_files_properties(${BAND3_ROOT}/src/Render/capture_file.cpp PROPERTIES
    COMPILE_DEFINITIONS _CRT_SECURE_NO_WARNINGS
)
target_include_directories(band3_tests PRIVATE
    ${BAND3_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty
)

# src/Content/package_scan.cpp scans folders on threads of its own
find_package(Threads REQUIRED)
target_link_libraries(band3_tests PRIVATE Threads::Threads)

if(MSVC)
    target_compile_options(band3_tests PRIVATE /W4)
else()
    target_compile_options(band3_tests PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME band3_tests COMMAND band3_tests)
