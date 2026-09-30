#include "settings.h"
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
    "Override the displayed username (up to 15 characters). Empty keeps the profile's");

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
TrackedString g_username;
StartupSettings g_startup{};

void Track(TrackedString& tracked, std::string_view name) {
    tracked.Set(rex::cvar::GetFlagByName(name));
    rex::cvar::RegisterChangeCallback(name, [&tracked](std::string_view, std::string_view v) {
        tracked.Set(v);
    });
}

}

void Init() {
    g_startup = {
        .controller_type = REXCVAR_GET(controller_type),
        .rnd_sync = REXCVAR_GET(rnd_sync),
        .disable_metamusic = REXCVAR_GET(disable_metamusic),
        .main_heap_size = REXCVAR_GET(main_heap_size),
        .char_heap_size = REXCVAR_GET(char_heap_size),
        .events_target = REXCVAR_GET(events_target),
        .events_port = REXCVAR_GET(events_port),
        .discord_enabled = REXCVAR_GET(discord_enabled),
        .native_camera_shake = REXCVAR_GET(native_camera_shake),
    };

    Track(g_forced_venue, "forced_venue");
    Track(g_username, "username");
}

const StartupSettings& Startup() { return g_startup; }
std::string ForcedVenue() { return g_forced_venue.Get(); }
std::string Username() { return g_username.Get(); }

}
