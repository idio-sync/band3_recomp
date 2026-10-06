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

# The launcher's config writer needs toml++, but these tests don't need the
# ReXGlue SDK. Fetch the same header-only version shipped with the SDK.
include(FetchContent)
FetchContent_Declare(tomlplusplus
    URL https://codeload.github.com/marzer/tomlplusplus/tar.gz/refs/tags/v3.4.0
    URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
)
FetchContent_MakeAvailable(tomlplusplus)

add_executable(band3_tests
    main.cpp
    instruments_test.cpp
    gpu_residency_test.cpp
    camera_cut_test.cpp
    ps3_instruments_test.cpp
    ps4_instruments_test.cpp
    hid_capture_test.cpp
    midi_drums_test.cpp
    restart_debounce_test.cpp
    menu_shortcut_test.cpp
    mouse_menus_test.cpp
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
    present_model_test.cpp
    aspect_model_test.cpp
    http_request_test.cpp
    album_art_test.cpp
    paths_test.cpp
    loose_name_test.cpp
    package_scan_test.cpp
    json_test.cpp
    rhythmverse_test.cpp
    song_id_test.cpp
    online_test.cpp
    liveless_rooms_protocol_test.cpp
    liveless_rooms_client_test.cpp
    port_mapping_protocol_test.cpp
    native_socket_test.cpp
    frame_pacing_test.cpp
    launcher_start_test.cpp
    launcher_settings_test.cpp
    generated_rows_test.cpp
    settings_reference_test.cpp
    config_file_test.cpp
    relaunch_test.cpp
    instrument_decode_test.cpp
    device_lists_test.cpp
    input_backend_test.cpp
    device_view_test.cpp
    dred_report_test.cpp
    crash_report_test.cpp
    sync_cp_test.cpp
    sync_gpu_test.cpp
    renderer_mode_test.cpp
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
    ${BAND3_ROOT}/src/Input/mouse_menus.cpp
    ${BAND3_ROOT}/src/Input/ps3_instruments.cpp
    ${BAND3_ROOT}/src/Input/ps4_instruments.cpp
    ${BAND3_ROOT}/src/Launcher/config_file.cpp
    ${BAND3_ROOT}/src/Launcher/device_names.cpp
    ${BAND3_ROOT}/src/Launcher/device_view_model.cpp
    ${BAND3_ROOT}/src/Launcher/game_data_check.cpp
    ${BAND3_ROOT}/src/Launcher/generated_rows.cpp
    ${BAND3_ROOT}/src/Launcher/launcher_settings.cpp
    ${BAND3_ROOT}/src/Launcher/launcher_start.cpp
    ${BAND3_ROOT}/src/Launcher/settings_reference.cpp
    ${BAND3_ROOT}/src/Launcher/mic_level.cpp
    ${BAND3_ROOT}/src/Net/album_art.cpp
    ${BAND3_ROOT}/src/Net/liveless_rooms_client.cpp
    ${BAND3_ROOT}/src/Net/liveless_rooms_protocol.cpp
    ${BAND3_ROOT}/src/Net/native_socket.cpp
    ${BAND3_ROOT}/src/Net/online.cpp
    ${BAND3_ROOT}/src/Net/port_mapping_protocol.cpp
    ${BAND3_ROOT}/src/Net/http_request.cpp
    ${BAND3_ROOT}/src/Net/json.cpp
    ${BAND3_ROOT}/src/Net/rhythmverse.cpp
    ${BAND3_ROOT}/src/Net/sha1.cpp
    ${BAND3_ROOT}/src/paths.cpp
    ${BAND3_ROOT}/src/Render/capture_file.cpp
    ${BAND3_ROOT}/src/Render/frame_compose.cpp
    ${BAND3_ROOT}/src/Render/post_model.cpp
    ${BAND3_ROOT}/src/Render/shade_model.cpp
    ${BAND3_ROOT}/src/Render/soft_raster.cpp
    ${BAND3_ROOT}/src/Render/spot_model.cpp
    ${BAND3_ROOT}/src/Render/sync_gpu/sync_cp.cpp
    ${BAND3_ROOT}/src/Render/sync_gpu/sync_monitor.cpp
    ${BAND3_ROOT}/src/Test/game_state.cpp
    ${BAND3_ROOT}/src/Test/test_commands.cpp
    ${BAND3_ROOT}/src/Test/test_inputs.cpp
)
# instrument captures replayed by hid_capture_test.cpp, and the Liveless Rooms
# packets liveless_rooms_protocol_test.cpp shares with tools/liveless_rooms_mock.py (and
# port_mapping_protocol_test.cpp's with tools/port_mapping_mock.py); the repository,
# for settings_reference_test.cpp to check docs/settings-reference.md against
# src/settings.cpp
target_compile_definitions(band3_tests PRIVATE
    BAND3_CAPTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/captures"
    BAND3_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
    BAND3_ROOT_DIR="${BAND3_ROOT}"
)
# capture_file.cpp saves with fopen, as band3's own build lets it
set_source_files_properties(${BAND3_ROOT}/src/Render/capture_file.cpp PROPERTIES
    COMPILE_DEFINITIONS _CRT_SECURE_NO_WARNINGS
)
target_include_directories(band3_tests PRIVATE
    ${BAND3_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty
)
target_link_libraries(band3_tests PRIVATE tomlplusplus::tomlplusplus)

# src/Content/package_scan.cpp scans folders on threads of its own
find_package(Threads REQUIRED)
target_link_libraries(band3_tests PRIVATE Threads::Threads)
# src/Net/native_socket.cpp and liveless_rooms_client.cpp send and receive over Winsock
if(WIN32)
    target_link_libraries(band3_tests PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(band3_tests PRIVATE /W4)
else()
    target_compile_options(band3_tests PRIVATE -Wall -Wextra)
endif()

# band3.toml written by the launcher's writer, for tools/test_config_file.py
add_executable(band3_write_config
    write_config.cpp
    ${BAND3_ROOT}/src/Launcher/config_file.cpp
)
target_include_directories(band3_write_config PRIVATE ${BAND3_ROOT})
target_link_libraries(band3_write_config PRIVATE tomlplusplus::tomlplusplus)
if(MSVC)
    target_compile_options(band3_write_config PRIVATE /W4)
else()
    target_compile_options(band3_write_config PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME band3_tests COMMAND band3_tests)
