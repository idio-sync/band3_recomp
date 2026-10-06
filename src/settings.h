#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <rex/cvar.h>

// band3's settings, as rex cvars: the launcher and the in-game settings (F4)
// edit them and save the changed ones to band3.toml, and so does the SDK's
// settings menu ("All settings..."), whose "Save to config" writes every one
// that isn't its default. They can also be set with --name=value on the
// command line or in band3.toml; band3_config.ini fills in whatever those
// leave unset. Their categories (Band3/Game, ..., Band3/Advanced/...) follow
// the in-game settings' tabs.

// Band3/Game
REXCVAR_DECLARE(bool, fast_start);
REXCVAR_DECLARE(std::string, lang);
REXCVAR_DECLARE(bool, disable_metamusic);
REXCVAR_DECLARE(std::string, forced_venue);
REXCVAR_DECLARE(std::string, username);
REXCVAR_DECLARE(bool, steam_deck_defaults);
REXCVAR_DECLARE(bool, autosave);
REXCVAR_DECLARE(bool, skip_profile_prompt);
REXCVAR_DECLARE(double, song_speed);
REXCVAR_DECLARE(double, track_speed);
REXCVAR_DECLARE(bool, unlock_clothing);
REXCVAR_DECLARE(bool, gold_on_all_difficulties);
REXCVAR_DECLARE(bool, game_origin_icons);
REXCVAR_DECLARE(std::string, content_folders);
REXCVAR_DECLARE(bool, show_launcher);

// Band3/Graphics
REXCVAR_DECLARE(std::string, renderer);
REXCVAR_DECLARE(int32_t, rnd_sync);
REXCVAR_DECLARE(bool, disable_approximate_lights);
REXCVAR_DECLARE(bool, disable_hair_shader);
REXCVAR_DECLARE(bool, compress_character_textures);
REXCVAR_DECLARE(int32_t, background_fps);
REXCVAR_DECLARE(std::string, frame_cap);
REXCVAR_DECLARE(bool, debug_overlay);

// Band3/Graphics/Native
REXCVAR_DECLARE(int32_t, native_max_height);
REXCVAR_DECLARE(int32_t, native_anisotropic);
REXCVAR_DECLARE(int32_t, native_view_msaa);

// Band3/Audio
REXCVAR_DECLARE(bool, usb_mics);
REXCVAR_DECLARE(std::string, usb_mic_devices);

// Band3/Controllers
REXCVAR_DECLARE(int32_t, controller_type);
REXCVAR_DECLARE(bool, hid_instruments);
REXCVAR_DECLARE(bool, menu_shortcut);
REXCVAR_DECLARE(std::string, joypad_lag);

// Band3/Controllers/MIDI drums
REXCVAR_DECLARE(bool, midi_drums);
REXCVAR_DECLARE(std::string, midi_drums_device);
REXCVAR_DECLARE(std::string, midi_drums_notes);
REXCVAR_DECLARE(int32_t, midi_drums_pulse_ms);
REXCVAR_DECLARE(int32_t, midi_drums_min_velocity);
REXCVAR_DECLARE(bool, midi_drums_combos);

// Band3/Online
REXCVAR_DECLARE(bool, gocentral);
REXCVAR_DECLARE(std::string, gocentral_address);
REXCVAR_DECLARE(bool, liveless);
REXCVAR_DECLARE(std::string, liveless_connect);
REXCVAR_DECLARE(std::string, liveless_external_ip);
REXCVAR_DECLARE(int32_t, liveless_port);
REXCVAR_DECLARE(bool, liveless_port_mapping);
REXCVAR_DECLARE(bool, liveless_rooms);
REXCVAR_DECLARE(std::string, liveless_rooms_server);
REXCVAR_DECLARE(bool, events_enabled);
REXCVAR_DECLARE(std::string, events_target);
REXCVAR_DECLARE(int32_t, events_port);
REXCVAR_DECLARE(bool, discord_enabled);
REXCVAR_DECLARE(bool, http_enabled);
REXCVAR_DECLARE(int32_t, http_port);
REXCVAR_DECLARE(std::string, http_address);
REXCVAR_DECLARE(bool, http_allow_cors);
REXCVAR_DECLARE(bool, http_allow_scripts);
REXCVAR_DECLARE(bool, http_rhythmverse);
REXCVAR_DECLARE(bool, rb3e_mode);

// Band3/Advanced/Memory
REXCVAR_DECLARE(int32_t, main_heap_size);
REXCVAR_DECLARE(int32_t, char_heap_size);

// Band3/Advanced/Game code
REXCVAR_DECLARE(bool, native_math);
REXCVAR_DECLARE(bool, native_camera_shake);

// Band3/Advanced/Graphics
REXCVAR_DECLARE(bool, fullbright);
REXCVAR_DECLARE(bool, disable_even_odd_rendering);

// Band3/Advanced/Native renderer
REXCVAR_DECLARE(std::string, native_view_backend);
REXCVAR_DECLARE(bool, native_present_zero_copy);
REXCVAR_DECLARE(bool, native_present_pacing);
REXCVAR_DECLARE(bool, native_present_request_paint);
REXCVAR_DECLARE(bool, native_present_pipeline);
REXCVAR_DECLARE(int32_t, native_query_sample_count);
REXCVAR_DECLARE(bool, native_query_log);
REXCVAR_DECLARE(int32_t, native_sync_short_wait_us);
REXCVAR_DECLARE(bool, native_vblank_free_running);
REXCVAR_DECLARE(int32_t, native_slow_frame_ms);
REXCVAR_DECLARE(bool, native_view_record_targets);
REXCVAR_DECLARE(bool, native_view_normal_maps);
REXCVAR_DECLARE(bool, native_view_texture_filtering);
REXCVAR_DECLARE(std::string, native_view_rt_fallback);
REXCVAR_DECLARE(bool, native_view_target_scale);
REXCVAR_DECLARE(int32_t, native_view_shadow_scale);

// Band3/Advanced/Logging
REXCVAR_DECLARE(bool, log_shake_timing);
REXCVAR_DECLARE(bool, log_net_calls);
REXCVAR_DECLARE(int32_t, game_stall_log_ms);
REXCVAR_DECLARE(bool, dred);

// Band3/Advanced/Test harness
REXCVAR_DECLARE(int32_t, test_port);
REXCVAR_DECLARE(int32_t, test_random_seed);
REXCVAR_DECLARE(bool, autoplay);
REXCVAR_DECLARE(bool, virtual_instrument);
REXCVAR_DECLARE(std::string, virtual_instrument_type);
REXCVAR_DECLARE(int32_t, virtual_instrument_player);
REXCVAR_DECLARE(int32_t, usb_mic_test_tone);
REXCVAR_DECLARE(std::string, liveless_gateway);
REXCVAR_DECLARE(std::string, liveless_upnp_url);

// Band3/Advanced/Startup
REXCVAR_DECLARE(bool, launcher);
REXCVAR_DECLARE(int32_t, relaunch_wait_pid);

// Band3/Advanced/Retired
REXCVAR_DECLARE(std::string, emulated_gpu);

namespace band3::settings {

// Reads the retired emulated_gpu into renderer (src/Render/renderer_mode.h's
// MigrateEmulatedGpu) and clears it. Call once, after band3.toml, the
// environment and the command line are applied and before the runtime is
// configured (Band3App::OnPreSetup reads renderer).
void MigrateRendererSettings();

// Starts tracking the string settings guest threads read. Call once, after the
// config sources are applied and before the game runs.
void Init();

// Takes the Startup() snapshot from the current values: once at startup, and
// again when the launcher's Play starts the game with what was set in it.
void SnapshotStartupSettings();

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
    bool rb3e_mode;
    bool gocentral;
    std::string gocentral_address;
    bool liveless;
    std::string liveless_connect;
    std::string liveless_external_ip;
    int32_t liveless_port;
    bool liveless_port_mapping;
    bool liveless_rooms;
    std::string liveless_rooms_server;
    std::string liveless_gateway;
    std::string liveless_upnp_url;
    bool native_camera_shake;
};
const StartupSettings& Startup();

// Takes every setting's value (the SDK's and the GPU plugin's too) as the game
// starts with them, for StartupValue: Band3App calls it as the game starts,
// with the launcher's Play or without the launcher, once the GPU plugin has
// registered its settings.
void SnapshotStartupValues();
// A setting's value as of SnapshotStartupValues: what the game started with.
// nullopt for a name that wasn't registered then. UI thread.
std::optional<std::string> StartupValue(std::string_view name);

// The settings the game reads once, as it starts, though the registry has them
// HotReload: what Startup() keeps, the emulated GPU's swap_post_effect (read
// when the GPU is set up), input_backend (the input system is made once) and
// the folders. A change to one in game applies at the next start, as with a
// kRequiresRestart setting (the in-game settings say so).
bool ReadAtStartupOnly(std::string_view name);

// Copies of the string settings that can change in F4 while the game reads
// them; read these instead of REXCVAR_GET, which would race with the UI thread.
std::string ForcedVenue();

// Forces a venue for this session only, as RB3E's rb3e_set_venue does: the
// forced_venue setting (and band3.toml) keeps its value, and changing it in F4
// replaces this.
void SetSessionVenue(std::string_view venue);

// The song and track speed multipliers: song_speed and track_speed, or what a
// script set for this session with rb3e_change_music_speed and
// rb3e_change_track_speed (which, as on RB3E, aren't saved; changing the
// setting in F4 replaces them). Guest threads read these.
double SongSpeed();
double TrackSpeed();
void SetSessionSongSpeed(double speed);
void SetSessionTrackSpeed(double speed);
std::string Username();

}
