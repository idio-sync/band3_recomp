#include "launcher_settings.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include "src/Input/joypad_lag_status.h"
#include "src/Net/online.h"
#include "src/Render/renderer_mode.h"
#include "src/paths.h"

namespace band3::launcher {

namespace {

// the footer's startup box (ShowAtStartup)
constexpr std::string_view kShowLauncher = "show_launcher";

// Choices

constexpr Choice kLanguages[] = {
    {"", "Game's default"}, {"eng", "English"}, {"esl", "Spanish"},
    {"fre", "French"},      {"ita", "Italian"}, {"deu", "German"},
};

// disable_metamusic, the other way round
constexpr Choice kMenuMusic[] = {{"false", "On"}, {"true", "Off"}};

constexpr Choice kVideoFits[] = {
    {"fit", "Fit, with black bars"},
    {"fill", "Fill, edges cut off"},
    {"stretch", "Stretch"},
};

constexpr Choice kVenues[] = {
    {"false", "Don't force"},
    {"arena", "Arena"},
    {"big_club", "Big club"},
    {"small_club", "Small club"},
    {"festival", "Festival"},
    {"video", "Video venue"},
    {"none", "None (black background)"},
};

// where the monitors can't be listed (off Windows); named from the list otherwise
constexpr Choice kMonitors[] = {
    {"0", "Default"},   {"1", "Primary"},   {"2", "Monitor 2"},
    {"3", "Monitor 3"}, {"4", "Monitor 4"},
};

constexpr Choice kResolutions[] = {
    {"", "Default"},
    {"720p", "1280 x 720"},
    {"1080p", "1920 x 1080"},
    {"1440p", "2560 x 1440"},
    {"4k", "3840 x 2160"},
};

constexpr Choice kAspect[] = {{"true", "Letterbox"}, {"false", "Stretch"}};

// renderer_mode.h's RendererLabel. Native is offered on Windows only until
// its Vulkan path (sync_graphics_system.h) has run on Linux
constexpr Choice kRenderers[] = {
    {"native", "Native", true},
    {"emulated", "Emulated"},
    {"both", "Native + emulated (debug)"},
};

constexpr Choice kNativeMsaa[] = {{"1", "Off"}, {"2", "2x (the game's)"}, {"4", "4x"}};

// native_max_height's: the most lines drawn, a taller window's picture scaled up
constexpr Choice kNativeHeights[] = {
    {"0", "None (the window's size)"},
    {"720", "720p"},
    {"1080", "1080p"},
    {"1440", "1440p"},
    {"2160", "4K"},
};

constexpr Choice kEmulatedWhileNative[] = {
    {"skip_draws", "Skip what the native picture draws"},
    {"full", "Everything"},
    {"swap_only", "Only what the game waits on (test)"},
};

constexpr Choice kAntiAliasing[] = {
    {"none", "Off"},
    {"fxaa", "FXAA"},
    {"fxaa_extreme", "FXAA (extreme)"},
};

constexpr Choice kAnisotropic[] = {
    {"-1", "No override"}, {"0", "Off"}, {"1", "1x"},  {"2", "2x"},
    {"3", "4x"},             {"4", "8x"},  {"5", "16x"},
};

constexpr Choice kFrameSync[] = {{"-1", "Game's choice"}, {"0", "Off"}, {"1", "On"}};

constexpr Choice kFrameCaps[] = {
    {"display", "The display's refresh rate"},
    {"auto", "Auto (VRR: G-Sync, FreeSync)"},
    {"off", "Off (the console's vblank)"},
    {"60", "60 fps"},
    {"120", "120 fps"},
    {"144", "144 fps"},
    {"180", "180 fps"},
    {"240", "240 fps"},
};

constexpr Choice kBackgroundFps[] = {{"0", "The venue's own"}};

constexpr Choice kControllerTypes[] = {
    {"-1", "Don't override"},
    {"1", "Vocals"},
    {"7", "Guitar"},
    {"8", "Drums"},
};

constexpr Choice kGuitarTypes[] = {
    {"auto", "What each guitar reports"},
    {"rock_band", "Rock Band guitars"},
    {"guitar_hero", "Guitar Hero guitars"},
};

constexpr Choice kInputBackends[] = {{"sdl", "SDL"}, {"xinput", "XInput"}};

constexpr Condition kWithMidiDrums{"midi_drums", "true"};
constexpr Condition kWithMidiKeys{"midi_keys", "true"};
constexpr Condition kWithUsbMics{"usb_mics", "true"};
constexpr Condition kWithHttp{"http_enabled", "true"};
constexpr Condition kWithEvents{"events_enabled", "true"};
constexpr Condition kWithGoCentral{"gocentral", "true"};
constexpr Condition kWithLiveless{"liveless", "true"};
// a broker's host turns Home Assistant's MQTT on
constexpr Condition kWithHaBroker{.cvar = "ha_mqtt_host", .not_empty = true};
// vsync paces the game only with the frame cap off; the cap turns it off
constexpr Condition kWithoutFrameCap{"frame_cap", "off"};

// the native renderer runs with native and both, the emulated GPU with
// emulated and both
constexpr uint8_t kWithNative = kForNative | kForBoth;
constexpr uint8_t kWithEmulatedGpu = kForEmulated | kForBoth;

constexpr Range kSpeeds{0.5, 2.0, 0.05};
constexpr Range kPorts{1, 65535, 1};
// liveless_port's
constexpr Range kGamePorts{1024, 65000, 1};

using enum Tab;
using enum Widget;

constexpr Setting kSettings[] = {
    // Game
    {.cvar = "game_data_root", .tab = kGame, .section = "Folders", .label = "Game data folder",
     .widget = kPath},
    {.cvar = "content_folders", .tab = kGame, .section = "Folders", .label = "Song folders",
     .widget = kFolderList},
    {.cvar = "user_data_root", .tab = kGame, .section = "Folders", .label = "User data folder",
     .widget = kPath},
    {.cvar = "lang", .tab = kGame, .section = "Profile", .label = "Language", .widget = kCombo,
     .choices = kLanguages},
    {.cvar = "username", .tab = kGame, .section = "Profile", .label = "Profile name",
     .widget = kText},
    {.cvar = "fast_start", .tab = kGame, .section = "Game", .label = "Skip the splash screens",
     .widget = kCheckbox},
    {.cvar = "autosave", .tab = kGame, .section = "Game", .label = "Autosave",
     .widget = kCheckbox},
    {.cvar = "skip_profile_prompt", .tab = kGame, .section = "Game",
     .label = "Join without a profile as a guest", .widget = kCheckbox},
    {.cvar = "disable_metamusic", .tab = kGame, .section = "Game", .label = "Menu music",
     .widget = kCombo, .choices = kMenuMusic},
    {.cvar = "forced_venue", .tab = kGame, .section = "Game", .label = "Forced venue",
     .widget = kComboText, .choices = kVenues},
    {.cvar = "music_videos", .tab = kGame, .section = "Game",
     .label = "Music videos in the video venues", .widget = kCheckbox},
    {.cvar = "music_videos_folder", .tab = kGame, .section = "Game",
     .label = "Music videos folder", .widget = kPath},
    {.cvar = "music_video_fit", .tab = kGame, .section = "Game", .label = "Music video shape",
     .widget = kCombo, .choices = kVideoFits},
    {.cvar = "song_speed", .tab = kGame, .section = "Game", .label = "Song speed",
     .widget = kFloatSlider, .range = kSpeeds, .unit = "x"},
    {.cvar = "track_speed", .tab = kGame, .section = "Game", .label = "Track speed",
     .widget = kFloatSlider, .range = kSpeeds, .unit = "x"},
    {.cvar = "unlock_clothing", .tab = kGame, .section = "Game", .label = "Unlock all clothing",
     .widget = kCheckbox},
    {.cvar = "gold_on_all_difficulties", .tab = kGame, .section = "Game",
     .label = "Gold stars on every difficulty", .widget = kCheckbox},
    {.cvar = "game_origin_icons", .tab = kGame, .section = "Game",
     .label = "Song source icons (Deluxe)", .widget = kCheckbox},

    // Graphics
    // first: what rhythm players tune for least lag
    {.cvar = "frame_cap", .tab = kGraphics, .section = "Latency", .label = "Frame rate cap",
     .widget = kComboText, .choices = kFrameCaps,
     .note = "Higher means less input lag: the game judges your hits once a frame. It can run "
             "above your display's refresh rate; the screen shows what it can"},
    {.cvar = "native_present_pacing", .tab = kGraphics, .section = "Latency",
     .label = "Smooth frame pacing", .widget = kCheckbox, .renderers = kWithNative,
     .note = "Off shows each frame as soon as it's drawn: about 3 to 4 ms less lag, with "
             "now and then an uneven step in motion"},
    {.cvar = "monitor", .tab = kGraphics, .section = "Display", .label = "Monitor",
     .widget = kMonitor, .choices = kMonitors},
    {.cvar = "fullscreen", .tab = kGraphics, .section = "Display", .label = "Window mode",
     .widget = kWindowMode, .companion = "fullscreen_exclusive"},
    {.cvar = "fullscreen_exclusive", .tab = kGraphics, .section = "Display",
     .label = "Exclusive fullscreen", .widget = kNone},
    {.cvar = "resolution", .tab = kGraphics, .section = "Display", .label = "Resolution",
     .widget = kResolution, .choices = kResolutions},
    {.cvar = "present_letterbox", .tab = kGraphics, .section = "Display", .label = "Aspect",
     .widget = kCombo, .choices = kAspect},
    {.cvar = "native_fill_window", .tab = kGraphics, .section = "Display",
     .label = "Fill the window", .widget = kCheckbox, .renderers = kWithNative},
    {.cvar = "renderer", .tab = kGraphics, .section = "Renderer", .label = "Renderer",
     .widget = kCombo, .choices = kRenderers},
    // retired (settings.cpp's MigrateRendererSettings clears it): not drawn,
    // here so Save removes its key from band3.toml
    {.cvar = "emulated_gpu", .tab = kGraphics, .section = "Renderer", .label = "Emulated GPU",
     .widget = kNone},
    {.cvar = "native_view_msaa", .tab = kGraphics, .section = "Native renderer",
     .label = "Anti-aliasing (MSAA)", .widget = kCombo, .choices = kNativeMsaa,
     .renderers = kWithNative},
    {.cvar = "native_anisotropic", .tab = kGraphics, .section = "Native renderer",
     .label = "Anisotropic filtering", .widget = kCombo, .choices = kAnisotropic,
     .renderers = kWithNative},
    {.cvar = "native_max_height", .tab = kGraphics, .section = "Native renderer",
     .label = "Resolution limit", .widget = kComboText, .choices = kNativeHeights,
     .renderers = kWithNative},
    {.cvar = "resolution_scale", .tab = kGraphics, .section = "Emulated GPU",
     .label = "Render scale", .widget = kIntStepper, .range = Range{1, 8, 1}, .unit = "x",
     .renderers = kWithEmulatedGpu},
    {.cvar = "swap_post_effect", .tab = kGraphics, .section = "Emulated GPU",
     .label = "Anti-aliasing", .widget = kCombo, .choices = kAntiAliasing,
     .renderers = kWithEmulatedGpu},
    {.cvar = "anisotropic_override", .tab = kGraphics, .section = "Emulated GPU",
     .label = "Anisotropic filtering", .widget = kCombo, .choices = kAnisotropic,
     .renderers = kWithEmulatedGpu},
    {.cvar = "vsync", .tab = kGraphics, .section = "Emulated GPU", .label = "VSync",
     .widget = kCheckbox, .shown_when = kWithoutFrameCap, .renderers = kWithEmulatedGpu},
    {.cvar = "emulated_gpu_while_native", .tab = kGraphics, .section = "Emulated GPU",
     .label = "Emulated GPU work while native", .widget = kCombo,
     .choices = kEmulatedWhileNative, .renderers = kForBoth},
    {.cvar = "rnd_sync", .tab = kGraphics, .section = "Game", .label = "Game frame sync",
     .widget = kCombo, .choices = kFrameSync},
    {.cvar = "background_fps", .tab = kGraphics, .section = "Game",
     .label = "Background frame rate", .widget = kIntStepper, .choices = kBackgroundFps,
     .range = Range{0, 240, 5}, .unit = "fps"},
    {.cvar = "disable_hair_shader", .tab = kGraphics, .section = "Game",
     .label = "Disable the hair shader", .widget = kCheckbox},
    {.cvar = "disable_approximate_lights", .tab = kGraphics, .section = "Game",
     .label = "Disable approximate lighting", .widget = kCheckbox},
    // ignored with renderer native (Hooks/graphics.cpp)
    {.cvar = "compress_character_textures", .tab = kGraphics, .section = "Game",
     .label = "Compress character textures", .widget = kCheckbox,
     .renderers = kWithEmulatedGpu},

    // Audio
    {.cvar = "usb_mics", .tab = kAudio, .section = "Microphones",
     .label = "Use PC microphones", .widget = kCheckbox},
    {.cvar = "usb_mic_devices", .tab = kAudio, .section = "Microphones", .label = "Mics",
     .widget = kMicSlots, .shown_when = kWithUsbMics},
    {.cvar = "audio_maxqframes", .tab = kAudio, .section = "Output", .label = "Audio buffer",
     .widget = kIntStepper, .range = Range{4, 64, 1}, .unit = "frames"},

    // Controllers
    {.cvar = "controller_type", .tab = kControllers, .section = "Instruments",
     .label = "Gamepads play as", .widget = kCombo, .choices = kControllerTypes},
    {.cvar = "guitar_type", .tab = kControllers, .section = "Instruments",
     .label = "Guitars play as", .widget = kCombo, .choices = kGuitarTypes},
    {.cvar = "hid_instruments", .tab = kControllers, .section = "Instruments",
     .label = "PlayStation, Wii and Xbox One instruments", .widget = kCheckbox},
    {.cvar = "midi_drums", .tab = kControllers, .section = "MIDI drums",
     .label = "Play a MIDI drum kit", .widget = kCheckbox},
    {.cvar = "midi_drums_device", .tab = kControllers, .section = "MIDI drums",
     .label = "MIDI port", .widget = kMidiPort, .shown_when = kWithMidiDrums},
    {.cvar = "midi_drums_min_velocity", .tab = kControllers, .section = "MIDI drums",
     .label = "Minimum velocity", .widget = kIntSlider, .range = Range{1, 127, 1},
     .shown_when = kWithMidiDrums},
    {.cvar = "midi_drums_combos", .tab = kControllers, .section = "MIDI drums",
     .label = "Menu buttons from the kit", .widget = kCheckbox, .shown_when = kWithMidiDrums},
    {.cvar = "midi_keys", .tab = kControllers, .section = "MIDI keyboard",
     .label = "Play a MIDI keyboard", .widget = kCheckbox},
    {.cvar = "midi_keys_device", .tab = kControllers, .section = "MIDI keyboard",
     .label = "MIDI port", .widget = kMidiPort, .shown_when = kWithMidiKeys},
    {.cvar = "midi_keys_base_note", .tab = kControllers, .section = "MIDI keyboard",
     .label = "Lowest C", .widget = kMidiNote, .range = Range{0, 103, 1},
     .shown_when = kWithMidiKeys},
    // the test harness's (`midi`), which nobody playing wants: not drawn, and
    // here so it isn't generated into the Advanced tab
    {.cvar = "midi_keys_test_device", .tab = kControllers, .section = "MIDI keyboard",
     .label = "Harness keyboard", .widget = kNone},
    {.cvar = "joypad_lag", .tab = kControllers, .section = "Input lag",
     .label = "Input lag per controller type", .widget = kJoypadLag, .unit = "ms"},
    {.cvar = "left_stick_deadzone_percentage", .tab = kControllers, .section = "Sticks",
     .label = "Left stick deadzone", .widget = kPercentSlider, .range = Range{0, 1, 0.01}},
    {.cvar = "right_stick_deadzone_percentage", .tab = kControllers, .section = "Sticks",
     .label = "Right stick deadzone", .widget = kPercentSlider, .range = Range{0, 1, 0.01}},
    {.cvar = "input_backend", .tab = kControllers, .section = "Input backend",
     .label = "Input backend", .widget = kCombo, .choices = kInputBackends,
     .windows_only = true},

    // Online
    {.cvar = "http_enabled", .tab = kOnline, .section = "Web song browser",
     .label = "Web song browser", .widget = kCheckbox},
    {.cvar = "http_port", .tab = kOnline, .section = "Web song browser", .label = "Port",
     .widget = kIntStepper, .range = kPorts, .shown_when = kWithHttp},
    {.cvar = "http_rhythmverse", .tab = kOnline, .section = "Web song browser",
     .label = "RhythmVerse downloads", .widget = kCheckbox, .shown_when = kWithHttp},
    {.cvar = "discord_enabled", .tab = kOnline, .section = "Discord",
     .label = "Discord presence", .widget = kCheckbox},
    {.cvar = "rb3e_mode", .tab = kOnline, .section = "RB3Enhanced", .label = "RB3E mode",
     .widget = kCheckbox},
    {.cvar = "events_enabled", .tab = kOnline, .section = "RB3Enhanced",
     .label = "Send RB3E events", .widget = kCheckbox},
    {.cvar = "events_target", .tab = kOnline, .section = "RB3Enhanced", .label = "Send to",
     .widget = kText, .shown_when = kWithEvents},
    {.cvar = "events_port", .tab = kOnline, .section = "RB3Enhanced", .label = "Port",
     .widget = kIntStepper, .range = kPorts, .shown_when = kWithEvents},
    {.cvar = "ha_mqtt_host", .tab = kOnline, .section = "Home Assistant", .label = "MQTT broker",
     .widget = kText},
    {.cvar = "ha_mqtt_port", .tab = kOnline, .section = "Home Assistant", .label = "Port",
     .widget = kIntStepper, .range = kPorts, .shown_when = kWithHaBroker},
    {.cvar = "ha_mqtt_username", .tab = kOnline, .section = "Home Assistant",
     .label = "User name", .widget = kText, .shown_when = kWithHaBroker},
    {.cvar = "ha_mqtt_password", .tab = kOnline, .section = "Home Assistant", .label = "Password",
     .widget = kPassword, .shown_when = kWithHaBroker},
    {.cvar = "ha_discovery_prefix", .tab = kOnline, .section = "Home Assistant",
     .label = "Discovery prefix", .widget = kText, .shown_when = kWithHaBroker},
    {.cvar = "ha_stagekit", .tab = kOnline, .section = "Home Assistant",
     .label = "Stage Kit lights", .widget = kCheckbox, .shown_when = kWithHaBroker},
    {.cvar = "ha_webhook_url", .tab = kOnline, .section = "Home Assistant", .label = "Webhook URL",
     .widget = kText},
    // only on Windows for now (online::Start)
    {.cvar = "gocentral", .tab = kOnline, .section = "GoCentral",
     .label = "Leaderboards, battles and setlists (GoCentral)", .widget = kCheckbox,
     .windows_only = true},
    {.cvar = "gocentral_address", .tab = kOnline, .section = "GoCentral", .label = "Server",
     .widget = kText, .shown_when = kWithGoCentral, .windows_only = true},
    {.cvar = "liveless", .tab = kOnline, .section = "Online play (Liveless)",
     .label = "Play online without Xbox Live", .widget = kCheckbox, .windows_only = true},
    {.cvar = "liveless_connect", .tab = kOnline, .section = "Online play (Liveless)",
     .label = "Game to join", .widget = kText, .shown_when = kWithLiveless,
     .windows_only = true},
    {.cvar = "liveless_external_ip", .tab = kOnline, .section = "Online play (Liveless)",
     .label = "Your address for players joining", .widget = kText,
     .shown_when = kWithLiveless, .windows_only = true},
    {.cvar = "liveless_port", .tab = kOnline, .section = "Online play (Liveless)",
     .label = "Port", .widget = kIntStepper, .range = kGamePorts,
     .shown_when = kWithLiveless, .windows_only = true},

    // the devices found and the test controls follow this section
    // (SettingsPage::DrawLights)
    {.cvar = "stagekit_usb", .tab = kLights, .section = "Stage Kit lights",
     .label = "Stage Kits plugged in by USB", .widget = kCheckbox},
    {.cvar = "pico_discovery", .tab = kLights, .section = "Stage Kit lights",
     .label = "Find wireless Stage Kits (Pico W)", .widget = kCheckbox},

    {.cvar = "steam_deck_defaults", .tab = kSteamDeck, .section = "Steam Deck",
     .label = "Use the Steam Deck settings", .widget = kCheckbox},
    {.cvar = "show_launcher", .tab = kFooter, .section = "Footer",
     .label = "Show this screen at startup", .widget = kCheckbox},
};

// the controller types band3's instruments reach the game as
// (JoypadTypeName, Input/pro_instrument_status.cpp)
constexpr LagType kLagTypes[] = {
    {5, "Guitar"},
    {6, "RB2 guitar"},
    {29, "Core guitar"},
    {8, "Drums"},
    {9, "RB2 drums"},
    {33, "MIDI Pro Adapter drums"},
    {30, "Pro guitar (Mustang)"},
    {31, "Pro guitar (Squier)"},
    {32, "MIDI Pro Adapter keyboard"},
    {34, "Keytar"},
};

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

std::string Utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

// joypad_lag's numbers: whole ones without a point
std::string FormatMs(float ms) {
    char buffer[64];
    const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), ms);
    return ec == std::errc() ? std::string(buffer, end) : std::to_string(ms);
}

// an entry's type, if it's an entry joypad_lag reads
std::optional<uint32_t> LagEntryType(std::string_view entry) {
    input::JoypadLagOverrides overrides{};
    if (!input::ParseJoypadLagOverrides(entry, overrides).empty()) return std::nullopt;
    const std::string_view number = Trim(entry.substr(0, entry.find('=')));
    uint32_t type = 0;
    const auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), type);
    if (ec != std::errc() || end != number.data() + number.size()) return std::nullopt;
    return type;
}

bool InRange(const CvarFacts& facts, double value) {
    return (!facts.min || value >= *facts.min) && (!facts.max || value <= *facts.max);
}

// whether the cvar would take `value` (SetFlagByName's checks, as far as they're known here)
bool Takes(const CvarFacts& facts, std::string_view value) {
    switch (facts.type) {
    case ValueType::kBool: return true;
    case ValueType::kInt: {
        const auto v = AsInt(value);
        if (!v || !InRange(facts, static_cast<double>(*v))) return false;
        break;
    }
    case ValueType::kFloat: {
        const auto v = AsFloat(value);
        if (!v || !InRange(facts, *v)) return false;
        break;
    }
    case ValueType::kString: break;
    }
    return facts.allowed.empty() || std::ranges::find(facts.allowed, value) != facts.allowed.end();
}

}

std::span<const Setting> SettingTable() { return kSettings; }

const Setting* FindSetting(std::span<const Setting> table, std::string_view cvar) {
    const auto it = std::ranges::find(table, cvar, &Setting::cvar);
    return it == table.end() ? nullptr : &*it;
}

std::vector<std::string_view> SectionsOf(std::span<const Setting> table, Tab tab) {
    std::vector<std::string_view> sections;
    for (const auto& s : table) {
        if (s.tab == tab && std::ranges::find(sections, s.section) == sections.end()) {
            sections.push_back(s.section);
        }
    }
    return sections;
}

std::string LowestLatencyCap(double display_hz) {
    constexpr int kMax = 240;  // frame_cap's highest (frame_pacing.h's ParseFrameCap)
    const int hz = static_cast<int>(std::lround(display_hz));
    if (hz <= 0 || hz >= kMax) return std::to_string(kMax);
    return std::to_string(hz * (kMax / hz));
}

bool AsBool(std::string_view value) {
    // rex::string::from_string<bool>
    return value == "true" || value == "1" || value == "yes";
}

std::optional<int64_t> AsInt(std::string_view value) {
    int64_t v = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
    if (ec != std::errc()) return std::nullopt;
    return v;
}

std::optional<double> AsFloat(std::string_view value) {
    double v = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
    if (ec != std::errc() || end != value.data() + value.size()) return std::nullopt;
    return v;
}

bool SameValue(ValueType type, std::string_view a, std::string_view b) {
    switch (type) {
    case ValueType::kBool: return AsBool(a) == AsBool(b);
    case ValueType::kInt: {
        const auto x = AsInt(a);
        const auto y = AsInt(b);
        if (x && y) return *x == *y;
        break;
    }
    case ValueType::kFloat: {
        const auto x = AsFloat(a);
        const auto y = AsFloat(b);
        if (x && y) {
            // the cvars print six decimals (std::to_string)
            const double scale = std::max({1.0, std::fabs(*x), std::fabs(*y)});
            return std::fabs(*x - *y) <= 1e-6 * scale;
        }
        break;
    }
    case ValueType::kString: break;
    }
    return a == b;
}

std::string PathForConfig(std::string_view value, const std::filesystem::path& anchor) {
    if (Trim(value).empty()) return {};
    std::filesystem::path path = paths::Resolve(Trim(value), anchor).lexically_normal();
    // "songs/" is the folder songs
    if (!path.has_filename() && path.has_relative_path()) path = path.parent_path();
    const std::filesystem::path relative = path.lexically_relative(anchor.lexically_normal());
    if (!relative.empty() && *relative.begin() != "..") return Utf8(relative);
    return Utf8(path);
}

std::string FolderListForConfig(std::string_view value, const std::filesystem::path& anchor) {
    std::string out;
    for (const auto& folder : paths::SplitList(value)) {
        if (!out.empty()) out += '|';
        out += PathForConfig(folder, anchor);
    }
    return out;
}

std::string JoinMicSlots(std::span<const std::string> slots) {
    size_t count = slots.size();
    while (count > 0 && Trim(slots[count - 1]).empty()) count--;
    std::string out;
    for (size_t i = 0; i < count; i++) {
        if (i > 0) out += ',';
        out += Trim(slots[i]);
    }
    return out;
}

std::span<const LagType> LagEditorTypes() { return kLagTypes; }

std::optional<float> LagFor(std::string_view text, uint32_t type) {
    input::JoypadLagOverrides overrides{};
    input::ParseJoypadLagOverrides(text, overrides);
    if (type >= overrides.size() || !overrides[type]) return std::nullopt;
    return overrides[type]->game;
}

std::string WithLag(std::string_view text, uint32_t type, std::optional<float> ms) {
    std::vector<std::string> entries;
    while (!text.empty()) {
        const size_t comma = text.find(',');
        const std::string_view entry = Trim(text.substr(0, comma));
        text.remove_prefix(comma == std::string_view::npos ? text.size() : comma + 1);
        if (!entry.empty()) entries.emplace_back(entry);
    }

    // the last entry for the type is the one the game reads
    std::optional<size_t> found;
    for (size_t i = 0; i < entries.size(); i++) {
        if (LagEntryType(entries[i]) == type) found = i;
    }
    const std::string game = ms ? FormatMs(*ms) : std::string();
    if (!found) {
        if (ms) entries.push_back(std::to_string(type) + "=" + game);
    } else {
        std::string& entry = entries[*found];
        const size_t equals = entry.find('=');
        const std::string_view values = std::string_view(entry).substr(equals + 1);
        const size_t slash = values.find('/');
        // the video and audio parts, as they were
        const std::string rest =
            slash == std::string_view::npos ? std::string() : std::string(values.substr(slash));
        bool rest_blank = true;
        for (char c : rest) {
            if (c != '/' && c != ' ') rest_blank = false;
        }
        if (!ms && rest_blank) {
            entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(*found));
        } else {
            entry = std::string(Trim(std::string_view(entry).substr(0, equals))) + "=" + game + rest;
        }
    }

    std::string out;
    for (const auto& entry : entries) {
        if (!out.empty()) out += ',';
        out += entry;
    }
    return out;
}

PageFeatures FeaturesFor(Where where) {
    switch (where) {
    case Where::kLauncher: return {};
    case Where::kInGame:
        return {.device_tester = false,
                .mic_meters = false,
                .generated_rows = true,
                .next_start_notes = true};
    }
    return {};
}

const char* LockReason(Lock lock) {
    switch (lock) {
    case Lock::kCommandLine: return "Set on the command line";
    case Lock::kEnvironment: return "Set by an environment variable";
    case Lock::kNone: break;
    }
    return "";
}

SettingsModel::SettingsModel(std::span<const Setting> table, Environment env, CvarStore& store)
    : table_(table), env_(std::move(env)), store_(store) {
    if (const CvarFacts* facts = Facts(kShowLauncher); facts && facts->exists) {
        const bool on = AsBool(Value(kShowLauncher));
        show_at_startup_ = IsLocked(kShowLauncher) ? on : on && facts->from_config;
    }
    MarkSaved();
}

const Setting* SettingsModel::Find(std::string_view cvar) const {
    return FindSetting(table_, cvar);
}

const CvarFacts* SettingsModel::Facts(std::string_view cvar) const {
    const auto it = env_.cvars.find(cvar);
    return it == env_.cvars.end() ? nullptr : &it->second;
}

std::string_view SettingsModel::Description(std::string_view cvar) const {
    const CvarFacts* facts = Facts(cvar);
    return facts ? std::string_view(facts->description) : std::string_view();
}

bool SettingsModel::Available(const Setting& setting) const {
    const CvarFacts* facts = Facts(setting.cvar);
    if (!facts || !facts->exists) return false;
    return !setting.windows_only || env_.windows;
}

bool SettingsModel::Visible(const Setting& setting) const {
    if (!Available(setting) || setting.widget == Widget::kNone) return false;
    if (setting.tab == Tab::kSteamDeck && !env_.steam_deck) return false;
    if (!ForRenderer(setting.renderers)) return false;
    if (setting.shown_when.cvar.empty()) return true;
    const CvarFacts* facts = Facts(setting.shown_when.cvar);
    if (!facts || !facts->exists) return true;
    if (setting.shown_when.not_empty) return !Value(setting.shown_when.cvar).empty();
    return SameValue(facts->type, Value(setting.shown_when.cvar), setting.shown_when.value);
}

bool SettingsModel::ForRenderer(uint8_t renderers) const {
    if (renderers == kForAnyRenderer) return true;
    const CvarFacts* facts = Facts("renderer");
    // a value it wouldn't take (nothing sets one) shows everything
    const auto mode =
        facts && facts->exists ? render::ParseRenderer(Value("renderer")) : std::nullopt;
    if (!mode) return true;
    switch (*mode) {
    case render::RendererMode::kNative: return renderers & kForNative;
    case render::RendererMode::kEmulated: return renderers & kForEmulated;
    case render::RendererMode::kBoth: return renderers & kForBoth;
    }
    return true;
}

bool SettingsModel::Offered(const Choice& choice) const {
    return !choice.windows_only || env_.windows;
}

std::optional<std::string> SettingsModel::SectionNote(Tab tab, std::string_view section) const {
    // the game's calibration takes up what lag is left, as long as it doesn't change
    if (tab == Tab::kGraphics && section == "Latency") {
        return "After changing these, run the game's calibration again (in its Options), so "
               "your hits are judged against the lag you have now.";
    }
    // the emulated GPU's own settings are the plugin's, there only once it runs
    if (tab == Tab::kGraphics && section == "Emulated GPU" && !env_.emulated_gpu_running &&
        ForRenderer(kWithEmulatedGpu)) {
        return env_.in_game
                   ? "The emulated GPU's own settings (render scale, anti-aliasing, anisotropic "
                     "filtering, VSync) show here once band3 runs with it: the new renderer "
                     "applies at the next start."
                   : "The emulated GPU's own settings (render scale, anti-aliasing, anisotropic "
                     "filtering, VSync) show here once band3 runs with it: Play restarts band3 "
                     "for the new renderer.";
    }
    // the game has its folders: the game data, the user data and the songs
    if (env_.in_game && tab == Tab::kGame && section == "Folders") {
        return "Folders apply at the next start.";
    }
    return std::nullopt;
}

std::string SettingsModel::Value(std::string_view cvar) const {
    const CvarFacts* facts = Facts(cvar);
    if (!facts || !facts->exists) return {};
    return store_.Get(cvar);
}

bool SettingsModel::DeckPresetsOn() const {
    return env_.steam_deck && AsBool(Value("steam_deck_defaults"));
}

std::string SettingsModel::DefaultFor(std::string_view cvar, bool deck_on) const {
    const CvarFacts* facts = Facts(cvar);
    if (!facts) return {};
    if (facts->path_default) return Utf8(*facts->path_default);
    if (facts->startup_forced) return *facts->startup_forced;
    if (deck_on && facts->deck_preset) return *facts->deck_preset;
    // ApplyLegacyIni leaves the default where the cvar refuses the ini's value
    if (facts->ini && Takes(*facts, *facts->ini)) return *facts->ini;
    if (facts->startup_default) return *facts->startup_default;
    return facts->registry_default;
}

std::string SettingsModel::EffectiveDefault(std::string_view cvar) const {
    return DefaultFor(cvar, DeckPresetsOn());
}

bool SettingsModel::Same(std::string_view cvar, std::string_view a, std::string_view b) const {
    const CvarFacts* facts = Facts(cvar);
    if (!facts) return a == b;
    const Setting* setting = Find(cvar);
    if (setting && setting->widget == Widget::kPath) {
        // an empty folder setting is the default folder
        const std::string x = PathForConfig(a, env_.anchor);
        const std::string y = PathForConfig(b, env_.anchor);
        const std::string fallback = facts->path_default
                                         ? PathForConfig(Utf8(*facts->path_default), env_.anchor)
                                         : std::string();
        return (x.empty() ? fallback : x) == (y.empty() ? fallback : y);
    }
    if (setting && setting->widget == Widget::kFolderList) {
        return FolderListForConfig(a, env_.anchor) == FolderListForConfig(b, env_.anchor);
    }
    return SameValue(facts->type, a, b);
}

bool SettingsModel::IsChanged(std::string_view cvar) const {
    const Setting* setting = Find(cvar);
    if (!setting) return false;
    auto changed = [this](std::string_view name) {
        const CvarFacts* facts = Facts(name);
        return facts && facts->exists && !Same(name, Value(name), EffectiveDefault(name));
    };
    return changed(cvar) || (!setting->companion.empty() && changed(setting->companion));
}

bool SettingsModel::Set(std::string_view cvar, std::string_view value) {
    const CvarFacts* facts = Facts(cvar);
    if (!facts || !facts->exists || facts->lock != Lock::kNone || ReadOnly(cvar)) return false;

    const bool deck_toggle = cvar == "steam_deck_defaults" && env_.steam_deck;
    // the preset settings' defaults before the toggle
    std::map<std::string, std::string, std::less<>> old_defaults;
    if (deck_toggle) {
        for (const auto& s : table_) {
            const CvarFacts* f = Facts(s.cvar);
            if (f && f->exists && f->deck_preset) {
                old_defaults[std::string(s.cvar)] = EffectiveDefault(s.cvar);
            }
        }
    }

    if (!store_.Set(cvar, value)) return false;
    edited_.insert(std::string(cvar));

    // turning the presets off (or on) doesn't make them overrides: settings
    // still at the old default follow to the new one
    for (const auto& [name, old_default] : old_defaults) {
        if (edited_.contains(name) || IsLocked(name)) continue;
        if (!Same(name, Value(name), old_default)) continue;
        const std::string new_default = EffectiveDefault(name);
        if (!Same(name, old_default, new_default)) store_.Set(name, new_default);
    }
    return true;
}

bool SettingsModel::Reset(std::string_view cvar) {
    const Setting* setting = Find(cvar);
    if (!setting || IsLocked(cvar) || ReadOnly(cvar)) return false;
    auto reset = [this](std::string_view name) {
        const CvarFacts* facts = Facts(name);
        if (!facts || !facts->exists || facts->lock != Lock::kNone) return;
        const Setting* s = Find(name);
        // an empty folder setting is the default folder
        const std::string value =
            s && s->widget == Widget::kPath ? std::string() : EffectiveDefault(name);
        // setting a value it already has would still re-apply a HotReload one.
        // The Deck toggle goes through Set, so the preset settings the player
        // hasn't edited follow it as they do when it's ticked.
        const bool deck_toggle = name == "steam_deck_defaults" && env_.steam_deck;
        if (Same(name, Value(name), value) ||
            (deck_toggle ? Set(name, value) : store_.Set(name, value))) {
            edited_.erase(std::string(name));
        }
    };
    reset(cvar);
    if (!setting->companion.empty()) reset(setting->companion);
    return true;
}

Lock SettingsModel::LockOf(std::string_view cvar) const {
    const CvarFacts* facts = Facts(cvar);
    return facts ? facts->lock : Lock::kNone;
}

bool SettingsModel::ReadOnly(std::string_view cvar) const {
    const CvarFacts* facts = Facts(cvar);
    return env_.in_game && facts && facts->lifecycle == Lifecycle::kInitOnly;
}

bool SettingsModel::NextStartOnly(std::string_view cvar) const {
    const CvarFacts* facts = Facts(cvar);
    if (!env_.in_game || !facts || !facts->exists) return false;
    return facts->lifecycle != Lifecycle::kHotReload || facts->read_once;
}

bool SettingsModel::RendererNeedsRestart() const {
    const CvarFacts* facts = Facts("renderer");
    if (!facts || !facts->exists) return false;
    const render::RendererState run{.native_only = !env_.emulated_gpu_running,
                                    .presentable = env_.native_presentable};
    return render::RendererRestartNeeded(run, Value("renderer"));
}

bool SettingsModel::WaitsForNextStart(std::string_view cvar) const {
    if (!env_.in_game) return false;
    if (cvar == "renderer") return RendererNeedsRestart();
    auto waits = [this](std::string_view name) {
        const CvarFacts* facts = Facts(name);
        return NextStartOnly(name) && facts->started_with &&
               !Same(name, Value(name), *facts->started_with);
    };
    const Setting* setting = Find(cvar);
    return waits(cvar) || (setting && !setting->companion.empty() && waits(setting->companion));
}

int SettingsModel::ChoiceIndex(const Setting& setting) const {
    const std::string value = Value(setting.cvar);
    const CvarFacts* facts = Facts(setting.cvar);
    const ValueType type = facts ? facts->type : ValueType::kString;
    for (size_t i = 0; i < setting.choices.size(); i++) {
        if (SameValue(type, value, setting.choices[i].value)) return static_cast<int>(i);
    }
    return -1;
}

WindowMode SettingsModel::GetWindowMode() const {
    if (!AsBool(Value("fullscreen"))) return WindowMode::kWindowed;
    return AsBool(Value("fullscreen_exclusive")) ? WindowMode::kExclusive : WindowMode::kBorderless;
}

bool SettingsModel::SetWindowMode(WindowMode mode) {
    const bool fullscreen = mode != WindowMode::kWindowed;
    const bool exclusive = mode == WindowMode::kExclusive;
    bool ok = true;
    // exclusive first, so the window doesn't go borderless on the way
    if (!SameValue(ValueType::kBool, Value("fullscreen_exclusive"), exclusive ? "true" : "false")) {
        ok = Set("fullscreen_exclusive", exclusive ? "true" : "false") && ok;
    }
    if (!SameValue(ValueType::kBool, Value("fullscreen"), fullscreen ? "true" : "false")) {
        ok = Set("fullscreen", fullscreen ? "true" : "false") && ok;
    }
    return ok;
}

std::optional<std::string> SettingsModel::Warning(std::string_view cvar) const {
    if (cvar == "resolution_scale" && env_.steam_deck) {
        const auto scale = AsInt(Value(cvar));
        if (scale && *scale > 1) {
            return "Costs a lot of GPU time for detail the Deck's 1280x800 screen can't show; "
                   "1 is the console's resolution";
        }
    }
    // what online::Start refuses, said before Play
    if (cvar == "gocentral" && AsBool(Value(cvar)) && !online::IsOwnAccountName(Value("username"))) {
        return "Set the profile name (Game tab) to a name of your own first: it's your "
               "GoCentral account, and band3 won't connect as \"User\" or a blank one";
    }
    if (cvar == "liveless_connect" && AsBool(Value("liveless"))) {
        const auto port = AsInt(Value("liveless_port"));
        if (!online::ParseJoinAddress(Value(cvar), static_cast<uint16_t>(port.value_or(online::kGamePort)))) {
            return "Not an address: an IP or a name, with :port if it isn't 9103";
        }
    }
    return std::nullopt;
}

bool SettingsModel::Secret(std::string_view cvar) const {
    const Setting* setting = Find(cvar);
    return setting && setting->widget == Widget::kPassword;
}

std::string SettingsModel::Refusal(std::string_view cvar, std::string_view value) const {
    if (Secret(cvar)) return "Not accepted";
    std::string text = "Not accepted: \"" + std::string(value) + "\"";
    const CvarFacts* facts = Facts(cvar);
    if (!facts) return text;
    auto number = [](double v) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%g", v);
        return std::string(buffer);
    };
    const bool numeric = facts->type == ValueType::kInt || facts->type == ValueType::kFloat;
    const bool parses = facts->type == ValueType::kInt ? AsInt(value).has_value()
                                                       : AsFloat(value).has_value();
    if (numeric && !parses) return text + " isn't a number";
    if (numeric && (facts->min || facts->max)) {
        if (facts->min && facts->max) {
            return text + " isn't from " + number(*facts->min) + " to " + number(*facts->max);
        }
        return facts->min ? text + " is below " + number(*facts->min)
                          : text + " is above " + number(*facts->max);
    }
    if (!facts->allowed.empty()) {
        std::string list;
        for (const auto& allowed : facts->allowed) {
            if (!list.empty()) list += ", ";
            list += allowed.empty() ? "(empty)" : allowed;
        }
        return text + " isn't one of " + list;
    }
    return text;
}

bool SettingsModel::SetShowAtStartup(bool on) {
    const CvarFacts* facts = Facts(kShowLauncher);
    if (!facts || !facts->exists || IsLocked(kShowLauncher)) return false;
    show_at_startup_ = on;
    return true;
}

bool SettingsModel::HasUnsavedChanges() const {
    if (show_at_startup_ != saved_show_at_startup_) return true;
    for (const auto& s : table_) {
        if (!Available(s) || IsLocked(s.cvar) || ReadOnly(s.cvar)) continue;
        const auto it = saved_.find(s.cvar);
        if (it == saved_.end() || !Same(s.cvar, Value(s.cvar), it->second)) return true;
    }
    return false;
}

std::optional<ConfigValue> SettingsModel::TypedValue(const Setting& setting) const {
    const CvarFacts* facts = Facts(setting.cvar);
    const std::string value = Value(setting.cvar);
    switch (facts->type) {
    case ValueType::kBool: return AsBool(value);
    case ValueType::kInt:
        if (const auto v = AsInt(value)) return *v;
        break;
    case ValueType::kFloat:
        if (const auto v = AsFloat(value)) return *v;
        break;
    case ValueType::kString: break;
    }
    if (setting.widget == Widget::kPath) return PathForConfig(value, env_.anchor);
    if (setting.widget == Widget::kFolderList) return FolderListForConfig(value, env_.anchor);
    return value;
}

std::vector<ConfigEdit> SettingsModel::Edits() const {
    std::vector<ConfigEdit> edits;
    for (const auto& s : table_) {
        // a locked value wouldn't win, and saving it would keep it after the
        // command line stops setting it; a read-only one is band3.toml's own
        if (!Available(s) || IsLocked(s.cvar) || ReadOnly(s.cvar)) continue;
        if (s.cvar == kShowLauncher) {
            edits.push_back({std::string(s.cvar), ConfigValue(show_at_startup_)});
            continue;
        }
        const bool changed = !Same(s.cvar, Value(s.cvar), EffectiveDefault(s.cvar));
        edits.push_back({std::string(s.cvar), changed ? TypedValue(s) : std::nullopt});
    }
    return edits;
}

SaveResult SettingsModel::Save(const std::filesystem::path& file) {
    const auto edits = Edits();
    SaveResult result = SaveConfigFile(file, edits);
    if (!result.ok) return result;
    // into the cvar as well as the file, so F4's "Save to config" keeps it
    const Setting* show = Find(kShowLauncher);
    if (show && Available(*show) && !IsLocked(kShowLauncher)) {
        store_.Set(kShowLauncher, show_at_startup_ ? "true" : "false");
    }
    MarkSaved();
    return result;
}

void SettingsModel::MarkSaved() {
    saved_show_at_startup_ = show_at_startup_;
    saved_.clear();
    for (const auto& s : table_) {
        if (Available(s)) saved_[std::string(s.cvar)] = Value(s.cvar);
    }
}

}
