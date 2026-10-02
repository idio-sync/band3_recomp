#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <rex/cvar.h>

// band3's settings, as rex cvars: F4 edits them in game and "Save to config"
// writes band3.toml. They can also be set with --name=value on the command line
// or in band3.toml; band3_config.ini fills in whatever those leave unset.

// Band3/Game
REXCVAR_DECLARE(int32_t, controller_type);
REXCVAR_DECLARE(int32_t, rnd_sync);
REXCVAR_DECLARE(bool, fast_start);
REXCVAR_DECLARE(std::string, lang);
REXCVAR_DECLARE(bool, disable_metamusic);
REXCVAR_DECLARE(std::string, forced_venue);
REXCVAR_DECLARE(std::string, username);
REXCVAR_DECLARE(int32_t, main_heap_size);
REXCVAR_DECLARE(int32_t, char_heap_size);
REXCVAR_DECLARE(bool, hid_instruments);
REXCVAR_DECLARE(bool, menu_shortcut);
REXCVAR_DECLARE(bool, steam_deck_defaults);
REXCVAR_DECLARE(std::string, joypad_lag);
REXCVAR_DECLARE(bool, autosave);

// Band3/MIDI drums
REXCVAR_DECLARE(bool, midi_drums);
REXCVAR_DECLARE(std::string, midi_drums_device);
REXCVAR_DECLARE(std::string, midi_drums_notes);
REXCVAR_DECLARE(int32_t, midi_drums_pulse_ms);
REXCVAR_DECLARE(int32_t, midi_drums_min_velocity);
REXCVAR_DECLARE(bool, midi_drums_combos);

// Band3/Microphones
REXCVAR_DECLARE(bool, usb_mics);
REXCVAR_DECLARE(std::string, usb_mic_devices);
REXCVAR_DECLARE(int32_t, usb_mic_test_tone);

// Band3/Graphics
REXCVAR_DECLARE(bool, disable_approximate_lights);
REXCVAR_DECLARE(bool, disable_hair_shader);
REXCVAR_DECLARE(bool, fullbright);
REXCVAR_DECLARE(bool, compress_character_textures);
REXCVAR_DECLARE(bool, disable_even_odd_rendering);

// Band3/Integrations
REXCVAR_DECLARE(bool, events_enabled);
REXCVAR_DECLARE(std::string, events_target);
REXCVAR_DECLARE(int32_t, events_port);
REXCVAR_DECLARE(bool, discord_enabled);
REXCVAR_DECLARE(bool, http_enabled);
REXCVAR_DECLARE(int32_t, http_port);
REXCVAR_DECLARE(std::string, http_address);
REXCVAR_DECLARE(bool, http_allow_cors);
REXCVAR_DECLARE(bool, http_allow_scripts);

// Band3/Debug
REXCVAR_DECLARE(bool, debug_overlay);
REXCVAR_DECLARE(bool, native_math);
REXCVAR_DECLARE(bool, native_camera_shake);
REXCVAR_DECLARE(bool, log_shake_timing);
REXCVAR_DECLARE(bool, autoplay);
REXCVAR_DECLARE(bool, virtual_instrument);
REXCVAR_DECLARE(std::string, virtual_instrument_type);
REXCVAR_DECLARE(int32_t, virtual_instrument_player);
REXCVAR_DECLARE(int32_t, test_port);
REXCVAR_DECLARE(int32_t, test_random_seed);
REXCVAR_DECLARE(std::string, native_view_backend);
REXCVAR_DECLARE(bool, native_view_record_targets);
REXCVAR_DECLARE(std::string, native_view_rt_fallback);
REXCVAR_DECLARE(bool, native_view_normal_maps);

namespace band3::settings {

// Snapshots the restart settings and starts tracking the string settings guest
// threads read. Call once, after the config sources are applied and before the
// game runs.
void Init();

// The settings that need a restart, as they were when the game started. F4
// changes a cvar as soon as it is edited, so hooks that run after startup read
// these instead, keeping the change for the next launch.
struct StartupSettings {
    int32_t controller_type;
    int32_t rnd_sync;
    bool disable_metamusic;
    int32_t main_heap_size;
    int32_t char_heap_size;
    std::string events_target;
    int32_t events_port;
    bool discord_enabled;
    bool http_enabled;
    int32_t http_port;
    std::string http_address;
    bool native_camera_shake;
};
const StartupSettings& Startup();

// Copies of the string settings that can change in F4 while the game reads
// them; read these instead of REXCVAR_GET, which would race with the UI thread.
std::string ForcedVenue();

// Forces a venue for this session only, as RB3E's rb3e_set_venue does: the
// forced_venue setting (and band3.toml) keeps its value, and changing it in F4
// replaces this.
void SetSessionVenue(std::string_view venue);
std::string Username();

}
