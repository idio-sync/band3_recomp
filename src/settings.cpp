#include "settings.h"
#include "renderer_default.h"
#include "Hooks/frame_pacing.h"
#include <atomic>
#include <mutex>
#include <string_view>

using rex::cvar::Lifecycle;

// the game crashes on start if its heaps add up to more than this
static constexpr int64_t kMaxHeapTotal = 0x40000000;
// mem.dta's sizes, which a heap size of 0 keeps
static constexpr int64_t kDefaultMainHeap = 105000000;
static constexpr int64_t kDefaultCharHeap = 32000000;

static bool ParseHeapSize(std::string_view v, int64_t& out) {
    double d;
    if (!rex::cvar::ParseDouble(v, d) || d < 0 || d > kMaxHeapTotal) return false;
    out = static_cast<int64_t>(d);
    return true;
}

static int64_t EffectiveHeap(int64_t size, int64_t default_size) {
    return size > 0 ? size : default_size;
}

// Band3/Game

REXCVAR_DEFINE_INT32(controller_type, 7, "Band3/Game",
    "Instrument gamepads and the keyboard play as: -1 = don't override, 1 = vocals, "
    "7 = guitar, 8 = drums. Instruments that report their own type (Xbox 360 instruments "
    "with input_backend = xinput, the virtual instrument) keep it")
    .range(-1, 255)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(rnd_sync, -1, "Band3/Game",
    "Vertical sync: -1 = don't override, 0 = off, 1 = on")
    .range(-1, 1)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(fast_start, false, "Band3/Game",
    "Skip the splash screens once the game has finished initializing")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(lang, "", "Band3/Game",
    "Force a language: eng, esl (Spanish), fre, ita or deu. Empty uses the default")
    .allowed({"", "eng", "esl", "fre", "ita", "deu"})
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(disable_metamusic, false, "Band3/Game",
    "Disable the background menu music")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(forced_venue, "false", "Band3/Game",
    "Venue to force, applied from the next venue load. false = don't force. "
    "A venue (arena_04), a class (arena, big_club, small_club, festival, video), "
    "none for a black background, or a comma separated list to pick from at random");

REXCVAR_DEFINE_STRING(username, "", "Band3/Game",
    "Override the username (up to 15 characters): the profile's gamertag wherever the game "
    "asks for it, online included. Empty keeps the profile's");

REXCVAR_DEFINE_INT32(main_heap_size, 0, "Band3/Game",
    "Main heap size in bytes, 0 = mem.dta's 105000000. Main and char together must stay "
    "under 0x40000000")
    .range(0, kMaxHeapTotal)
    .validator([](std::string_view v) {
        int64_t size;
        return ParseHeapSize(v, size) &&
               EffectiveHeap(size, kDefaultMainHeap) +
                   EffectiveHeap(REXCVAR_GET(char_heap_size), kDefaultCharHeap) <= kMaxHeapTotal;
    })
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(char_heap_size, 0, "Band3/Game",
    "Character heap size in bytes, 0 = mem.dta's 32000000. Main and char together must "
    "stay under 0x40000000")
    .range(0, kMaxHeapTotal)
    .validator([](std::string_view v) {
        int64_t size;
        return ParseHeapSize(v, size) &&
               EffectiveHeap(REXCVAR_GET(main_heap_size), kDefaultMainHeap) +
                   EffectiveHeap(size, kDefaultCharHeap) <= kMaxHeapTotal;
    })
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(hid_instruments, false, "Band3/Game",
    "Experimental: play PS3, Wii, PS4 and PS5 Rock Band guitars and drum kits (and the "
    "MIDI Pro Adapter in drum mode) through their USB dongles, as Xbox 360 instruments")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(menu_shortcut, true, "Band3/Game",
    "Open menus from a controller: hold both stick clicks for a second for this settings "
    "menu, or both stick clicks and the left bumper for the Instrument Lab");

REXCVAR_DEFINE_BOOL(steam_deck_defaults, true, "Band3/Game",
    "On a Steam Deck, start fullscreen and letterboxed with vsync on and the FPS counter "
    "off, unless band3.toml or the command line set those")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(joypad_lag, "", "Band3/Game",
    "Change the extra lag the game builds in per controller type, as type=ms, comma "
    "separated (e.g. 5=20,8=30). type=ms/video/audio also sets the calibration tests' lag; "
    "a blank part keeps the game's. The Instrument Lab's Lag tab shows each player's type")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(autosave, true, "Band3/Game",
    "Let the game autosave profiles, after songs and setlist edits. Off, they're only saved "
    "from the options menu, e.g. to test with autoplay without touching your profile");

REXCVAR_DEFINE_BOOL(skip_profile_prompt, true, "Band3/Game",
    "Players who join without a profile join as guests, without the game asking them to "
    "choose one (No Profile, Sign In, Swap to User). Off, it asks, as on a console");

REXCVAR_DEFINE_DOUBLE(song_speed, 1.0, "Band3/Game",
    "Plays songs faster or slower: 1.5 is half again as fast, 0.75 three quarters. "
    "Applies from the next song. A speed other than 1 that practice mode or Rock Band 3 "
    "Deluxe's song speed sets is left as it is")
    .range(0.1, 10.0);

REXCVAR_DEFINE_DOUBLE(track_speed, 1.0, "Band3/Game",
    "Scrolls the note highway faster or slower: 2 is twice as fast, with the notes "
    "twice as far apart. Applies from the next song")
    .range(0.1, 10.0);

REXCVAR_DEFINE_BOOL(unlock_clothing, false, "Band3/Game",
    "Unlock every piece of clothing, tattoo and face paint for your characters, and the "
    "video venues, without earning them (RB3Enhanced's UnlockClothing)");

REXCVAR_DEFINE_BOOL(gold_on_all_difficulties, false, "Band3/Game",
    "Let gold stars be earned on every difficulty, not only expert "
    "(RB3Enhanced's AllowGoldOnAllDifficulties). Applies from the next song");
REXCVAR_DEFINE_STRING(content_folders, "songs", "Band3/Game",
    "Folders RB3 reads DLC and custom songs from, without installing them, "
    "separated by '|'. Subfolders count too. A relative folder is relative to "
    "band3_config.ini's folder (or band3's own folder if there is no ini)")
    .lifecycle(Lifecycle::kRequiresRestart);

// Band3/MIDI drums

REXCVAR_DEFINE_BOOL(midi_drums, false, "Band3/MIDI drums",
    "Play a MIDI drum kit as a Rock Band pro drum kit, without a MIDI Pro Adapter")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(midi_drums_device, "", "Band3/MIDI drums",
    "MIDI input port to play, or part of its name. Empty uses the first one")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(midi_drums_notes, "", "Band3/MIDI drums",
    "Note overrides as note=Part, comma separated (e.g. 44=Kick,40=Snare), in RPCS3's "
    "format. Parts: Kick, HihatPedal, Snare, SnareRim, HiTom, LowTom, FloorTom, Hihat, "
    "Ride, Crash, None")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(midi_drums_pulse_ms, 30, "Band3/MIDI drums",
    "How long each hit is held, in milliseconds")
    .range(1, 100)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(midi_drums_min_velocity, 10, "Band3/MIDI drums",
    "Quieter hits than this (1-127) are ignored")
    .range(1, 127)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(midi_drums_combos, true, "Band3/MIDI drums",
    "Menu buttons from the kit: hi-hat pedal three times, then snare for Start, rim for "
    "Select, or kick to hold the kick (RB3's song category menu)")
    .lifecycle(Lifecycle::kRequiresRestart);

// Band3/Microphones

REXCVAR_DEFINE_BOOL(usb_mics, false, "Band3/Microphones",
    "Experimental: sing through microphones on this PC, as Xbox 360 USB microphones")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(usb_mic_devices, "", "Band3/Microphones",
    "Microphones to sing through, or part of their names, comma separated, one per mic "
    "slot (up to 4). Empty uses the system's default recording device")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(usb_mic_test_tone, 0, "Band3/Microphones",
    "Sing a steady tone at this pitch in Hz into the first mic slot instead of using "
    "microphones, to check that the game hears it. 0 = off")
    .range(0, 2000)
    .lifecycle(Lifecycle::kRequiresRestart);

// Band3/Graphics

REXCVAR_DEFINE_BOOL(disable_approximate_lights, true, "Band3/Graphics",
    "Disable approximate lighting; works around a graphical bug in current ReXGlue");

REXCVAR_DEFINE_BOOL(disable_hair_shader, false, "Band3/Graphics",
    "Don't use the hair shader variation on materials. Applies to materials loaded afterwards");

REXCVAR_DEFINE_BOOL(fullbright, false, "Band3/Graphics",
    "Force materials to not use an environ, making most things fullbright. "
    "Applies to materials loaded afterwards");

REXCVAR_DEFINE_BOOL(compress_character_textures, false, "Band3/Graphics",
    "Compress character textures. Needs readback resolve, or the textures appear bugged. "
    "Applies to characters loaded afterwards");

REXCVAR_DEFINE_BOOL(disable_even_odd_rendering, false, "Band3/Graphics",
    "Process every render command each frame instead of alternating even/odd frames");

REXCVAR_DEFINE_INT32(background_fps, 0, "Band3/Graphics",
    "The venue's frame rate under even/odd rendering: 0 = the venue's own (30 in most), "
    "whatever the game's refresh rate, or that many fps (at most the refresh rate). RB3 "
    "counts it as if the game ran at 60, so at refresh_rate 120 a venue's 30 drew at 60")
    .range(0, 240);

// src/Hooks/frame_pacing.h says what the cap does in place of the vblank
REXCVAR_DEFINE_STRING(frame_cap, "off", "Band3/Graphics",
    "What paces the game's frames. display: the display's refresh rate exactly "
    "(119.88 Hz, not 120), for fixed-refresh displays. auto: a little under it (5% less, at "
    "least 4 fps), for VRR displays (G-Sync, FreeSync), keeping each frame inside their range; "
    "on a fixed-refresh display a cap under the refresh rate shows a frame twice every "
    "1/(refresh - cap) seconds. A number of Hz (24 to 240), e.g. 117. off: the emulated "
    "console's vertical blank, paced by vsync and refresh_rate (the default). With the cap on, "
    "vsync is turned off and an unset refresh_rate follows the cap. Without a display whose "
    "rate can be told, display and auto are off")
    .validator([](std::string_view v) { return band3::pacing::ParseFrameCap(v).has_value(); });

// native on Windows, emulated elsewhere: renderer_default.h says why it's
// chosen at build time
REXCVAR_DEFINE_STRING(renderer, band3::settings::kDefaultRenderer, "Band3/Graphics",
    "What draws the game's picture: native, band3's own renderer drawing what the game "
    "sent the GPU, at the window's size, under the overlays, or emulated, the emulated Xbox "
    "360 GPU. Native is the default on Windows, emulated elsewhere; on Microsoft's software "
    "rasterizer (Windows without a GPU driver) native draws on the CPU. The emulated GPU "
    "keeps running either way, so it switches at once (F8)")
    .allowed({"emulated", "native"});

REXCVAR_DEFINE_INT32(native_max_height, 0, "Band3/Graphics",
    "With renderer = native, the most lines the native renderer draws: a window taller "
    "than this has its picture drawn this tall and scaled up to fill it, for 4K on a GPU "
    "that can't draw it at full size. 0 = the window's size")
    .range(0, 4320);

// read by src/Render/gpu_skip.cpp, by name
REXCVAR_DEFINE_STRING(emulated_gpu_while_native, "skip_draws", "Band3/Graphics",
    "With renderer = native, what the emulated GPU still does: skip_draws leaves out the "
    "game's draws nobody sees (the native renderer draws them), keeping what RB3 draws once "
    "(outfits) so F8 back shows the game's picture; full draws everything, as with "
    "renderer = emulated")
    .allowed({"full", "skip_draws"});

// Band3/Integrations

REXCVAR_DEFINE_BOOL(events_enabled, false, "Band3/Integrations",
    "Send RB3Enhanced-compatible events over UDP: Stage Kit lighting, song, band, venue "
    "and screen info, and RB3 Deluxe's rb3e_send_event_string data");

REXCVAR_DEFINE_STRING(events_target, "255.255.255.255", "Band3/Integrations",
    "Where events are sent. 255.255.255.255 broadcasts to the local network; if receivers "
    "see nothing, use your subnet's broadcast address or one device's IP")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(events_port, 21070, "Band3/Integrations",
    "UDP port events are sent to")
    .range(1, 65535)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(discord_enabled, false, "Band3/Integrations",
    "Show the current song as Discord Rich Presence (needs the Discord desktop app)")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(http_enabled, false, "Band3/Integrations",
    "Serve RB3Enhanced's web page and API to this PC and the local network: browse the "
    "song library from a phone and select songs in the Music Library")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(http_port, 21070, "Band3/Integrations",
    "TCP port the web server listens on (RB3Enhanced's is 21070)")
    .range(1, 65535)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(http_address, "0.0.0.0", "Band3/Integrations",
    "Address the web server listens on: 0.0.0.0 for the local network, 127.0.0.1 for this "
    "PC only")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(http_allow_cors, false, "Band3/Integrations",
    "Let web pages from other sites use the web server's API (Access-Control-Allow-Origin)");

REXCVAR_DEFINE_BOOL(http_allow_scripts, false, "Band3/Integrations",
    "Let the web server's /execute run DTA scripts sent to it. Anyone on the local network "
    "could then run any script in the game");

REXCVAR_DEFINE_BOOL(http_rhythmverse, true, "Band3/Integrations",
    "Let the web page search RhythmVerse (rhythmverse.co) for custom songs and download the "
    "ones it hosts into a rhythmverse folder in the first of content_folders. Downloaded "
    "songs join the game without a restart, as songs bought from the Xbox store did");

REXCVAR_DEFINE_BOOL(rb3e_mode, true, "Band3/Integrations",
    "Tell the game's scripts RB3Enhanced is running (they see RB3E and RB3E_HAS_VERSION "
    "defined), so Rock Band 3 Deluxe turns on its RB3E features: its version line, party "
    "mode, song lookups, and clearing the song cache and restarting after an update")
    .lifecycle(Lifecycle::kRequiresRestart);

// Band3/Launcher

REXCVAR_DEFINE_BOOL(show_launcher, true, "Band3/Launcher",
    "Show the launcher, band3's setup screen, before the game starts. Off, band3 starts "
    "the game straight away; hold Shift as it starts, or start it with --launcher, to see "
    "the launcher anyway")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(launcher, false, "Band3/Launcher",
    "Show the launcher at this start whatever show_launcher says. Meant for the command "
    "line (--launcher), e.g. in Steam's launch options, or the environment; a value in "
    "band3.toml is ignored, and it's cleared once read, so \"Save to config\" can't keep it");

// Band3/Online

REXCVAR_DEFINE_BOOL(gocentral, false, "Band3/Online",
    "Connect to GoCentral, the fan-run Rock Central server RB3Enhanced uses, for "
    "leaderboards, battles and setlist sharing. Needs username set: it's your account "
    "there, with no password, so pick one nobody else uses")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(gocentral_address, "gocentral-xbox.rbenhanced.rocks", "Band3/Online",
    "The GoCentral server to connect to: RB3Enhanced's Xbox 360 one, or your own")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(liveless, false, "Band3/Online",
    "Play online with other band3 and RB3Enhanced players without Xbox Live, as "
    "RB3Enhanced's Liveless does: searching for an online game joins liveless_connect's")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(liveless_connect, "127.0.0.1", "Band3/Online",
    "The game to join when searching online: its player's address, and :port if it isn't "
    "9103. 127.0.0.1 to host")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(liveless_external_ip, "", "Band3/Online",
    "This PC's address as players joining you reach it: your public IP for players over the "
    "internet. Empty uses this PC's address on the local network")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(liveless_port, 9103, "Band3/Online",
    "The UDP port this game plays online on, which players joining you need open. "
    "RB3Enhanced's is 9103; another (9203, say) lets a second band3 on this PC join the first")
    .range(1024, 65000)
    .lifecycle(Lifecycle::kRequiresRestart);

// Band3/Debug

REXCVAR_DEFINE_BOOL(debug_overlay, true, "Band3/Debug",
    "Show band3's FPS counter");

REXCVAR_DEFINE_BOOL(native_math, true, "Band3/Debug",
    "Use native C++ replacements for the game's math functions (sin, cos, pow, "
    "vector/matrix ops)");

REXCVAR_DEFINE_BOOL(native_camera_shake, true, "Band3/Debug",
    "Use the frame-rate independent camera shake replacement")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(log_shake_timing, false, "Band3/Debug",
    "Log the game's frame time against the wall clock once a second while the camera "
    "shake runs")
    .debug_only();

REXCVAR_DEFINE_BOOL(log_net_calls, false, "Band3/Debug",
    "Log each of the game's network calls (sockets, XNet) and what it returned");

REXCVAR_DEFINE_BOOL(autoplay, false, "Band3/Debug",
    "The game plays every part itself, from the next song start: for repeatable profiling "
    "runs and for checking a song without playing it");

REXCVAR_DEFINE_BOOL(virtual_instrument, false, "Band3/Debug",
    "Connect a virtual Xbox 360 instrument, played from the Instrument Lab (F6)");

REXCVAR_DEFINE_STRING(virtual_instrument_type, "guitar", "Band3/Debug",
    "Which instrument the virtual instrument is: guitar, drums, keys, pro_guitar_mustang "
    "or pro_guitar_squier. Changing it unplugs the instrument for a moment")
    .allowed({"guitar", "drums", "keys", "pro_guitar_mustang", "pro_guitar_squier"});

REXCVAR_DEFINE_INT32(virtual_instrument_player, 2, "Band3/Debug",
    "Player slot the virtual instrument connects as, 1-4. Other controllers keep their "
    "order around it")
    .range(1, 4);

REXCVAR_DEFINE_INT32(test_port, 0, "Band3/Debug",
    "Take test harness commands on this local TCP port (0 = off), for tools/band3ctl.py. "
    "Connects the virtual instrument as player 1")
    .range(0, 65535)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(test_random_seed, 0, "Band3/Debug",
    "Seed the game's random numbers with this instead of the clock (0 = off), so a fresh "
    "profile gets the same band on every launch, for render checks")
    .range(0, 2147483647)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(relaunch_wait_pid, 0, "Band3/Debug",
    "Set by band3 when it relaunches itself (rb3e_relaunch_game): the new one waits for "
    "this process to close before starting. Cleared once it has");

REXCVAR_DEFINE_STRING(native_view_backend, "gpu", "Band3/Debug",
    "What draws the native view (F9, experimental) and the native renderer: gpu, or cpu for "
    "the reference rasterizer. The GPU falls back to the CPU when it can't start")
    .allowed({"cpu", "gpu"});

REXCVAR_DEFINE_BOOL(native_present_zero_copy, true, "Band3/Debug",
    "With renderer = native, show the GPU's frames where they are, on the GPU (Direct3D 12, "
    "when band3's renderer shares the game's device); off reads each frame back and uploads "
    "it, the way other platforms do, to compare");

REXCVAR_DEFINE_BOOL(dred, false, "Band3/Debug",
    "Turn on Direct3D 12's Device Removed Extended Data at startup (Windows): if the GPU "
    "hangs, the crash trace says which command list and op it stopped at. Costs the GPU a "
    "small write per op; the test harness (band3ctl launch) turns it on");

REXCVAR_DEFINE_BOOL(native_present_pacing, true, "Band3/Debug",
    "With renderer = native, publish each frame to the window a steady delay after the game "
    "presented it (about the slowest recent frame's), so frames that draw quickly (with "
    "even/odd rendering, every other one) don't reach a paint together with the one before; "
    "off publishes each as soon as it's drawn, to compare");

REXCVAR_DEFINE_BOOL(native_view_record_targets, false, "Band3/Debug",
    "Record the passes RB3 draws into textures (outfits, the crowd, blurs) all the time, "
    "for the native view (experimental), even while it's off: some are drawn once, in the "
    "main menu, and a later capture needs them. Costs a little game-thread time while "
    "characters load; turn it on at launch (--native_view_record_targets=true)");

REXCVAR_DEFINE_BOOL(native_view_normal_maps, true, "Band3/Debug",
    "Shade RB3's normal maps and detail maps in the native view (experimental), live and in "
    "the test harness's captures; off shades those materials with the vertex normal, to "
    "compare");

REXCVAR_DEFINE_BOOL(native_view_texture_filtering, true, "Band3/Debug",
    "Sample textures in the native view (experimental) as the game's samplers do: filtered, "
    "between mip levels by distance, and clamped or wrapped as each says, live and in the "
    "test harness's captures; off reads every texture's nearest texel at full size, to "
    "compare");

REXCVAR_DEFINE_STRING(native_view_rt_fallback, "guest", "Band3/Debug",
    "What the native view's capture keeps of a texture RB3 draws at runtime (outfits, the "
    "crowd, blurs): guest also decodes what guest memory holds, right only with "
    "--readback_resolve=full; none keeps only which texture and version it is, for the "
    "texture passes the capture records")
    .allowed({"guest", "none"});

REXCVAR_DEFINE_BOOL(native_view_target_scale, true, "Band3/Debug",
    "Draw the native renderer's passes that are pictures of the screen (the spotlights' "
    "haze, the soft particles' smoke) in proportion to its picture: 1.5 times the game's "
    "size at 1080p, 3 times at 4K. Off keeps the game's sizes, made for 1280x720, to "
    "compare");

REXCVAR_DEFINE_INT32(native_view_shadow_scale, 1, "Band3/Debug",
    "Draw the characters' self-shadow maps this many times the game's 512x512 in the "
    "native renderer and the native view (experimental): sharper shadow edges. 1 = the "
    "game's")
    .range(1, 4);

REXCVAR_DEFINE_INT32(native_view_msaa, 2, "Band3/Graphics",
    "Samples a pixel the native renderer and the native view (experimental) draw the HUD, "
    "the track and the menus over the world with, averaged at their edges: 2 = the game's "
    "(RB3 multisamples them, not the world), 4 smoother, 1 none")
    .range(1, 4)
    .validator([](std::string_view v) { return v == "1" || v == "2" || v == "4"; });

REXCVAR_DEFINE_BOOL(native_view_capture_profile, false, "Band3/Debug",
    "Time each step of the native view's capture on the game's thread (geometry, textures, "
    "shade states, bones...) for the test harness's native_view stats, at a clock read per "
    "step; its hooks' totals are timed either way");

namespace band3::settings {

namespace {

// guest threads read these while F4 can reassign the cvars' strings on the UI
// thread, so they read copies that change callbacks keep current
struct TrackedString {
    std::mutex mutex;
    std::string value;

    void Set(std::string_view v) {
        std::lock_guard<std::mutex> lock(mutex);
        value.assign(v);
    }

    std::string Get() {
        std::lock_guard<std::mutex> lock(mutex);
        return value;
    }
};

TrackedString g_forced_venue;
std::atomic<double> g_song_speed{1.0};
std::atomic<double> g_track_speed{1.0};
TrackedString g_username;
StartupSettings g_startup{};

void Track(TrackedString& tracked, std::string_view name) {
    tracked.Set(rex::cvar::GetFlagByName(name));
    rex::cvar::RegisterChangeCallback(name, [&tracked](std::string_view, std::string_view v) {
        tracked.Set(v);
    });
}

}

void SnapshotStartupSettings() {
    g_startup = {
        .controller_type = REXCVAR_GET(controller_type),
        .rnd_sync = REXCVAR_GET(rnd_sync),
        .disable_metamusic = REXCVAR_GET(disable_metamusic),
        .main_heap_size = REXCVAR_GET(main_heap_size),
        .char_heap_size = REXCVAR_GET(char_heap_size),
        .events_target = REXCVAR_GET(events_target),
        .events_port = REXCVAR_GET(events_port),
        .discord_enabled = REXCVAR_GET(discord_enabled),
        .http_enabled = REXCVAR_GET(http_enabled),
        .http_port = REXCVAR_GET(http_port),
        .http_address = REXCVAR_GET(http_address),
        .rb3e_mode = REXCVAR_GET(rb3e_mode),
        .gocentral = REXCVAR_GET(gocentral),
        .gocentral_address = REXCVAR_GET(gocentral_address),
        .liveless = REXCVAR_GET(liveless),
        .liveless_connect = REXCVAR_GET(liveless_connect),
        .liveless_external_ip = REXCVAR_GET(liveless_external_ip),
        .liveless_port = REXCVAR_GET(liveless_port),
        .native_camera_shake = REXCVAR_GET(native_camera_shake),
    };
}

void Init() {
    Track(g_forced_venue, "forced_venue");
    g_song_speed = REXCVAR_GET(song_speed);
    g_track_speed = REXCVAR_GET(track_speed);
    rex::cvar::RegisterChangeCallback("song_speed", [](std::string_view, std::string_view) {
        g_song_speed = REXCVAR_GET(song_speed);
    });
    rex::cvar::RegisterChangeCallback("track_speed", [](std::string_view, std::string_view) {
        g_track_speed = REXCVAR_GET(track_speed);
    });
    Track(g_username, "username");
}

const StartupSettings& Startup() { return g_startup; }
std::string ForcedVenue() { return g_forced_venue.Get(); }
void SetSessionVenue(std::string_view venue) { g_forced_venue.Set(venue); }
double SongSpeed() { return g_song_speed; }
double TrackSpeed() { return g_track_speed; }
void SetSessionSongSpeed(double speed) { g_song_speed = speed; }
void SetSessionTrackSpeed(double speed) { g_track_speed = speed; }
std::string Username() { return g_username.Get(); }

}
