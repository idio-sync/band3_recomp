#include "settings.h"
#include "renderer_default.h"
#include "Hooks/frame_pacing.h"
#include "Render/renderer_mode.h"
#include <rex/logging.h>
#include <algorithm>
#include <atomic>
#include <iterator>
#include <map>
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

REXCVAR_DEFINE_BOOL(steam_deck_defaults, true, "Band3/Game",
    "On a Steam Deck, start fullscreen and letterboxed at the console's 60 Hz with vsync on "
    "and the FPS counter off, unless band3.toml or the command line set those")
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

REXCVAR_DEFINE_BOOL(game_origin_icons, true, "Band3/Game",
    "Show an icon in the song list for the game or pack each song came from "
    "(RB3Enhanced's GameOriginIcons). Needs Rock Band 3 Deluxe's icons and a song list "
    "with a game_origin_icon slot. Applies the next time the song list opens");

REXCVAR_DEFINE_STRING(content_folders, "songs", "Band3/Game",
    "Folders RB3 reads DLC and custom songs from, without installing them, "
    "separated by '|'. Subfolders count too. A relative folder is relative to "
    "band3_config.ini's folder (or band3's own folder if there is no ini)")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(show_launcher, true, "Band3/Game",
    "Show the launcher, band3's setup screen, before the game starts. Off, band3 starts "
    "the game straight away; hold Shift as it starts, or start it with --launcher, to see "
    "the launcher anyway")
    .lifecycle(Lifecycle::kRequiresRestart);

// Band3/Graphics

// native on Windows, emulated elsewhere: renderer_default.h says why it's
// chosen at build time. src/Render/renderer_mode.h has its rules. Not
// kRequiresRestart: emulated and both switch at once. First in Band3/Graphics,
// whose cvars F4 lists in the order they're defined: its description says
// which of the groups apply.
REXCVAR_DEFINE_STRING(renderer, band3::settings::kDefaultRenderer, "Band3/Graphics",
    "What draws the picture. native: band3's own renderer alone, no emulated Xbox 360 GPU "
    "(the default on Windows). emulated: the emulated GPU alone (the default elsewhere). "
    "both, Native + emulated (debug): the two, F8 switching between their pictures. "
    "Native to or from the others applies at the next start. Band3/Graphics/Native applies "
    "to native and both; Band3/Graphics/Emulated and the emulated GPU's own (under GPU, "
    "there only in a run with it) to emulated and both")
    .allowed({"native", "emulated", "both"});

REXCVAR_DEFINE_INT32(rnd_sync, -1, "Band3/Graphics",
    "Vertical sync: -1 = don't override, 0 = off, 1 = on")
    .range(-1, 1)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(disable_approximate_lights, true, "Band3/Graphics",
    "Disable approximate lighting; works around a graphical bug in current ReXGlue");

REXCVAR_DEFINE_BOOL(disable_hair_shader, false, "Band3/Graphics",
    "Don't use the hair shader variation on materials. Applies to materials loaded afterwards");

REXCVAR_DEFINE_BOOL(compress_character_textures, false, "Band3/Graphics",
    "Compress character textures. Needs the emulated GPU (renderer emulated or both): it's "
    "ignored with renderer native, where nothing composes the outfits to read back and they'd "
    "show black. Needs readback resolve, or the textures appear bugged. Applies to "
    "characters loaded afterwards");

REXCVAR_DEFINE_INT32(background_fps, 0, "Band3/Graphics",
    "The venue's frame rate under even/odd rendering: 0 = the venue's own (30 in most), "
    "whatever the game's refresh rate, or that many fps (at most the refresh rate). RB3 "
    "counts it as if the game ran at 60, so at refresh_rate 120 a venue's 30 drew at 60")
    .range(0, 240);

// src/Hooks/frame_pacing.h says what the cap does in place of the vblank
REXCVAR_DEFINE_STRING(frame_cap, "display", "Band3/Graphics",
    "What paces the game's frames. display: the display's refresh rate exactly (119.88 Hz, "
    "not 120), for fixed-refresh displays (the default). auto: a little under it (5% less, "
    "at least 4 fps), for VRR displays (G-Sync, FreeSync), keeping each frame inside their "
    "range; on a fixed-refresh display a cap under the refresh rate shows a frame twice "
    "every 1/(refresh - cap) seconds. A number of Hz (24 to 240), e.g. 117. off: the "
    "emulated console's vertical blank, paced by vsync and refresh_rate (60 unless set). "
    "With the cap on, vsync is turned off and an unset refresh_rate follows the cap. Without "
    "a display whose rate can be told, display and auto are off")
    .validator([](std::string_view v) { return band3::pacing::ParseFrameCap(v).has_value(); });

REXCVAR_DEFINE_BOOL(debug_overlay, true, "Band3/Graphics",
    "Show band3's FPS counter");

// Band3/Graphics/Native: the native renderer, with renderer native or both

REXCVAR_DEFINE_INT32(native_max_height, 0, "Band3/Graphics/Native",
    "Native renderer (renderer native or both): the most lines it draws: a window taller "
    "than this has its picture drawn this tall and scaled up to fill it, for 4K on a GPU "
    "that can't draw it at full size. 0 = the window's size")
    .range(0, 4320);

// read by src/Render/scene_capture.cpp (NativeAnisotropy, renderer_mode.h)
REXCVAR_DEFINE_INT32(native_anisotropic, -1, "Band3/Graphics/Native",
    "Native renderer (renderer native or both): the anisotropic filtering it samples "
    "textures with, as the emulated GPU's anisotropic_override counts it: 0 off, 1 = 1x, "
    "2 = 2x, 3 = 4x, 4 = 8x, 5 = 16x. -1 (the default) follows anisotropic_override where "
    "the emulated GPU runs (both), so the two pictures match, and keeps the game's own "
    "otherwise")
    .range(-1, 5);

REXCVAR_DEFINE_INT32(native_view_msaa, 2, "Band3/Graphics/Native",
    "Native renderer (renderer native or both) and the native view (experimental): the "
    "samples a pixel they draw the HUD, the track and the menus over the world with, "
    "averaged at their edges: 2 = the game's (RB3 multisamples them, not the world), 4 "
    "smoother, 1 none")
    .range(1, 4)
    .validator([](std::string_view v) { return v == "1" || v == "2" || v == "4"; });

// Band3/Graphics/Emulated: the emulated GPU, with renderer emulated or both. The emulated GPU's
// own settings (vsync, resolution_scale, ...) are the plugin's, in its categories.

// read by src/Render/gpu_skip.cpp, by name
REXCVAR_DEFINE_STRING(emulated_gpu_while_native, "skip_draws", "Band3/Graphics/Emulated",
    "Renderer both only: what the emulated GPU still does while the native picture shows: "
    "skip_draws leaves out the game's draws nobody sees (the native renderer draws them), "
    "keeping what RB3 draws once (outfits) so F8 back shows the game's picture; full draws "
    "everything, as with the emulated picture shown. swap_only also skips clears, resolves, "
    "the flares' occlusion-test quads and the passes RB3 draws once, leaving only what the "
    "game waits on: a test and performance mode, after which F8 back to emulated may show "
    "black outfits, portraits and other stale pictures until RB3 draws them again")
    .allowed({"full", "skip_draws", "swap_only"});

// Band3/Audio

REXCVAR_DEFINE_BOOL(usb_mics, false, "Band3/Audio",
    "Experimental: sing through microphones on this PC, as Xbox 360 USB microphones")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(usb_mic_devices, "", "Band3/Audio",
    "Microphones to sing through, or part of their names, comma separated, one per mic "
    "slot (up to 4). Empty uses the system's default recording device")
    .lifecycle(Lifecycle::kRequiresRestart);

// Band3/Controllers

REXCVAR_DEFINE_INT32(controller_type, 7, "Band3/Controllers",
    "Instrument gamepads and the keyboard play as: -1 = don't override, 1 = vocals, "
    "7 = guitar, 8 = drums. Instruments that report their own type (Xbox 360 instruments "
    "with input_backend = xinput, the virtual instrument) keep it")
    .range(-1, 255)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(hid_instruments, false, "Band3/Controllers",
    "Experimental: play PS3, Wii, PS4 and PS5 Rock Band guitars and drum kits (and the "
    "MIDI Pro Adapter in drum mode) through their USB dongles, as Xbox 360 instruments")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(menu_shortcut, true, "Band3/Controllers",
    "Open menus from a controller: hold both stick clicks for a second for this settings "
    "menu, or both stick clicks and the left bumper for the Instrument Lab");

REXCVAR_DEFINE_STRING(joypad_lag, "", "Band3/Controllers",
    "Change the extra lag the game builds in per controller type, as type=ms, comma "
    "separated (e.g. 5=20,8=30). type=ms/video/audio also sets the calibration tests' lag; "
    "a blank part keeps the game's. The Instrument Lab's Lag tab shows each player's type")
    .lifecycle(Lifecycle::kRequiresRestart);

// Band3/Controllers/MIDI drums

REXCVAR_DEFINE_BOOL(midi_drums, false, "Band3/Controllers/MIDI drums",
    "Play a MIDI drum kit as a Rock Band pro drum kit, without a MIDI Pro Adapter")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(midi_drums_device, "", "Band3/Controllers/MIDI drums",
    "MIDI input port to play, or part of its name. Empty uses the first one")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(midi_drums_notes, "", "Band3/Controllers/MIDI drums",
    "Note overrides as note=Part, comma separated (e.g. 44=Kick,40=Snare), in RPCS3's "
    "format. Parts: Kick, HihatPedal, Snare, SnareRim, HiTom, LowTom, FloorTom, Hihat, "
    "Ride, Crash, None")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(midi_drums_pulse_ms, 30, "Band3/Controllers/MIDI drums",
    "How long each hit is held, in milliseconds")
    .range(1, 100)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(midi_drums_min_velocity, 10, "Band3/Controllers/MIDI drums",
    "Quieter hits than this (1-127) are ignored")
    .range(1, 127)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(midi_drums_combos, true, "Band3/Controllers/MIDI drums",
    "Menu buttons from the kit: hi-hat pedal three times, then snare for Start, rim for "
    "Select, or kick to hold the kick (RB3's song category menu)")
    .lifecycle(Lifecycle::kRequiresRestart);

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

REXCVAR_DEFINE_BOOL(liveless_port_mapping, true, "Band3/Online",
    "Ask the router to forward liveless_port to this PC (PCP, NAT-PMP or UPnP), as "
    "RB3Enhanced does, so players over the internet reach you without a port forward made "
    "by hand. Deleted again when band3 closes")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(liveless_rooms, false, "Band3/Online",
    "Join friends by a room code (1-8 letters and digits) through a Liveless Rooms server "
    "(RB3Enhanced's), instead of typing an address. Needs liveless")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(liveless_rooms_server, "liveless-testing.ipg.pw", "Band3/Online",
    "The Liveless Rooms server's host name, as the server knows itself (host[:port], port "
    "19532 by default). Logging in registers your username there")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(events_enabled, false, "Band3/Online",
    "Send RB3Enhanced-compatible events over UDP: Stage Kit lighting, song, band, venue "
    "and screen info, and RB3 Deluxe's rb3e_send_event_string data");

REXCVAR_DEFINE_STRING(events_target, "255.255.255.255", "Band3/Online",
    "Where events are sent. 255.255.255.255 broadcasts to the local network; if receivers "
    "see nothing, use your subnet's broadcast address or one device's IP")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(events_port, 21070, "Band3/Online",
    "UDP port events are sent to")
    .range(1, 65535)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(discord_enabled, false, "Band3/Online",
    "Show the current song as Discord Rich Presence (needs the Discord desktop app)")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(http_enabled, false, "Band3/Online",
    "Serve RB3Enhanced's web page and API to this PC and the local network: browse the "
    "song library from a phone and select songs in the Music Library")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(http_port, 21070, "Band3/Online",
    "TCP port the web server listens on (RB3Enhanced's is 21070)")
    .range(1, 65535)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(http_address, "0.0.0.0", "Band3/Online",
    "Address the web server listens on: 0.0.0.0 for the local network, 127.0.0.1 for this "
    "PC only")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(http_allow_cors, false, "Band3/Online",
    "Let web pages from other sites use the web server's API (Access-Control-Allow-Origin)");

REXCVAR_DEFINE_BOOL(http_allow_scripts, false, "Band3/Online",
    "Let the web server's /execute run DTA scripts sent to it. Anyone on the local network "
    "could then run any script in the game");

REXCVAR_DEFINE_BOOL(http_rhythmverse, true, "Band3/Online",
    "Let the web page search RhythmVerse (rhythmverse.co) for custom songs and download the "
    "ones it hosts into a rhythmverse folder in the first of content_folders. Downloaded "
    "songs join the game without a restart, as songs bought from the Xbox store did");

REXCVAR_DEFINE_BOOL(rb3e_mode, true, "Band3/Online",
    "Tell the game's scripts RB3Enhanced is running (they see RB3E and RB3E_HAS_VERSION "
    "defined), so Rock Band 3 Deluxe turns on its RB3E features: its version line, party "
    "mode, song lookups, and clearing the song cache and restarting after an update")
    .lifecycle(Lifecycle::kRequiresRestart);

// Band3/Advanced: the in-game settings' Advanced tab, by subcategory

// Band3/Advanced/Memory

REXCVAR_DEFINE_INT32(main_heap_size, 0, "Band3/Advanced/Memory",
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

REXCVAR_DEFINE_INT32(char_heap_size, 0, "Band3/Advanced/Memory",
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

// Band3/Advanced/Game code

REXCVAR_DEFINE_BOOL(native_math, true, "Band3/Advanced/Game code",
    "Use native C++ replacements for the game's math functions (sin, cos, pow, "
    "vector/matrix ops)");

REXCVAR_DEFINE_BOOL(native_camera_shake, true, "Band3/Advanced/Game code",
    "Use the frame-rate independent camera shake replacement")
    .lifecycle(Lifecycle::kRequiresRestart);

// Band3/Advanced/Graphics

REXCVAR_DEFINE_BOOL(fullbright, false, "Band3/Advanced/Graphics",
    "Force materials to not use an environ, making most things fullbright. "
    "Applies to materials loaded afterwards");

REXCVAR_DEFINE_BOOL(disable_even_odd_rendering, false, "Band3/Advanced/Graphics",
    "Process every render command each frame instead of alternating even/odd frames");

// Band3/Advanced/Native renderer

REXCVAR_DEFINE_STRING(native_view_backend, "gpu", "Band3/Advanced/Native renderer",
    "What draws the native view (F9, experimental) and the native renderer: gpu, or cpu for "
    "the reference rasterizer. The GPU falls back to the CPU when it can't start")
    .allowed({"cpu", "gpu"});

REXCVAR_DEFINE_BOOL(native_present_zero_copy, true, "Band3/Advanced/Native renderer",
    "With the native picture shown (renderer native or both), show the GPU's frames where "
    "they are, on the GPU (Direct3D 12, when band3's renderer shares the game's device); off "
    "reads each frame back and uploads it, the way other platforms do, to compare");

REXCVAR_DEFINE_BOOL(native_present_pacing, true, "Band3/Advanced/Native renderer",
    "With the native picture shown, publish each frame to the window a steady delay after "
    "the game presented it (about the slowest recent frame's), so frames that draw quickly "
    "(with even/odd rendering, every other one) don't reach a paint together with the one "
    "before; off publishes each as soon as it's drawn, to compare");

REXCVAR_DEFINE_BOOL(native_present_request_paint, true, "Band3/Advanced/Native renderer",
    "With renderer native (no emulated GPU), ask the window to paint each time the native "
    "renderer has a new frame for it, as the emulated GPU's swaps did; off leaves the window "
    "to paint when something else asks, to compare");

REXCVAR_DEFINE_BOOL(native_present_pipeline, false, "Band3/Advanced/Native renderer",
    "With the native picture shown, on the zero-copy path, record the next frame while the "
    "GPU draws the one before, waiting for the GPU only to hand a frame to the window, so a "
    "frame costs the longer of its CPU and GPU time rather than both (for 120 Hz); off waits "
    "for each frame right after sending it. Keep it off: it hangs AMD GPUs and grows video "
    "memory steadily (about 10 MB a second at 120 Hz: the SDL_gpu allocations it makes "
    "aren't released on this path)");

REXCVAR_DEFINE_INT32(native_query_sample_count, 1000, "Band3/Advanced/Native renderer",
    "With renderer native (no emulated GPU), the samples every occlusion query reports as "
    "drawn (the lens flares' visibility tests), as the emulated GPU's "
    "query_occlusion_fake_sample_count does (1000, its default, what RB3 has always got "
    "here); -1 leaves the queries unanswered")
    .range(-1, 1000000);

REXCVAR_DEFINE_BOOL(native_query_log, false, "Band3/Advanced/Native renderer",
    "Log what the occlusion queries (the lens flares' visibility tests) give the game: the "
    "result of its first 50 reads of a query, then one a second at most, with either GPU; "
    "with renderer native also the counts the first 20 query packets found. Turning it on "
    "again logs as many more");

REXCVAR_DEFINE_INT32(native_sync_short_wait_us, 0, "Band3/Advanced/Native renderer",
    "With renderer native (no emulated GPU), how the GPU's command processor waits between "
    "polls of a wait whose interval is short (under 0x100): 0 yields and polls again at "
    "once, as the emulated GPU does; more sleeps that many microseconds instead")
    .range(0, 16000);

REXCVAR_DEFINE_BOOL(native_vblank_free_running, false, "Band3/Advanced/Native renderer",
    "With renderer native (no emulated GPU), raise the vertical blank every millisecond "
    "whatever the frame cap says, as the emulated GPU's vsync off does: with frame_cap off "
    "and rnd_sync 0 too, nothing paces the game (uncapped, to measure). Off, it runs at the "
    "game's refresh rate unless the frame cap paces the game");

REXCVAR_DEFINE_INT32(native_slow_frame_ms, 12, "Band3/Advanced/Native renderer",
    "With the native picture shown, log a line for each frame the native renderer takes longer "
    "than this many milliseconds to draw (its GPU wait included), with what kind of frame "
    "it was, where the time went and what it had to send or make; at most a few a second. "
    "0 = off")
    .range(0, 10000);

REXCVAR_DEFINE_BOOL(native_view_record_targets, false, "Band3/Advanced/Native renderer",
    "Record the passes RB3 draws into textures (outfits, the crowd, blurs) all the time, "
    "for the native view (experimental), even while it's off: some are drawn once, in the "
    "main menu, and a later capture needs them. Costs a little game-thread time while "
    "characters load; turn it on at launch (--native_view_record_targets=true)");

REXCVAR_DEFINE_BOOL(native_view_normal_maps, true, "Band3/Advanced/Native renderer",
    "Shade RB3's normal maps and detail maps in the native view (experimental), live and in "
    "the test harness's captures; off shades those materials with the vertex normal, to "
    "compare");

REXCVAR_DEFINE_BOOL(native_view_texture_filtering, true, "Band3/Advanced/Native renderer",
    "Sample textures in the native view (experimental) as the game's samplers do: filtered, "
    "between mip levels by distance, and clamped or wrapped as each says, live and in the "
    "test harness's captures; off reads every texture's nearest texel at full size, to "
    "compare");

REXCVAR_DEFINE_STRING(native_view_rt_fallback, "guest", "Band3/Advanced/Native renderer",
    "What the native view's capture keeps of a texture RB3 draws at runtime (outfits, the "
    "crowd, blurs): guest also decodes what guest memory holds, right only with "
    "--readback_resolve=full; none keeps only which texture and version it is, for the "
    "texture passes the capture records")
    .allowed({"guest", "none"});

REXCVAR_DEFINE_BOOL(native_view_target_scale, true, "Band3/Advanced/Native renderer",
    "Draw the native renderer's passes that are pictures of the screen (the spotlights' "
    "haze, the soft particles' smoke) in proportion to its picture: 1.5 times the game's "
    "size at 1080p, 3 times at 4K. Off keeps the game's sizes, made for 1280x720, to "
    "compare");

REXCVAR_DEFINE_INT32(native_view_shadow_scale, 1, "Band3/Advanced/Native renderer",
    "Draw the characters' self-shadow maps this many times the game's 512x512 in the "
    "native renderer and the native view (experimental): sharper shadow edges. 1 = the "
    "game's")
    .range(1, 4);

REXCVAR_DEFINE_BOOL(native_view_capture_profile, false, "Band3/Advanced/Native renderer",
    "Time each step of the native view's capture on the game's thread (geometry, textures, "
    "shade states, bones...) for the test harness's native_view stats, at a clock read per "
    "step; its hooks' totals are timed either way");

// Band3/Advanced/Logging

REXCVAR_DEFINE_BOOL(log_shake_timing, false, "Band3/Advanced/Logging",
    "Log the game's frame time against the wall clock once a second while the camera "
    "shake runs")
    .debug_only();

REXCVAR_DEFINE_BOOL(log_net_calls, false, "Band3/Advanced/Logging",
    "Log each of the game's network calls (sockets, XNet) and what it returned");

REXCVAR_DEFINE_INT32(game_stall_log_ms, 100, "Band3/Advanced/Logging",
    "Log each of the game's frames that takes longer than this many milliseconds, with "
    "samples of the game thread's stack and what else was going on (src/stall_watch.h); at "
    "most one every two seconds. 0 = off")
    .range(0, 100000);

REXCVAR_DEFINE_BOOL(dred, false, "Band3/Advanced/Logging",
    "Turn on Direct3D 12's Device Removed Extended Data at startup (Windows): if the GPU "
    "hangs, the crash trace says which command list and op it stopped at. Costs the GPU a "
    "small write per op; the test harness (band3ctl launch) turns it on");

// Band3/Advanced/Test harness

REXCVAR_DEFINE_INT32(test_port, 0, "Band3/Advanced/Test harness",
    "Take test harness commands on this local TCP port (0 = off), for tools/band3ctl.py. "
    "Connects the virtual instrument as player 1")
    .range(0, 65535)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(test_random_seed, 0, "Band3/Advanced/Test harness",
    "Seed the game's random numbers with this instead of the clock (0 = off), so a fresh "
    "profile gets the same band on every launch, for render checks")
    .range(0, 2147483647)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(autoplay, false, "Band3/Advanced/Test harness",
    "The game plays every part itself, from the next song start: for repeatable profiling "
    "runs and for checking a song without playing it");

REXCVAR_DEFINE_BOOL(virtual_instrument, false, "Band3/Advanced/Test harness",
    "Connect a virtual Xbox 360 instrument, played from the Instrument Lab (F6)");

REXCVAR_DEFINE_STRING(virtual_instrument_type, "guitar", "Band3/Advanced/Test harness",
    "Which instrument the virtual instrument is: guitar, drums, keys, pro_guitar_mustang "
    "or pro_guitar_squier. Changing it unplugs the instrument for a moment")
    .allowed({"guitar", "drums", "keys", "pro_guitar_mustang", "pro_guitar_squier"});

REXCVAR_DEFINE_INT32(virtual_instrument_player, 2, "Band3/Advanced/Test harness",
    "Player slot the virtual instrument connects as, 1-4. Other controllers keep their "
    "order around it")
    .range(1, 4);

REXCVAR_DEFINE_INT32(usb_mic_test_tone, 0, "Band3/Advanced/Test harness",
    "Sing a steady tone at this pitch in Hz into the first mic slot instead of using "
    "microphones, to check that the game hears it. 0 = off")
    .range(0, 2000)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(liveless_gateway, "", "Band3/Advanced/Test harness",
    "Where liveless_port_mapping sends PCP and NAT-PMP (host[:port], port 5351 by default) "
    "instead of the router, for testing against a stand-in. Empty asks the default gateway; "
    "under the test harness, empty skips PCP and NAT-PMP")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(liveless_upnp_url, "", "Band3/Advanced/Test harness",
    "The UPnP router description liveless_port_mapping uses (http://host:port/desc.xml) "
    "instead of looking for one on the network, for testing against a stand-in. Under the "
    "test harness, empty skips UPnP")
    .lifecycle(Lifecycle::kRequiresRestart);

// Band3/Advanced/Startup: read once as band3 starts, then cleared by band3
// itself. kInitOnly has the settings menus show them read-only; the SDK refuses
// SetFlagByName on them only once rex::cvar::FinalizeInit has run, which
// nothing calls (rexruntime.dll, 2026-10), so band3's own clearing still works.

REXCVAR_DEFINE_BOOL(launcher, false, "Band3/Advanced/Startup",
    "Show the launcher at this start whatever show_launcher says. Meant for the command "
    "line (--launcher), e.g. in Steam's launch options, or the environment; a value in "
    "band3.toml is ignored, and it's cleared once read, so \"Save to config\" can't keep it")
    .lifecycle(Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(relaunch_wait_pid, 0, "Band3/Advanced/Startup",
    "Set by band3 when it relaunches itself (rb3e_relaunch_game): the new one waits for "
    "this process to close before starting. Cleared once it has")
    .lifecycle(Lifecycle::kInitOnly);

// Band3/Advanced/Retired

// Retired: renderer took its place. Still read, once, so an old band3.toml or
// command line keeps working (Band3App::OnPostInitLogging,
// MigrateRendererSettings below); cleared once read, so neither save writes it
// again (the launcher removes its key, F4 writes only what isn't the default).
REXCVAR_DEFINE_STRING(emulated_gpu, "", "Band3/Advanced/Retired",
    "Retired: set renderer instead. Read at startup for one more release: off is renderer "
    "native, on with renderer native is renderer both, on with renderer emulated stays "
    "emulated (a renderer set on the command line, or already both, wins). Then cleared")
    .allowed({"", "on", "off"})
    .lifecycle(Lifecycle::kRequiresRestart);

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
// every setting's value as the game started (SnapshotStartupValues)
std::map<std::string, std::string, std::less<>> g_startup_values;

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
        .liveless_port_mapping = REXCVAR_GET(liveless_port_mapping),
        .liveless_rooms = REXCVAR_GET(liveless_rooms),
        .liveless_rooms_server = REXCVAR_GET(liveless_rooms_server),
        .liveless_gateway = REXCVAR_GET(liveless_gateway),
        .liveless_upnp_url = REXCVAR_GET(liveless_upnp_url),
        .native_camera_shake = REXCVAR_GET(native_camera_shake),
    };
}

void SnapshotStartupValues() {
    g_startup_values.clear();
    for (const rex::cvar::FlagEntry& entry : rex::cvar::GetRegistry()) {
        if (entry.type == rex::cvar::FlagType::Command || !entry.getter) continue;
        g_startup_values[entry.name] = entry.getter();
    }
}

std::optional<std::string> StartupValue(std::string_view name) {
    const auto it = g_startup_values.find(name);
    if (it == g_startup_values.end()) return std::nullopt;
    return it->second;
}

bool ReadAtStartupOnly(std::string_view name) {
    static constexpr std::string_view kNames[] = {
        // SnapshotStartupSettings' (most are kRequiresRestart too)
        "controller_type", "rnd_sync", "disable_metamusic", "main_heap_size", "char_heap_size",
        "events_target", "events_port", "discord_enabled", "http_enabled", "http_port",
        "http_address", "rb3e_mode", "gocentral", "gocentral_address", "liveless",
        "liveless_connect", "liveless_external_ip", "liveless_port", "liveless_port_mapping",
        "liveless_rooms", "liveless_rooms_server", "liveless_gateway", "liveless_upnp_url",
        "native_camera_shake",
        // the emulated GPU reads FXAA once, as it's set up (GraphicsSystem::SetupGuestGpu;
        // see Band3App::StartFromLauncher)
        "swap_post_effect",
        // the input system is made once (input_system.h)
        "input_backend",
        // the folders the runtime was built with (Band3App::OnFinalizePaths)
        "game_data_root", "user_data_root", "cache_root", "content_folders",
    };
    return std::ranges::find(kNames, name) != std::end(kNames);
}

namespace {

render::SettingSource SourceOf(std::string_view name) {
    switch (rex::cvar::GetFlagSource(name)) {
    case rex::cvar::Source::kDefault: return render::SettingSource::kUnset;
    case rex::cvar::Source::kEnvironment: return render::SettingSource::kEnvironment;
    case rex::cvar::Source::kCommandLine: return render::SettingSource::kCommandLine;
    // nothing but the command line, the environment and band3.toml has set
    // anything this early
    default: return render::SettingSource::kConfig;
    }
}

}

void MigrateRendererSettings() {
    using render::SettingSource;
    const render::OldSetting emulated_gpu{REXCVAR_GET(emulated_gpu), SourceOf("emulated_gpu")};
    const render::OldSetting renderer{REXCVAR_GET(renderer), SourceOf("renderer")};
    const render::RendererMigration m = render::MigrateEmulatedGpu(emulated_gpu, renderer);
    if (!m.log.empty()) REXLOG_INFO("settings: {}", m.log);
    if (m.renderer) {
        // from the command line (or the environment) it's as fixed as if
        // renderer had been set there: the launcher shows it locked and
        // doesn't save it
        const bool fixed =
            m.source == SettingSource::kCommandLine || m.source == SettingSource::kEnvironment;
        const bool ok = fixed ? rex::cvar::SetFlagFromCommandLine("renderer", *m.renderer)
                              : rex::cvar::SetFlagByName("renderer", *m.renderer);
        if (!ok) REXLOG_WARN("settings: couldn't set renderer to {}", *m.renderer);
    }
    // read once: the launcher's save then removes its key from band3.toml,
    // and F4's leaves it out (it writes only what isn't the default)
    // (a restart-only setting, which the SDK would otherwise list as waiting on
    // one, as band3's startup does for audio_maxqframes)
    if (emulated_gpu.source != SettingSource::kUnset) {
        rex::cvar::ResetToDefault("emulated_gpu");
        rex::cvar::ClearPendingRestartFlags();
    }
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
