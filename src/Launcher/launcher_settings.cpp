#include "launcher_settings.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include "src/Hooks/frame_pacing.h"
#include "src/Input/joypad_lag_status.h"
#include "src/Net/online.h"
#include "src/Render/renderer_mode.h"
#include "src/paths.h"

namespace band3::launcher {

namespace {

// the "Open the launcher at startup" row (ShowAtStartup)
constexpr std::string_view kShowLauncher = "show_launcher";

// Choices

constexpr Choice kLanguages[] = {
    {"", "Default"},  {"eng", "English"}, {"esl", "Spanish"},
    {"fre", "French"}, {"ita", "Italian"}, {"deu", "German"},
};

// the disable_ settings, read the other way round: On is "false"
constexpr Choice kOnWhenFalse[] = {{"false", "On"}, {"true", "Off"}};

// music_video_venue_chance's
constexpr Choice kVideoVenueChances[] = {
    {"0", "Never"},
    {"25", "A quarter of the time"},
    {"50", "Half the time"},
    {"75", "Most of the time"},
    {"100", "Always"},
};

// music_video_max_height's
constexpr Choice kVideoHeights[] = {
    {"480", "480p"}, {"720", "720p"}, {"1080", "1080p"}, {"1440", "1440p"}, {"2160", "4K"},
};

constexpr Choice kVideoFits[] = {
    {"fit", "Letterbox"},
    {"fill", "Crop to fill"},
    {"stretch", "Stretch"},
};

constexpr Choice kVenues[] = {
    {"false", "Game's pick"},
    {"arena", "Arena"},
    {"big_club", "Big club"},
    {"small_club", "Small club"},
    {"festival", "Festival"},
    {"video", "Video venue"},
    {"none", "None (black)"},
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

constexpr Choice kAspect[] = {{"true", "Keep 16:9"}, {"false", "Stretch"}};

// ui/theme.h's Look
constexpr Choice kMenuStyles[] = {{"rb3", "Rock Band 3"}, {"simple", "Simple"}};

// renderer_mode.h's RendererLabel
constexpr Choice kRenderers[] = {
    {"native", "Native"},
    {"emulated", "Emulated"},
    {"both", "Both (debug)"},
};

constexpr Choice kNativeMsaa[] = {{"1", "Off"}, {"2", "Game's (2x)"}, {"4", "4x"}};

// native_view_shadow_scale's: the characters' self-shadow maps, times the game's 512x512
constexpr Choice kShadowScales[] = {
    {"1", "Game's (512)"},
    {"2", "High (1024)"},
    {"3", "Higher (1536)"},
    {"4", "Highest (2048)"},
};

// native_max_height's: the most lines drawn, a taller window's picture scaled up
constexpr Choice kNativeHeights[] = {
    {"0", "Window's size"},
    {"360", "360p"},
    {"480", "480p"},
    {"540", "540p"},
    {"720", "720p"},
    {"1080", "1080p"},
    {"1440", "1440p"},
    {"2160", "4K"},
};

constexpr Choice kEmulatedWhileNative[] = {
    {"skip_draws", "Only what's needed"},
    {"full", "Everything"},
    {"swap_only", "What the game waits on (test)"},
};

constexpr Choice kAntiAliasing[] = {
    {"none", "Off"},
    {"fxaa", "FXAA"},
    {"fxaa_extreme", "FXAA (extreme)"},
};

constexpr Choice kAnisotropic[] = {
    {"-1", "Default"}, {"0", "Off"}, {"1", "1x"}, {"2", "2x"},
    {"3", "4x"},       {"4", "8x"},  {"5", "16x"},
};

constexpr Choice kFrameSync[] = {{"-1", "Game's pick"}, {"0", "Off"}, {"1", "On"}};

constexpr Choice kFrameCaps[] = {
    {"display", "Match the display"},
    {"auto", "Just under the display"},
    {"off", "Off (the console's 60)"},
    {"60", "60 fps"},
    {"120", "120 fps"},
    {"144", "144 fps"},
    {"180", "180 fps"},
    {"240", "240 fps"},
};

constexpr Choice kBackgroundFps[] = {{"0", "Venue's own"}};

// native_deferred_decode_threads': 0 decodes on the renderer's own thread
constexpr Choice kDecodeThreads[] = {{"0", "Off"}};

constexpr Choice kControllerTypes[] = {
    {"-1", "Game's pick"},
    {"1", "Vocals"},
    {"7", "Guitar"},
    {"8", "Drums"},
};

constexpr Choice kGuitarTypes[] = {
    {"auto", "Auto"},
    {"rock_band", "Rock Band guitars"},
    {"guitar_hero", "Guitar Hero guitars"},
};

constexpr Choice kInputBackends[] = {{"sdl", "SDL"}, {"xinput", "XInput"}};

constexpr Choice kHttpAddresses[] = {
    {"0.0.0.0", "Your network"},
    {"127.0.0.1", "This PC only"},
};

constexpr Condition kWithMidiDrums{"midi_drums", "true"};
constexpr Condition kWithMidiKeys{"midi_keys", "true"};
constexpr Condition kWithUsbMics{"usb_mics", "true"};
constexpr Condition kWithHttp{"http_enabled", "true"};
constexpr Condition kWithLastfm{"lastfm_enabled", "true"};
constexpr Condition kWithListenBrainz{"listenbrainz_enabled", "true"};
constexpr Condition kWithEvents{"events_enabled", "true"};
constexpr Condition kWithGoCentral{"gocentral", "true"};
constexpr Condition kWithLiveless{"liveless", "true"};
constexpr Condition kWithRooms{"liveless_rooms", "true"};
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

using enum Category;
using enum Widget;

constexpr Setting kSettings[] = {
    // Game
    {.cvar = "game_data_root", .category = kGame, .section = "Folders",
     .label = "Game data folder", .widget = kPath, .launcher_only = true,
     .about = "Where Rock Band 3's own files are: the folder that holds gen/main_xbox.hdr. "
              "band3 checks it as you pick it."},
    {.cvar = "content_folders", .category = kGame, .section = "Folders", .label = "Song folders",
     .widget = kFolderList, .launcher_only = true,
     .about = "Folders of extra songs: customs, exports and DLC, loose or in packages, their "
              "subfolders too. The game reads them as it starts."},
    {.cvar = "user_data_root", .category = kGame, .section = "Folders",
     .label = "User data folder", .widget = kPath, .launcher_only = true,
     .about = "Where your saves, profiles and score history are kept. Default keeps them "
              "beside band3."},
    {.cvar = "allow_game_relative_writes", .category = kGame, .section = "Folders",
     .label = "Allow writes to the game folder", .widget = kCheckbox,
     .about = "Lets the game and its mods write files to paths relative to the game data "
              "folder. Off, those writes are refused."},
    {.cvar = "username", .category = kGame, .section = "Profile", .label = "Profile name",
     .widget = kText,
     .about = "The name your profile goes by wherever the game asks for one, online too, up "
              "to 15 characters. Empty keeps the profile's own."},
    {.cvar = "lang", .category = kGame, .section = "Profile", .label = "Language",
     .widget = kCombo, .choices = kLanguages,
     .about = "The game's language. Default follows the game data."},
    {.cvar = "skip_profile_prompt", .category = kGame, .section = "Profile",
     .label = "Guests without a profile", .widget = kCheckbox,
     .about = "Players who join without a profile join as guests, without the game asking "
              "them to pick one. Off, it asks, as on a console."},
    {.cvar = "score_history", .category = kGame, .section = "Profile", .label = "Score history",
     .widget = kCheckbox,
     .about = "Keeps every finished song's results (score, stars, notes hit) for the web "
              "page's History tab. Songs played on autoplay aren't kept."},
    {.cvar = "show_launcher", .category = kGame, .section = "Startup",
     .label = "Open the launcher at startup", .widget = kCheckbox,
     .about = "Off starts the game straight away. To see the launcher again, hold Shift as "
              "band3 starts, or start it with --launcher."},
    {.cvar = "fast_start", .category = kGame, .section = "Startup",
     .label = "Skip the splash screens", .widget = kCheckbox,
     .about = "Goes straight to the title screen, past the logos, once the game has loaded."},
    {.cvar = "autosave", .category = kGame, .section = "Startup", .label = "Autosave",
     .widget = kCheckbox,
     .about = "Lets the game save your profile after songs and setlist edits. Off, it's saved "
              "only from the game's options menu."},
    {.cvar = "disable_metamusic", .category = kGame, .section = "Startup", .label = "Menu music",
     .widget = kCombo, .choices = kOnWhenFalse, .about = "The music behind the game's menus."},

    // Gameplay
    {.cvar = "song_speed", .category = kGameplay, .section = "Speed", .label = "Song speed",
     .widget = kFloatSlider, .range = kSpeeds, .unit = "x",
     .about = "Plays every song faster or slower, from the next song. A speed practice mode "
              "or Rock Band 3 Deluxe sets is left as it is."},
    {.cvar = "track_speed", .category = kGameplay, .section = "Speed", .label = "Note speed",
     .widget = kFloatSlider, .range = kSpeeds, .unit = "x",
     .about = "How fast the notes come down the track: faster spaces them out. From the next "
              "song."},
    {.cvar = "unlock_clothing", .category = kGameplay, .section = "Unlocks",
     .label = "Unlock all clothing", .widget = kCheckbox,
     .about = "Every piece of clothing, tattoo and face paint, and the video venues, without "
              "earning them."},
    {.cvar = "gold_on_all_difficulties", .category = kGameplay, .section = "Unlocks",
     .label = "Gold stars on every difficulty", .widget = kCheckbox,
     .about = "Gold stars can be earned on every difficulty, not only Expert. From the next "
              "song."},
    {.cvar = "game_origin_icons", .category = kGameplay, .section = "Song list",
     .label = "Song source icons", .widget = kCheckbox,
     .about = "Shows an icon in the song list for the game or pack each song came from. "
              "Needs Rock Band 3 Deluxe's icons; shows the next time the song list opens."},
    {.cvar = "forced_venue", .category = kGameplay, .section = "Venue", .label = "Venue",
     .widget = kComboText, .choices = kVenues,
     .about = "Plays every song in this venue instead of the one the game picks, from the "
              "next venue that loads."},

    // Display
    {.cvar = "monitor", .category = kDisplay, .section = "Screen", .label = "Monitor",
     .widget = kMonitor, .choices = kMonitors, .about = "Which display the game opens on."},
    {.cvar = "fullscreen", .category = kDisplay, .section = "Screen", .label = "Window mode",
     .widget = kWindowMode, .companion = "fullscreen_exclusive",
     .about = "Borderless covers the screen like fullscreen and switches away at once. "
              "Exclusive can take a little delay off on some displays."},
    {.cvar = "fullscreen_exclusive", .category = kDisplay, .section = "Screen",
     .label = "Exclusive fullscreen", .widget = kNone},
    {.cvar = "resolution", .category = kDisplay, .section = "Screen", .label = "Window size",
     .widget = kResolution, .choices = kResolutions,
     .about = "The game window's size. The monitor's own sizes come first, then common ones."},
    {.cvar = "present_letterbox", .category = kDisplay, .section = "Screen",
     .label = "Picture shape", .widget = kCombo, .choices = kAspect,
     .about = "On a screen that isn't 16:9, keep the picture's shape with bars at the edges, "
              "or stretch it to fill the screen."},
    {.cvar = "native_fill_window", .category = kDisplay, .section = "Screen",
     .label = "Fill the window", .widget = kCheckbox, .renderers = kWithNative,
     .about = "Draws the game at the window's shape rather than 16:9 with bars: a wider "
              "screen shows more to the sides, the tracks and HUD in the middle."},
    {.cvar = "steam_deck_defaults", .category = kDisplay, .section = "Screen",
     .label = "Steam Deck settings", .widget = kCheckbox, .deck_only = true,
     .about = "Settings that suit the Deck are the defaults (fullscreen, letterboxed, 60 Hz, "
              "vsync on); what you change here wins over them. Off: band3's desktop "
              "defaults."},
    // first: what rhythm players tune for least lag
    {.cvar = "frame_cap", .category = kDisplay, .section = "Smoothness",
     .label = "Frame rate cap", .widget = kComboText, .choices = kFrameCaps,
     .note = "Run the game's calibration again after changing this",
     .about = "Higher means less delay: the game judges your hits once a frame, and can run "
              "above your display's refresh rate. Just under the display suits G-Sync and "
              "FreeSync displays."},
    {.cvar = "native_present_pacing", .category = kDisplay, .section = "Smoothness",
     .label = "Smooth frame pacing", .widget = kCheckbox, .renderers = kWithNative,
     .note = "Run the game's calibration again after changing this",
     .about = "Shows each frame a steady delay after it's drawn, so quick frames don't bunch "
              "up. Off is 3 to 4 ms sooner, with now and then an uneven step. Only with a cap "
              "under 1.5 times the display's rate."},
    {.cvar = "debug_overlay", .category = kDisplay, .section = "Smoothness",
     .label = "FPS counter", .widget = kCheckbox,
     .about = "Shows band3's frame rate counter: the game's rate, which hit timing follows, "
              "and the window's."},
    {.cvar = "launcher_style", .category = kDisplay, .section = "Menus", .label = "Menu style",
     .widget = kCombo, .choices = kMenuStyles,
     .about = "How these menus look. Rock Band 3 uses the game's own textures; Simple is solid "
              "colour boxes on a plain background, easier to read."},

    // Graphics
    {.cvar = "renderer", .category = kGraphics, .section = "Renderer", .label = "Renderer",
     .widget = kCombo, .choices = kRenderers,
     .about = "What draws the picture. Native: band3's own renderer, the default on Windows. "
              "Emulated: the emulated Xbox 360 GPU. Both: the two at once, F8 switching "
              "between their pictures, to compare them."},
    {.cvar = "emulated_gpu_while_native", .category = kGraphics, .section = "Renderer",
     .label = "Emulated GPU's work", .widget = kCombo, .choices = kEmulatedWhileNative,
     .renderers = kForBoth,
     .about = "What the emulated GPU still draws while the native picture shows. Only what's "
              "needed keeps F8 back to its picture right."},
    // retired (settings.cpp's MigrateRendererSettings clears it): not drawn,
    // here so Save removes its key from band3.toml
    {.cvar = "emulated_gpu", .category = kGraphics, .section = "Renderer",
     .label = "Emulated GPU", .widget = kNone},
    {.cvar = "native_view_msaa", .category = kGraphics, .section = "Quality",
     .label = "Anti-aliasing", .widget = kCombo, .choices = kNativeMsaa,
     .renderers = kWithNative,
     .about = "Smooths jagged edges on the track, the HUD and the menus. Each step asks more "
              "of the graphics card."},
    {.cvar = "swap_post_effect", .category = kGraphics, .section = "Quality",
     .label = "Anti-aliasing", .widget = kCombo, .choices = kAntiAliasing,
     .renderers = kWithEmulatedGpu,
     .about = "Smooths jagged edges over the whole picture, a filter over each frame."},
    {.cvar = "native_anisotropic", .category = kGraphics, .section = "Quality",
     .label = "Texture filtering", .widget = kCombo, .choices = kAnisotropic,
     .renderers = kWithNative,
     .about = "Keeps the track and the stage sharp where they lean away from you. Default "
              "keeps the game's own."},
    {.cvar = "anisotropic_override", .category = kGraphics, .section = "Quality",
     .label = "Texture filtering", .widget = kCombo, .choices = kAnisotropic,
     .renderers = kWithEmulatedGpu,
     .about = "Keeps the track and the stage sharp where they lean away from you. Default "
              "keeps the game's own."},
    {.cvar = "native_max_height", .category = kGraphics, .section = "Quality",
     .label = "Render size cap", .widget = kComboText, .choices = kNativeHeights,
     .renderers = kWithNative,
     .about = "The tallest picture drawn before it's scaled up to the window: for a slower "
              "graphics card on a big screen."},
    {.cvar = "native_view_shadow_scale", .category = kGraphics, .section = "Quality",
     .label = "Shadows", .widget = kCombo, .choices = kShadowScales, .renderers = kWithNative,
     .about = "How sharp the shadows characters cast on themselves are."},
    {.cvar = "resolution_scale", .category = kGraphics, .section = "Quality",
     .label = "Render scale", .widget = kIntStepper, .range = Range{1, 8, 1}, .unit = "x",
     .renderers = kWithEmulatedGpu,
     .about = "The emulated GPU draws this many times the console's 1280 x 720 each way. "
              "Each step asks much more of the graphics card."},
    {.cvar = "disable_hair_shader", .category = kGraphics, .section = "Effects",
     .label = "Hair shader", .widget = kCombo, .choices = kOnWhenFalse,
     .about = "The shading on characters' hair. Applies to characters loaded afterwards."},
    {.cvar = "disable_approximate_lights", .category = kGraphics, .section = "Effects",
     .label = "Approximate lighting", .widget = kCombo, .choices = kOnWhenFalse,
     .about = "The game's approximate lighting on characters. Off, the default, works around "
              "a lighting bug."},
    {.cvar = "force_self_shadow", .category = kGraphics, .section = "Effects",
     .label = "Self-shadows in every shot", .widget = kCheckbox,
     .about = "Characters shadow themselves in every camera shot, also the ones that turn it "
              "off. A few shots may show flaws the game avoided there."},
    // ignored with renderer native (Hooks/graphics.cpp)
    {.cvar = "compress_character_textures", .category = kGraphics, .section = "Effects",
     .label = "Compress character textures", .widget = kCheckbox,
     .renderers = kWithEmulatedGpu,
     .about = "Saves graphics memory on characters. Applies to characters loaded afterwards."},
    {.cvar = "rnd_sync", .category = kGraphics, .section = "Timing", .label = "Game frame sync",
     .widget = kCombo, .choices = kFrameSync,
     .about = "The game's own vertical sync, which waits for each frame's turn."},
    {.cvar = "background_fps", .category = kGraphics, .section = "Timing",
     .label = "Venue frame rate", .widget = kIntStepper, .choices = kBackgroundFps,
     .range = Range{0, 240, 5}, .unit = "fps",
     .about = "How often the venue behind the track is drawn. Most venues' own is 30."},
    {.cvar = "vsync", .category = kGraphics, .section = "Timing", .label = "VSync",
     .widget = kCheckbox, .shown_when = kWithoutFrameCap, .renderers = kWithEmulatedGpu,
     .about = "The emulated GPU waits for the display's refresh. Only with the frame rate cap "
              "off."},
    {.cvar = "native_post_effects", .category = kGraphics, .section = "Performance",
     .label = "Bloom, depth of field, motion blur", .widget = kCheckbox,
     .renderers = kWithNative,
     .about = "The camera's glow, focus blur and motion blur. Off is quicker on a slower "
              "graphics card; the venue's colours, spotlights and smoke stay."},
    // on Linux the upload path (native_upload_pipeline, on by default) never
    // draws the world ahead, so the two rows would do nothing there
    {.cvar = "native_world_ahead_auto", .category = kGraphics, .section = "Performance",
     .label = "Draw ahead on integrated GPUs", .widget = kCheckbox, .renderers = kWithNative,
     .windows_only = true,
     .about = "On a laptop's or a handheld's graphics, draws the venue right after each world "
              "frame, so the next frame has less to do: fewer repeated frames and a little less "
              "delay. Off leaves it to the row below."},
    {.cvar = "native_world_ahead", .category = kGraphics, .section = "Performance",
     .label = "Draw ahead on every GPU", .widget = kCheckbox, .renderers = kWithNative,
     .windows_only = true,
     .note = "Experimental. On a fast graphics card at a high refresh rate it can repeat more "
             "frames; band3 checks every few seconds and keeps whichever repeats fewer",
     .about = "Draws the venue ahead on any graphics card, not just integrated ones: worth a "
              "try on an older or slower card."},
    {.cvar = "native_deferred_decode_threads", .category = kGraphics, .section = "Performance",
     .label = "Texture decode threads", .widget = kIntStepper, .choices = kDecodeThreads,
     .range = Range{0, 3, 1}, .renderers = kWithNative,
     .about = "Threads that unpack a song's or a camera cut's new textures and meshes, so the "
              "picture holds for less time. On a processor with four threads or fewer, 1 or "
              "Off leaves more of it to the game."},

    // Audio
    {.cvar = "audio_maxqframes", .category = kAudio, .section = "Output",
     .label = "Audio buffer", .widget = kIntStepper, .range = Range{4, 64, 1},
     .unit = "frames", .about = "Smaller is less delay. Raise it if the sound crackles."},
    {.cvar = "usb_mics", .category = kAudio, .section = "Microphones",
     .label = "PC microphones", .widget = kCheckbox,
     .about = "Sing through microphones on this PC (USB mics, headsets, an audio interface) "
              "as the console's USB mics. Turn it on to pick them."},
    {.cvar = "usb_mic_devices", .category = kAudio, .section = "Microphones",
     .label = "Microphones", .widget = kMicSlots, .shown_when = kWithUsbMics,
     .about = "The microphone each singer uses. With none picked, the first singer uses the "
              "system's default."},
    {.cvar = "usb_mic_monitor", .category = kAudio, .section = "Microphones",
     .label = "Hear yourself sooner", .widget = kCheckbox, .shown_when = kWithUsbMics,
     .about = "band3 plays the mics to the speakers itself: you hear yourself in about 30 ms "
              "instead of 80. The game's gentle pitch correction isn't heard."},

    // Controllers
    {.cvar = "controller_type", .category = kControllers, .section = "Instruments",
     .label = "Gamepads play as", .widget = kCombo, .choices = kControllerTypes,
     .about = "What ordinary gamepads and the keyboard play as. Instruments that say what "
              "they are keep it."},
    {.cvar = "guitar_type", .category = kControllers, .section = "Instruments",
     .label = "Guitars play as", .widget = kCombo, .choices = kGuitarTypes,
     .about = "Rock Band guitars pick the overdrive effect with their switch; Guitar Hero "
              "guitars have none. Auto goes by what each guitar says it is."},
    {.cvar = "hid_instruments", .category = kControllers, .section = "Instruments",
     .label = "Other consoles' instruments", .widget = kCheckbox,
     .about = "Plays PlayStation, Wii and Xbox One Rock Band instruments through their USB "
              "dongles, as Xbox 360 ones."},
    {.cvar = "input_backend", .category = kControllers, .section = "Instruments",
     .label = "Controller reading", .widget = kCombo, .choices = kInputBackends,
     .windows_only = true,
     .about = "What reads the controllers. Change it only if one isn't found."},
    {.cvar = "mouse_menus", .category = kControllers, .section = "Menus",
     .label = "Mouse in menus", .widget = kCheckbox,
     .about = "Point at a button to highlight it: left click picks, right click goes back, "
              "the wheel scrolls. Off during songs."},
    {.cvar = "keyboard_search", .category = kControllers, .section = "Menus",
     .label = "Type to search", .widget = kCheckbox,
     .about = "Type on the song list to search it. Needs a Rock Band 3 Deluxe with keyboard "
              "search; off during songs."},
    {.cvar = "menu_shortcut", .category = kControllers, .section = "Menus",
     .label = "Stick-click shortcut", .widget = kCheckbox,
     .about = "Hold both stick clicks for a second to open the pause menu in game, or both "
              "stick clicks and LB for the Instrument Lab."},
    {.cvar = "left_stick_deadzone_percentage", .category = kControllers, .section = "Sticks",
     .label = "Left stick deadzone", .widget = kPercentSlider, .range = Range{0, 1, 0.01},
     .about = "How far the stick moves before it counts."},
    {.cvar = "right_stick_deadzone_percentage", .category = kControllers, .section = "Sticks",
     .label = "Right stick deadzone", .widget = kPercentSlider, .range = Range{0, 1, 0.01},
     .about = "How far the stick moves before it counts."},
    {.cvar = "joypad_lag", .category = kControllers, .section = "Delay",
     .label = "Delay per controller type", .widget = kJoypadLag, .unit = "ms",
     .about = "The delay the game allows each kind of controller, for one that lags behind "
              "the rest. Empty keeps the game's own."},

    // MIDI
    {.cvar = "midi_drums", .category = kMidi, .section = "Drums", .label = "MIDI drum kit",
     .widget = kCheckbox,
     .about = "Play an electronic drum kit through MIDI as a pro drum kit, without a MIDI Pro "
              "Adapter. Turn it on to pick its port and pads."},
    {.cvar = "midi_drums_device", .category = kMidi, .section = "Drums",
     .label = "Drum kit port", .widget = kMidiPort, .shown_when = kWithMidiDrums,
     .about = "The kit's MIDI port. The first port shows which one band3 takes by itself."},
    {.cvar = "midi_drums_min_velocity", .category = kMidi, .section = "Drums",
     .label = "Minimum velocity", .widget = kIntSlider, .range = Range{1, 127, 1},
     .shown_when = kWithMidiDrums, .about = "Hits quieter than this are ignored."},
    {.cvar = "midi_drums_combos", .category = kMidi, .section = "Drums",
     .label = "Menu buttons from the kit", .widget = kCheckbox, .shown_when = kWithMidiDrums,
     .about = "Hi-hat pedal three times, then the snare for Start, the rim for Select, or the "
              "kick to hold the kick."},
    {.cvar = "midi_drums_notes", .category = kMidi, .section = "Drums",
     .label = "Note overrides", .widget = kText, .shown_when = kWithMidiDrums,
     .about = "Which pad a note plays, as note=Part, comma separated (44=Kick,40=Snare). "
              "Parts: Kick, HihatPedal, Snare, SnareRim, HiTom, LowTom, FloorTom, Hihat, Ride, "
              "Crash, None."},
    {.cvar = "midi_drums_pulse_ms", .category = kMidi, .section = "Drums", .label = "Hit length",
     .widget = kIntStepper, .range = Range{20, 100, 1}, .unit = "ms",
     .shown_when = kWithMidiDrums,
     .about = "How long each hit is held. Two cymbals hit together are played this far apart."},
    {.cvar = "midi_keys", .category = kMidi, .section = "Keyboard", .label = "MIDI keyboard",
     .widget = kCheckbox,
     .about = "Play a MIDI keyboard as a Rock Band keytar, for Keys and Pro Keys. Turn it on "
              "to pick its port."},
    {.cvar = "midi_keys_device", .category = kMidi, .section = "Keyboard",
     .label = "Keyboard port", .widget = kMidiPort, .shown_when = kWithMidiKeys,
     .about = "The keyboard's MIDI port. The first port shows which one band3 takes by "
              "itself."},
    {.cvar = "midi_keys_base_note", .category = kMidi, .section = "Keyboard", .label = "Lowest C",
     .widget = kMidiNote, .range = Range{0, 103, 1}, .shown_when = kWithMidiKeys,
     .about = "The key the keytar's lowest C is on: 48 (C3) on most 25-key keyboards."},
    // the test harness's (`midi`), which nobody playing wants: not drawn, and
    // here so it isn't generated into the Advanced category
    {.cvar = "midi_keys_test_device", .category = kMidi, .section = "Keyboard",
     .label = "Harness keyboard", .widget = kNone},

    // Music Videos
    {.cvar = "music_videos", .category = kVideos, .section = "Playback", .label = "Music videos",
     .widget = kCheckbox,
     .about = "Plays a song's music video in the video venues, when the videos folder has "
              "one for it."},
    {.cvar = "music_video_venue_chance", .category = kVideos, .section = "Playback",
     .label = "Video venue", .widget = kCombo, .choices = kVideoVenueChances,
     .about = "How often a song with a video is put in a video venue, so its video shows."},
    {.cvar = "music_video_hide_band", .category = kVideos, .section = "Playback",
     .label = "Hide the band", .widget = kCheckbox,
     .about = "Leaves the band out while a video plays, so only the video shows behind the "
              "tracks."},
    {.cvar = "music_video_as_is", .category = kVideos, .section = "Playback",
     .label = "Without the venue's filters", .widget = kCheckbox,
     .about = "Shows a video as it is, without the video venue's colour filter, grain and "
              "glow."},
    {.cvar = "music_video_full_rate", .category = kVideos, .section = "Playback",
     .label = "Smooth videos", .widget = kCheckbox,
     .about = "Draws the venue every frame while a video plays, so each of its frames shows "
              "evenly, which takes about twice the drawing. Off, at the venue's 24 or 30."},
    {.cvar = "music_video_auto_sync", .category = kVideos, .section = "Playback",
     .label = "Sync videos by themselves", .widget = kCheckbox,
     .about = "Matches each video's sound against its song's when the song is picked, and "
              "keeps a sure match."},
    {.cvar = "music_videos_folder", .category = kVideos, .section = "Playback",
     .label = "Videos folder", .widget = kPath, .about = "Where the music videos are kept."},
    {.cvar = "music_video_fit", .category = kVideos, .section = "Playback", .label = "Video shape",
     .widget = kCombo, .choices = kVideoFits,
     .about = "How a video whose shape isn't the screen's fills it."},
    {.cvar = "music_video_ytdlp_path", .category = kVideos, .section = "Finding videos",
     .label = "yt-dlp program", .widget = kText,
     .about = "The program band3 finds and downloads videos with (yt-dlp.exe). Empty looks "
              "for it on PATH. Get it from github.com/yt-dlp/yt-dlp."},
    {.cvar = "music_video_ytdlp_args", .category = kVideos, .section = "Finding videos",
     .label = "More yt-dlp options", .widget = kText,
     .about = "Options added to yt-dlp's command line, e.g. --cookies-from-browser firefox "
              "for videos YouTube asks to sign in for."},
    {.cvar = "music_video_max_height", .category = kVideos, .section = "Finding videos",
     .label = "Picture size", .widget = kCombo, .choices = kVideoHeights,
     .about = "The largest picture a video is downloaded at. Larger ones take more space and "
              "decoding."},
    {.cvar = "music_video_prefer_60fps", .category = kVideos, .section = "Finding videos",
     .label = "Download 60 fps videos", .widget = kCheckbox,
     .about = "Downloads a video's 60 fps picture when it has one; off, its 30 fps one."},
    {.cvar = "music_video_auto_accept", .category = kVideos, .section = "Finding videos",
     .label = "Accept clear matches", .widget = kCheckbox,
     .about = "Takes a video without asking when it's surely the song's. Off, every song's "
              "results wait for you to choose."},
    {.cvar = "music_video_keep_audio", .category = kVideos, .section = "Finding videos",
     .label = "Keep the videos' sound", .widget = kCheckbox,
     .about = "Keeps a downloaded video's sound, so it can be synced again later. Off, it's "
              "deleted once the video is synced."},

    // Lights: what the Stage Kits found are, and the rows that test them,
    // follow (the Settings screen's)
    {.cvar = "stagekit_usb", .category = kLights, .section = "Stage Kits",
     .label = "USB Stage Kits", .widget = kCheckbox,
     .about = "Lights the Stage Kits plugged into this PC as the game lights them."},
    {.cvar = "pico_discovery", .category = kLights, .section = "Stage Kits",
     .label = "Wireless Stage Kits", .widget = kCheckbox,
     .about = "Finds the RB3E Dashboard's wireless Stage Kits (Pico W) on the network. They "
              "follow the game through the RB3E events, in Other Apps. Turn it off while the "
              "RB3E Dashboard runs."},
    {.cvar = "black_background_lights", .category = kLights, .section = "Stage Kits",
     .label = "Black background", .widget = kCheckbox,
     .about = "With the Black Background modifier, keeps a venue drawn black, so its lights "
              "still reach the Stage Kits."},

    // Online
    {.cvar = "liveless", .category = kOnline, .section = "Online play", .label = "Play online",
     .widget = kCheckbox,
     .about = "Play online with other band3 and RB3Enhanced players, without Xbox Live."},
    {.cvar = "liveless_connect", .category = kOnline, .section = "Online play",
     .label = "Game to join", .widget = kText, .shown_when = kWithLiveless,
     .about = "The address of the player whose game you join, with :port if it isn't 9103. "
              "127.0.0.1 hosts."},
    {.cvar = "liveless_external_ip", .category = kOnline, .section = "Online play",
     .label = "Your address", .widget = kText, .shown_when = kWithLiveless,
     .about = "Where players joining you reach you: your public IP for players over the "
              "internet. Empty: this PC's address on your network."},
    {.cvar = "liveless_port", .category = kOnline, .section = "Online play",
     .label = "Online play port", .widget = kIntStepper, .range = kGamePorts,
     .shown_when = kWithLiveless,
     .about = "The port online play uses, which players joining you need open. Another one "
              "lets a second band3 on this PC join the first."},
    {.cvar = "liveless_port_mapping", .category = kOnline, .section = "Online play",
     .label = "Open the port on the router", .widget = kCheckbox, .shown_when = kWithLiveless,
     .about = "Asks the router to forward the port to this PC, so players over the internet "
              "reach you without setting it up by hand."},
    {.cvar = "liveless_rooms", .category = kOnline, .section = "Online play", .label = "Rooms",
     .widget = kCheckbox, .shown_when = kWithLiveless,
     .about = "Join friends by a room code through a Liveless Rooms server, instead of "
              "typing an address."},
    {.cvar = "liveless_rooms_server", .category = kOnline, .section = "Online play",
     .label = "Rooms server", .widget = kText, .shown_when = kWithRooms,
     .about = "The Liveless Rooms server. Logging in registers your profile name there."},
    {.cvar = "gocentral", .category = kOnline, .section = "GoCentral",
     .label = "Leaderboards and battles", .widget = kCheckbox,
     .about = "Connects to GoCentral, the fan-run server RB3Enhanced uses, for leaderboards, "
              "battles and shared setlists. Your profile name is your account there."},
    {.cvar = "gocentral_address", .category = kOnline, .section = "GoCentral", .label = "Server",
     .widget = kText, .shown_when = kWithGoCentral,
     .about = "The GoCentral server: RB3Enhanced's, or your own."},

    // Other Apps
    {.cvar = "http_enabled", .category = kOtherApps, .section = "Web song browser",
     .label = "Web song browser", .widget = kCheckbox,
     .about = "Browse the song library from a phone or another PC on your network, and pick "
              "songs in the Music Library from there."},
    {.cvar = "http_port", .category = kOtherApps, .section = "Web song browser",
     .label = "Web page port", .widget = kIntStepper, .range = kPorts, .shown_when = kWithHttp,
     .about = "The port the web page is served on."},
    {.cvar = "http_rhythmverse", .category = kOtherApps, .section = "Web song browser",
     .label = "RhythmVerse downloads", .widget = kCheckbox, .shown_when = kWithHttp,
     .about = "Search RhythmVerse for custom songs from the web page and download them into "
              "your first song folder. They join the game without a restart."},
    {.cvar = "http_qr_code", .category = kOtherApps, .section = "Web song browser",
     .label = "QR code in the Music Library", .widget = kCheckbox, .shown_when = kWithHttp,
     .about = "Shows a QR code for the web page at the top of the Music Library, so a phone "
              "can open it by scanning the screen."},
    {.cvar = "http_address", .category = kOtherApps, .section = "Web song browser",
     .label = "Who can reach it", .widget = kComboText, .choices = kHttpAddresses,
     .shown_when = kWithHttp, .about = "Who can open the web page."},
    {.cvar = "http_allow_cors", .category = kOtherApps, .section = "Web song browser",
     .label = "Other sites (CORS)", .widget = kCheckbox, .shown_when = kWithHttp,
     .about = "Lets web pages from other sites use the page's API (cross-origin requests, "
              "CORS)."},
    {.cvar = "http_allow_scripts", .category = kOtherApps, .section = "Web song browser",
     .label = "Scripts from the page", .widget = kCheckbox, .shown_when = kWithHttp,
     .note = "Only on a network you trust",
     .about = "Lets the page run scripts in the game. Anyone on your network could then run "
              "any script."},
    {.cvar = "lastfm_enabled", .category = kOtherApps, .section = "Scrobbling",
     .label = "Last.fm", .widget = kCheckbox,
     .about = "Scrobbles the songs you play to Last.fm. The first time, band3 opens Last.fm "
              "in the browser to ask for your permission."},
    {.cvar = "lastfm_session", .category = kOtherApps, .section = "Scrobbling",
     .label = "Last.fm sign-in", .widget = kPassword, .shown_when = kWithLastfm,
     .about = "The session Last.fm gave band3. Clear it to sign in again, as another user."},
    {.cvar = "listenbrainz_enabled", .category = kOtherApps, .section = "Scrobbling",
     .label = "ListenBrainz", .widget = kCheckbox,
     .about = "Submits the songs you play to ListenBrainz."},
    {.cvar = "listenbrainz_token", .category = kOtherApps, .section = "Scrobbling",
     .label = "ListenBrainz token", .widget = kPassword, .shown_when = kWithListenBrainz,
     .about = "Your user token, from listenbrainz.org/settings."},
    {.cvar = "rb3e_mode", .category = kOtherApps, .section = "RB3Enhanced",
     .label = "RB3E mode", .widget = kCheckbox,
     .about = "Tells the game's scripts RB3Enhanced is running, so Rock Band 3 Deluxe turns "
              "on its RB3E features."},
    {.cvar = "events_enabled", .category = kOtherApps, .section = "RB3Enhanced",
     .label = "Send events", .widget = kCheckbox,
     .about = "Sends RB3E's events (song, score, lights) over the network, for wireless "
              "Stage Kits and stream overlays."},
    {.cvar = "events_target", .category = kOtherApps, .section = "RB3Enhanced",
     .label = "Send to", .widget = kText, .shown_when = kWithEvents,
     .about = "Where the events go. 255.255.255.255 is everyone on your network."},
    {.cvar = "events_port", .category = kOtherApps, .section = "RB3Enhanced",
     .label = "Events port", .widget = kIntStepper, .range = kPorts, .shown_when = kWithEvents,
     .about = "The port the events are sent to."},
    {.cvar = "ha_mqtt_host", .category = kOtherApps, .section = "Home Assistant",
     .label = "MQTT broker", .widget = kText,
     .about = "Your Home Assistant's MQTT broker (MQTT is how Home Assistant hears from "
              "devices). Setting one tells it the song, score and screen."},
    {.cvar = "ha_mqtt_port", .category = kOtherApps, .section = "Home Assistant",
     .label = "MQTT port", .widget = kIntStepper, .range = kPorts, .shown_when = kWithHaBroker,
     .about = "The broker's port: 1883 unless it was changed."},
    {.cvar = "ha_mqtt_username", .category = kOtherApps, .section = "Home Assistant",
     .label = "MQTT user name", .widget = kText, .shown_when = kWithHaBroker,
     .about = "The broker's user name. Empty connects without one."},
    {.cvar = "ha_mqtt_password", .category = kOtherApps, .section = "Home Assistant",
     .label = "MQTT password", .widget = kPassword, .shown_when = kWithHaBroker,
     .about = "The broker's password, sent only with a user name."},
    {.cvar = "ha_discovery_prefix", .category = kOtherApps, .section = "Home Assistant",
     .label = "Discovery prefix", .widget = kText, .shown_when = kWithHaBroker,
     .about = "Home Assistant's MQTT discovery prefix: homeassistant unless it was changed."},
    {.cvar = "ha_stagekit", .category = kOtherApps, .section = "Home Assistant",
     .label = "Stage Kit lights", .widget = kCheckbox, .shown_when = kWithHaBroker,
     .about = "Also tells Home Assistant the Stage Kit's lights, strobe and fog. They change "
              "many times a second: leave them out of its recorder."},
    {.cvar = "ha_webhook_url", .category = kOtherApps, .section = "Home Assistant",
     .label = "Webhook URL", .widget = kText,
     .about = "A Home Assistant webhook told when a song starts, ends and changes, as the "
              "RB3E Dashboard tells it."},
    {.cvar = "discord_enabled", .category = kOtherApps, .section = "Discord",
     .label = "Discord status", .widget = kCheckbox,
     .about = "Shows the song you're playing on your Discord profile. Needs the Discord app."},
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

std::span<const Category> Categories() {
    static constexpr Category kOrder[] = {
        Category::kGame,     Category::kGameplay, Category::kDisplay,     Category::kGraphics,
        Category::kAudio,    Category::kControllers, Category::kMidi,     Category::kVideos,
        Category::kLights,   Category::kOnline,   Category::kOtherApps,   Category::kAdvanced,
    };
    return kOrder;
}

std::string_view CategoryName(Category category) {
    switch (category) {
    case Category::kGame: return "Game";
    case Category::kGameplay: return "Gameplay";
    case Category::kDisplay: return "Display";
    case Category::kGraphics: return "Graphics";
    case Category::kAudio: return "Audio";
    case Category::kControllers: return "Controllers";
    case Category::kMidi: return "MIDI";
    case Category::kVideos: return "Music Videos";
    case Category::kLights: return "Lights";
    case Category::kOnline: return "Online";
    case Category::kOtherApps: return "Other Apps";
    case Category::kAdvanced: return "Advanced";
    }
    return "";
}

std::string_view CategoryAbout(Category category) {
    switch (category) {
    case Category::kGame: return "Where the game, your songs and your saves are, your profile, and how band3 starts.";
    case Category::kGameplay: return "Song speed, what's unlocked, the song list and the venue.";
    case Category::kDisplay: return "The window or full screen, which monitor, and how smoothly frames reach it.";
    case Category::kGraphics: return "How the game is drawn: the renderer, its quality and its effects.";
    case Category::kAudio: return "Where the sound goes, and the microphones singers use.";
    case Category::kControllers: return "What gamepads and guitars play as, the menus, the sticks and each one's delay.";
    case Category::kMidi: return "A MIDI drum kit or keyboard: its port, its pads and its notes.";
    case Category::kVideos: return "Music videos behind the band: how they play, and how band3 finds them.";
    case Category::kLights: return "Stage Kits, plugged in or over the network, and a way to try them.";
    case Category::kOnline: return "Playing online with other bands, and the GoCentral server.";
    case Category::kOtherApps: return "The web song browser, scrobbling, RB3Enhanced, Home Assistant and Discord.";
    case Category::kAdvanced: return "Every other setting by its own name, for troubleshooting.";
    }
    return "";
}

std::span<const Setting> SettingTable() { return kSettings; }

const Setting* FindSetting(std::span<const Setting> table, std::string_view cvar) {
    const auto it = std::ranges::find(table, cvar, &Setting::cvar);
    return it == table.end() ? nullptr : &*it;
}

std::vector<std::string_view> SectionsOf(std::span<const Setting> table, Category category) {
    std::vector<std::string_view> sections;
    for (const auto& s : table) {
        if (s.category == category && std::ranges::find(sections, s.section) == sections.end()) {
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

bool LowestLatencyPacingOff(double display_hz) {
    const std::string cap = LowestLatencyCap(display_hz);
    return pacing::PacePublishing(pacing::FrameCapMode::kFixed, std::stod(cap), display_hz);
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
        return {.device_tester = false, .mic_meters = false, .next_start_notes = true};
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
    if (setting.deck_only && !env_.steam_deck) return false;
    if (!ForRenderer(setting.renderers)) return false;
    if (setting.shown_when.cvar.empty()) return true;
    const CvarFacts* facts = Facts(setting.shown_when.cvar);
    if (!facts || !facts->exists) return true;
    // the row that controls it hidden hides it too (Rooms' server, with online play off)
    if (const Setting* controller = Find(setting.shown_when.cvar);
        controller && controller != &setting && !Visible(*controller)) {
        return false;
    }
    if (setting.shown_when.not_empty) return !Value(setting.shown_when.cvar).empty();
    return SameValue(facts->type, Value(setting.shown_when.cvar), setting.shown_when.value);
}

std::string SettingsModel::Label(const Setting& setting) const {
    // a row for one renderer, with both running
    const bool one_renderer = setting.renderers == (kForNative | kForBoth) ||
                              setting.renderers == (kForEmulated | kForBoth);
    if (!one_renderer || !ForRenderer(kForBoth) || ForRenderer(kForNative | kForEmulated)) {
        return std::string(setting.label);
    }
    const bool twin = std::ranges::any_of(table_, [&](const Setting& other) {
        return &other != &setting && other.category == setting.category &&
               other.label == setting.label && other.renderers != setting.renderers;
    });
    if (!twin) return std::string(setting.label);
    return std::string(setting.label) +
           (setting.renderers & kForNative ? " (native)" : " (emulated)");
}

int SettingsModel::ChangedIn(Category category) const {
    int count = 0;
    for (const Setting& s : table_) {
        if (s.category == category && Visible(s) && IsChanged(s.cvar)) count++;
    }
    return count;
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

std::optional<std::string> SettingsModel::SectionNote(Category category,
                                                      std::string_view section) const {
    // the game's calibration takes up what lag is left, as long as it doesn't change
    if (category == Category::kDisplay && section == "Smoothness") {
        return "After changing these, run the game's calibration again (in its Options), so "
               "your hits are judged against the delay you have now.";
    }
    // the emulated GPU's own settings are the plugin's, there only once it runs
    if (category == Category::kGraphics && section == "Quality" && !env_.emulated_gpu_running &&
        ForRenderer(kWithEmulatedGpu)) {
        return env_.in_game
                   ? "The emulated GPU's own settings (render scale, anti-aliasing, texture "
                     "filtering, VSync) show here once band3 runs with it: the new renderer "
                     "applies at the next start."
                   : "The emulated GPU's own settings (render scale, anti-aliasing, texture "
                     "filtering, VSync) show here once band3 runs with it: Play restarts band3 "
                     "for the new renderer.";
    }
    // the tries act on the kits at once, and none of them is a setting
    if (category == Category::kLights && section == "Try the lights") {
        return "These play on the kits now, to check them; they aren't saved.";
    }
    // the game has its folders: the game data, the user data and the songs
    if (env_.in_game && category == Category::kGame && section == "Folders") {
        return "The game took its folders as it started: change them from the launcher.";
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
    // the startup box is the player's either way (ShowAtStartup), written every save
    if (!setting || cvar == kShowLauncher) return false;
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
    const Setting* setting = Find(cvar);
    if (setting && setting->command_line_only) return true;
    if (!env_.in_game) return false;
    const CvarFacts* facts = Facts(cvar);
    return (facts && facts->lifecycle == Lifecycle::kInitOnly) ||
           (setting && setting->launcher_only);
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
    // the startup box says what the next start does, nothing to wait for
    if (!env_.in_game || cvar == kShowLauncher) return false;
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
        return "Set the profile name (Settings > Game) to a name of your own first: it's your "
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
