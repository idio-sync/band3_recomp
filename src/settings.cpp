#include "settings.h"
#include "renderer_default.h"
#include "Game/heap_report.h"
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

// a heap size up to what a heap can be (heap_report.h's kMaxHeapSize): more
// fails at boot
static bool ValidHeapSize(std::string_view v) {
    double d;
    return rex::cvar::ParseDouble(v, d) && d >= 0 && d <= band3::heap_report::kMaxHeapSize;
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

REXCVAR_DEFINE_BOOL(music_videos, true, "Band3/Videos",
    "Play a song's music video in the video venues, in place of their background clips, "
    "when music_videos_folder has one for it: <shortname>.mp4 (or .mkv, .mov, .webm...), "
    "and optionally <shortname>.ini with Clone Hero's video_start_time. "
    "forced_venue video picks the video venues. Turned on, it applies from the next song");

REXCVAR_DEFINE_INT32(music_video_venue_chance, 100, "Band3/Videos",
    "How often a song with a music video is put in a video venue, so its video shows, in "
    "percent: 0 never (the game's venue), 25, 50, 75 or 100 always. A forced venue and the "
    "black background modifier still win. Applies from the next venue the game picks")
    .range(0, 100);

REXCVAR_DEFINE_BOOL(music_video_hide_band, true, "Band3/Videos",
    "Leave the band out of the picture while a song's music video plays, so only the "
    "video shows behind the highways. The venue's lights (and Stage Kit) go on as before");

REXCVAR_DEFINE_BOOL(music_video_as_is, true, "Band3/Videos",
    "Show a song's music video as it is: the video venue's colour filter, film grain, glow "
    "and trails are left off while it plays (and on the black background's black)");

REXCVAR_DEFINE_BOOL(music_video_full_rate, true, "Band3/Videos",
    "Play a music video faster than the venue's 30 fps (a 60 fps one) at its own frame rate: "
    "the video venue is drawn that fast while it plays (as background_fps would, up to the "
    "game's rate), which takes about twice the drawing and decoding. Off, it shows at the "
    "venue's rate");

REXCVAR_DEFINE_BOOL(music_video_auto_sync, true, "Band3/Videos",
    "Find each music video's offset from its sound: when its song is picked, the song's "
    "audio (read from its package or the game's archive, decrypted if need be, or for a "
    "song band3 can't read taken as it first plays, about 45 s in) is matched against the "
    "video's and a sure match is saved as video_start_time in the video's .ini, and used at "
    "once. An .ini that sets one is left alone");

REXCVAR_DEFINE_STRING(music_videos_folder, "videos", "Band3/Videos",
    "The folder music videos are looked for in (empty: videos). A relative folder is "
    "relative to band3_config.ini's folder (or band3's own folder if there is no ini)");

REXCVAR_DEFINE_STRING(music_video_ytdlp_path, "", "Band3/Videos",
    "yt-dlp, which finds and downloads songs' music videos from the Music Videos screen: its program "
    "file (yt-dlp.exe). Empty looks for yt-dlp on PATH. band3 doesn't come with it: get it "
    "from github.com/yt-dlp/yt-dlp");

REXCVAR_DEFINE_STRING(music_video_ytdlp_args, "", "Band3/Videos",
    "More yt-dlp options for finding and downloading music videos, as typed on its command "
    "line, e.g. --cookies-from-browser firefox for videos YouTube asks to sign in for");

REXCVAR_DEFINE_INT32(music_video_max_height, 1080, "Band3/Videos",
    "The tallest picture a downloaded music video has, in lines: 480, 720, 1080, 1440 or "
    "2160. Taller ones take more space and decoding")
    .range(144, 4320);

REXCVAR_DEFINE_BOOL(music_video_prefer_60fps, true, "Band3/Videos",
    "Download a music video's 60 fps picture where it has one (music_video_full_rate plays "
    "it at 60); off, its 30 fps one");

REXCVAR_DEFINE_BOOL(music_video_auto_accept, true, "Band3/Videos",
    "When finding music videos, take one without asking when it's surely the song's: its "
    "title, channel and length look right and its sound matches the song's. Off, every "
    "song's results wait on the Music Videos screen for you to choose");

REXCVAR_DEFINE_BOOL(music_video_keep_audio, false, "Band3/Videos",
    "Keep a downloaded music video's sound (in the videos folder's .sync folder), so it can "
    "be aligned again with its song; band3 plays the video under the game's own sound, so "
    "it's otherwise deleted once the video is synced");

REXCVAR_DEFINE_STRING(music_video_fit, "fit", "Band3/Videos",
    "How a music video whose shape isn't the screen's is shown: fit (all of it, with black "
    "bars), fill (the whole screen, its edges cut off) or stretch");

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

REXCVAR_DEFINE_BOOL(score_history, true, "Band3/Game",
    "Keep every finished song's results (score, stars, notes hit, streak, overdrive) in "
    "score_history.jsonl in the user data folder, for the web page's History tab and its "
    "played and stars filters. Songs played on autoplay aren't kept");

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
    "(the default on Windows, and on a Steam Deck through its presets). emulated: the "
    "emulated GPU alone (the default elsewhere). "
    "both, Native + emulated (debug): the two, F8 switching between their pictures. "
    "Native to or from the others applies at the next start. Band3/Graphics/Native applies "
    "to native and both; Band3/Graphics/Emulated and the emulated GPU's own (under GPU, "
    "there only in a run with it) to emulated and both")
    .allowed({"native", "emulated", "both"});

REXCVAR_DEFINE_INT32(rnd_sync, 1, "Band3/Graphics",
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
    "counts it as if the game ran at 60, so at a video_mode_refresh_rate of 120 a venue's 30 "
    "drew at 60")
    .range(0, 240);

// src/Hooks/frame_pacing.h says what the cap does in place of the vblank
REXCVAR_DEFINE_STRING(frame_cap, "display", "Band3/Graphics",
    "What paces the game's frames. display: the display's refresh rate exactly (119.88 Hz, "
    "not 120), for fixed-refresh displays (the default). auto: a little under it (5% less, "
    "at least 4 fps), for VRR displays (G-Sync, FreeSync), keeping each frame inside their "
    "range; on a fixed-refresh display a cap under the refresh rate shows a frame twice "
    "every 1/(refresh - cap) seconds. A number of Hz (24 to 240), e.g. 117. off: the "
    "emulated console's vertical blank, paced by vsync and video_mode_refresh_rate (60 "
    "unless set). With the cap on, vsync is turned off and an unset video_mode_refresh_rate "
    "follows the cap. Without a display whose rate can be told, display and auto are off")
    .validator([](std::string_view v) { return band3::pacing::ParseFrameCap(v).has_value(); });

REXCVAR_DEFINE_BOOL(debug_overlay, true, "Band3/Graphics",
    "Show band3's FPS counter: the game's frame rate (what frame_cap paces, and hit timing "
    "follows) and the window's, which the display's refresh rate holds back");

// Band3/Graphics/Native: the native renderer, with renderer native or both

REXCVAR_DEFINE_INT32(native_max_height, 0, "Band3/Graphics/Native",
    "Native renderer (renderer native or both): the most lines it draws: a window taller "
    "than this has its picture drawn this tall and scaled up to fill it, for 4K on a GPU "
    "that can't draw it at full size. 0 = the window's size")
    .range(0, 4320);

// read by src/Render/native_view.cpp, which hands it to src/Hooks/aspect.cpp
REXCVAR_DEFINE_BOOL(native_fill_window, true, "Band3/Graphics/Native",
    "Native renderer (renderer native or both): draw the game at the window's shape rather "
    "than 16:9 with black bars. A wider window (21:9) shows more to the sides, a taller one "
    "(16:10) more above and below, with the HUD and tracks at their size in the middle 16:9. "
    "The emulated GPU's picture stays 16:9");

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

// read by src/Render/native_view.cpp and src/Test/test_server.cpp's captures (post_model.h's
// PostOnlyFor)
REXCVAR_DEFINE_BOOL(native_post_effects, true, "Band3/Graphics/Native",
    "Native renderer (renderer native or both): the camera's post-processing effects, bloom "
    "(and glare), depth of field and motion blur. Off leaves them out for a slower GPU; the "
    "venue's colours, the spotlights' haze, soft particles and film grain stay. On a Radeon "
    "680M at 1920x1200 in arena_04 that's 1.5 ms less GPU work a post frame (busy 11.8 to "
    "10.7 ms). Applies from the next frame");

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
    "slot (up to 3). Empty uses the system's default recording device")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(usb_mic_monitor, false, "Band3/Audio",
    "band3 plays the microphones to the speakers itself, with the game's mic EQ and compressor "
    "at its mic volume, and the game adds only its reverb and overdrive echo: you hear yourself "
    "in roughly 30 ms instead of 80. The game's gentle pitch correction (Solo vocals) isn't "
    "heard");

// Band3/Controllers

REXCVAR_DEFINE_INT32(controller_type, 7, "Band3/Controllers",
    "Instrument gamepads and the keyboard play as: -1 = don't override, 1 = vocals, "
    "7 = guitar, 8 = drums. Instruments that report their own type (Xbox 360 instruments "
    "with input_backend = xinput, the virtual instrument) keep it")
    .range(-1, 255)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(guitar_type, "auto", "Band3/Controllers",
    "What RB3 takes guitars for: auto = what each reports itself, rock_band = Rock Band "
    "guitars, whose pickup switch picks the overdrive effect, guitar_hero = Guitar Hero "
    "guitars, which have no effect switch. A Guitar Hero guitar played as a Rock Band one "
    "changes the effect when it's turned face up. auto can't tell on Linux, or for a guitar "
    "that isn't an Xbox 360 one; choose guitar_hero for a Guitar Hero guitar there")
    .allowed({"auto", "rock_band", "guitar_hero"})
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(hid_instruments, false, "Band3/Controllers",
    "Experimental: play PS3, Wii, PS4 and PS5 Rock Band guitars and drum kits (and the "
    "MIDI Pro Adapter in drum mode) through their USB dongles, and on Windows Xbox One ones "
    "through GameInput 3, as Xbox 360 instruments")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(menu_shortcut, true, "Band3/Controllers",
    "Open menus from a controller: hold both stick clicks for a second for this settings "
    "menu, or both stick clicks and the left bumper for the Instrument Lab");

REXCVAR_DEFINE_BOOL(mouse_menus, true, "Band3/Controllers",
    "Use the mouse in the game's menus: pointing at a button or a list's row highlights it, "
    "left click selects (A), right click goes back (B), and the wheel moves up and down. "
    "Off during songs, where A and B are frets");

REXCVAR_DEFINE_BOOL(keyboard_search, true, "Band3/Controllers",
    "Type on the song list to search it, as RB3Enhanced does on the Xbox 360. Needs a Rock "
    "Band 3 Deluxe with keyboard search. There the keys that type stop pressing their "
    "controller buttons, and while the search is typed in, the arrows, Enter and Backspace "
    "edit it. Off during songs");

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
    "MIDI input port to play, or part of its name. Empty takes the first one no other band3 "
    "MIDI device has")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(midi_drums_notes, "", "Band3/Controllers/MIDI drums",
    "Note overrides as note=Part, comma separated (e.g. 44=Kick,40=Snare), in RPCS3's "
    "format. Parts: Kick, HihatPedal, Snare, SnareRim, HiTom, LowTom, FloorTom, Hihat, "
    "Ride, Crash, None")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(midi_drums_pulse_ms, 30, "Band3/Controllers/MIDI drums",
    "How long each hit is held, in milliseconds. Two cymbals hit together are played one "
    "pulse apart, so it must outlast a game frame")
    .range(20, 100)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(midi_drums_min_velocity, 10, "Band3/Controllers/MIDI drums",
    "Quieter hits than this (1-127) are ignored")
    .range(1, 127)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(midi_drums_combos, true, "Band3/Controllers/MIDI drums",
    "Menu buttons from the kit: hi-hat pedal three times, then snare for Start, rim for "
    "Select, or kick to hold the kick (RB3's song category menu)")
    .lifecycle(Lifecycle::kRequiresRestart);

// Band3/Controllers/MIDI keyboard

REXCVAR_DEFINE_BOOL(midi_keys, false, "Band3/Controllers/MIDI keyboard",
    "Play a MIDI keyboard as a Rock Band keytar, for Keys and Pro Keys")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(midi_keys_device, "", "Band3/Controllers/MIDI keyboard",
    "MIDI input port to play, or part of its name. Empty takes the first one no other band3 "
    "MIDI device has")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(midi_keys_base_note, 48, "Band3/Controllers/MIDI keyboard",
    "The MIDI note of the keytar's lowest C: 48 (C3) on most 25-key keyboards. The 25 keys "
    "from it play the keytar's")
    .range(0, 103);

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

// scrobbling (src/Net/scrobble.h)
REXCVAR_DEFINE_BOOL(lastfm_enabled, false, "Band3/Online",
    "Scrobble the songs played to Last.fm. The first time, band3 opens Last.fm in the browser "
    "to ask for your permission, then remembers it (lastfm_session)")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(lastfm_session, "", "Band3/Online",
    "The Last.fm session band3 was given when you allowed it to scrobble. Clear it to sign in "
    "again, as another Last.fm user. band3 never writes it to its log")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(listenbrainz_enabled, false, "Band3/Online",
    "Submit the songs played to ListenBrainz, with the user token from "
    "listenbrainz.org/settings")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(listenbrainz_token, "", "Band3/Online",
    "Your ListenBrainz user token (listenbrainz.org/settings). band3 never writes it to its log")
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
    "ones it hosts, or that are on MediaFire, Google Drive, Dropbox or dingorb, into a "
    "rhythmverse folder in the first of content_folders. Downloaded songs join the game "
    "without a restart, as songs bought from the Xbox store did, and band3 checks "
    "RhythmVerse for newer versions of them when it starts");

REXCVAR_DEFINE_BOOL(http_qr_code, true, "Band3/Online",
    "Show a QR code for the web page, and its address, at the top of the Music Library, so a "
    "phone can open it by scanning the screen");

// Home Assistant (src/Net/home_assistant.h): a host or a webhook URL turns it on
REXCVAR_DEFINE_STRING(ha_mqtt_host, "", "Band3/Online",
    "Home Assistant's MQTT broker (host name or address), which band3 tells the song, score, "
    "screen and more as Home Assistant entities. Empty leaves MQTT off")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(ha_mqtt_port, 1883, "Band3/Online",
    "TCP port of Home Assistant's MQTT broker (1883 unless it was changed)")
    .range(1, 65535)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(ha_mqtt_username, "", "Band3/Online",
    "User name for the MQTT broker. Empty connects without one")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(ha_mqtt_password, "", "Band3/Online",
    "Password for the MQTT broker, sent only with a user name. band3 never writes it to its log")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(ha_discovery_prefix, "homeassistant", "Band3/Online",
    "Home Assistant's MQTT discovery prefix (homeassistant unless it was changed)")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(ha_stagekit, false, "Band3/Online",
    "Also tell Home Assistant the Stage Kit's lights, strobe and fog. They change many times a "
    "second, so leave them out of Home Assistant's recorder")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(ha_webhook_url, "", "Band3/Online",
    "A Home Assistant webhook URL told when a song starts, ends and changes, as the RB3E "
    "Dashboard tells it, so its rb3e_lighting automation works unchanged. Empty leaves it off")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(rb3e_mode, true, "Band3/Online",
    "Tell the game's scripts RB3Enhanced is running (they see RB3E and RB3E_HAS_VERSION "
    "defined), so Rock Band 3 Deluxe turns on its RB3E features: its version line, party "
    "mode, song lookups, and clearing the song cache and restarting after an update")
    .lifecycle(Lifecycle::kRequiresRestart);

// Band3/Lights

REXCVAR_DEFINE_BOOL(stagekit_usb, true, "Band3/Lights",
    "Light the Stage Kits plugged into this PC as the game lights them: Santroller Stage Kits "
    "(which a PC sees in HID mode) and Xbox 360 Stage Kits (XInput, each taking one of "
    "Windows' four XInput slots). They don't take a player's place");

REXCVAR_DEFINE_BOOL(black_background_lights, false, "Band3/Lights",
    "With RB3Enhanced's Black Background modifier, play in a video venue drawn black with "
    "the band hidden instead of in no venue, so the venue's lights still reach the Stage "
    "Kits. The venue still loads and draws, which costs what a venue costs");

REXCVAR_DEFINE_BOOL(pico_discovery, true, "Band3/Lights",
    "Find the RB3E Dashboard's wireless Stage Kits (Pico W) on the network, to list and test "
    "them in Settings > Lights. A Pico answers whichever program found it last, so turn this off "
    "while the RB3E Dashboard runs. They follow the game through the RB3Enhanced events. Under "
    "the test harness nothing is broadcast, so the network's Picos are left alone");

// Band3/Advanced: the Settings screen's Advanced category, by subcategory

// Band3/Advanced/Memory

REXCVAR_DEFINE_INT32(main_heap_size, 0, "Band3/Advanced/Memory",
    "Main heap size in bytes, 0 = 134000000, the most a heap can be (mem.dta's is "
    "105000000, or 113000000 with Rock Band 3 Deluxe)")
    .range(0, band3::heap_report::kMaxHeapSize)
    .validator(ValidHeapSize)
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_INT32(char_heap_size, 0, "Band3/Advanced/Memory",
    "Character heap size in bytes, 0 = mem.dta's 32000000. At most 134000000")
    .range(0, band3::heap_report::kMaxHeapSize)
    .validator(ValidHeapSize)
    .lifecycle(Lifecycle::kRequiresRestart);

// Band3/Advanced/Scrobbling: where scrobbles go, for a test server

REXCVAR_DEFINE_STRING(lastfm_api_url, "https://ws.audioscrobbler.com/2.0/",
    "Band3/Advanced/Scrobbling",
    "Last.fm's API address, which scrobbling calls (lastfm_enabled)")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(listenbrainz_api_url, "https://api.listenbrainz.org",
    "Band3/Advanced/Scrobbling",
    "ListenBrainz's API address, which scrobbling calls (listenbrainz_enabled)")
    .lifecycle(Lifecycle::kRequiresRestart);

// Band3/Advanced/Game code

REXCVAR_DEFINE_BOOL(native_math, true, "Band3/Advanced/Game code",
    "Use native C++ replacements for the game's math functions (sin, cos, pow, "
    "vector/matrix ops)");

REXCVAR_DEFINE_BOOL(native_camera_shake, true, "Band3/Advanced/Game code",
    "Use the frame-rate independent camera shake replacement")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(vr_show_all, false, "Band3/Advanced/Game code",
    "Experimental, for VR: draw the whole world in every camera shot, ignoring what each "
    "shot hides and what lies outside the game's camera")
    .debug_only();

REXCVAR_DEFINE_BOOL(audio_xapo_nonblocking, true, "Band3/Advanced/Game code",
    "Answer the game's checks of whether each sound effect is still playing (dozens a frame, "
    "through its voice's effect parameters) with the newest result the audio thread set, "
    "instead of waiting for the audio thread's next pass to reach a sound that's starting or "
    "fading out: up to about 10 ms here, more than a frame at 120 Hz. Off waits, as the game "
    "does on a console (up to 5 ms there)");

REXCVAR_DEFINE_BOOL(audio_idle_bypass, true, "Band3/Advanced/Game code",
    "Skip the game's audio effects while all they do is process silence: the microphones' "
    "pitch correction, EQ, compressor and echo while no microphone is connected, and the "
    "reverb and the other effects while nothing reaches them. Each is skipped once what it "
    "plays has stayed below -144 dB for 5 seconds (a compressor's gain must have stopped "
    "moving too: the microphones' takes about 20 seconds), runs again on the first sound that "
    "reaches it, and none is skipped while a microphone is connected. On a Ryzen 6850U laptop "
    "it saves about 1.5-1.8 ms of the audio thread's CPU a frame in a song (of 2.6-2.8) and 2 "
    "ms paused (of 2.4). Off runs every effect all the time, as the game does on a console");

REXCVAR_DEFINE_BOOL(native_vorbis, true, "Band3/Advanced/Game code",
    "Run two parts of the game's Vorbis decoder as native code: the inverse MDCT, which turns "
    "each block of a song's audio into samples and is about a third of the decoding, and the "
    "codebook decoding, which reads the packed values each block is made from and is about a "
    "quarter. Both compute exactly what the game's own do, every sample the same. On a Ryzen "
    "6850U laptop the inverse MDCT saves about 0.35 ms of the decode thread's CPU a frame in "
    "a song (of 1.3) and the codebook decoding about 0.25 ms more (1.0 to 0.75). Off runs "
    "the game's own");

REXCVAR_DEFINE_BOOL(fast_heap_search, true, "Band3/Advanced/Game code",
    "Linux: find where the game's memory allocations go in the guest heap by skipping each "
    "allocated region whole, rather than one page at a time as the SDK does, then let the SDK "
    "allocate there: the same place, found faster. The game's physical allocations search 512 "
    "MB of 4 KB pages mostly full near the top; on a Steam Deck that was about 40 ms of the "
    "game thread in 28 s of a song, in bursts (0.4 ms of the frame a song starts in)");

REXCVAR_DEFINE_BOOL(fast_heap_search_check, false, "Band3/Advanced/Game code",
    "Linux, with fast_heap_search: run the SDK's own search for each allocation instead, and "
    "log any place it chose that fast_heap_search wouldn't have (none should). For checking "
    "fast_heap_search");

REXCVAR_DEFINE_BOOL(preload_song_start_resources, true, "Band3/Advanced/Game code",
    "Keep ui/resource/fonts/instruments_icons_rings.milo loaded for the whole session, as the "
    "game keeps its other fonts from boot. Otherwise the game reads it again as each song "
    "starts, in that song's slowest frame (2.3 ms of the game's thread on a Steam Deck), "
    "which could then miss its paint. Costs one small font's memory");

REXCVAR_DEFINE_BOOL(native_vorbis_check, false, "Band3/Advanced/Game code",
    "Run both the game's and native_vorbis's inverse MDCT and codebook decoding on every block "
    "of the songs' audio, play the game's, and log every 5 seconds how many samples and decoded "
    "values came out different and by how much (none should). For checking native_vorbis; it "
    "costs more CPU than either");

// Band3/Advanced/Graphics

REXCVAR_DEFINE_BOOL(fullbright, false, "Band3/Advanced/Graphics",
    "Force materials to not use an environ, making most things fullbright. "
    "Applies to materials loaded afterwards");

REXCVAR_DEFINE_BOOL(disable_even_odd_rendering, false, "Band3/Advanced/Graphics",
    "Process every render command each frame instead of alternating even/odd frames");

REXCVAR_DEFINE_BOOL(force_self_shadow, false, "Band3/Advanced/Graphics",
    "Draw the band's self-shadows in every camera shot, also the ones that turn them off. "
    "Characters far from the camera and the crowd still draw without them. Some of those "
    "shots may show shadow artifacts the game avoided there. Applies from the next shot");

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
    "before; off publishes each as soon as it's drawn, to compare. Only while the game runs "
    "under 1.5 times the display's refresh rate, frame_cap isn't auto and native_phase_lock "
    "isn't steering the cap on a display of 100 Hz or more: faster, each refresh shows the "
    "newest frame anyway, auto's variable refresh display shows each as it comes, and there "
    "the phase lock already has each frame reach the window before its paint, so the delay "
    "would only add lag. Below 100 Hz the lock keeps it, as frames vary more against the "
    "refresh");

REXCVAR_DEFINE_BOOL(native_present_trace, false, "Band3/Advanced/Native renderer",
    "With the native picture shown, log each frame the native renderer hands the window (its "
    "Present, the worker taking it, its submission, finish and publishing, in steady-clock ns, "
    "and its record and GPU wait ms) and each paint (its time, and the frame it shows), to tell why "
    "a frame was shown twice or skipped. Kept in memory and logged when the harness's "
    "present_stats reads the stretch (a reset drops them), so logging doesn't hold up the "
    "frames; about 120 lines a second");

REXCVAR_DEFINE_BOOL(native_phase_lock, true, "Band3/Advanced/Native renderer",
    "With the native picture shown and the frame cap at a fixed rate (display or a number "
    "matching the display's), move the cap's beat so each frame reaches the window a set "
    "time (native_phase_lock_slack) before the paint that shows it, rather than wherever the "
    "cap's clock happens to fall against the display's: a frame a little slower than the rest then "
    "still makes its paint instead of being shown a refresh late and the next skipped");

REXCVAR_DEFINE_INT32(native_phase_lock_slack, 0, "Band3/Advanced/Native renderer",
    "With native_phase_lock, how long before the paint that shows it the median frame should "
    "reach the window, in percent of a refresh: less is less lag, until frames a little slower "
    "than the rest miss their paint. 0, the default, picks by the display's rate: 65 at 100 Hz "
    "or more (at 120 Hz post-processed frames run about 1.9 ms behind the median, and 65 "
    "repeated 83% fewer frames than 50 for 1.2 ms more lag), 50 below, where 50 is already "
    "8.3 ms at 60 Hz. 10 to 90 sets it")
    .range(0, 90);

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

REXCVAR_DEFINE_BOOL(native_upload_pipeline, true, "Band3/Advanced/Native renderer",
    "With the native picture shown, on the upload path (Vulkan, where the window can't sample "
    "the native renderer's frames in place), record the next frame while the GPU draws and "
    "reads back the one before, so a frame costs the longer of its CPU and GPU time rather "
    "than both; off waits for each frame and its read-back right after sending it");

REXCVAR_DEFINE_BOOL(native_world_ahead, false, "Band3/Advanced/Native renderer",
    "Experimental: with RB3's even/odd rendering, draw each world frame's venue on the GPU "
    "right after that frame, without waiting for it, so the post frame after it only "
    "post-processes it and draws the track and HUD. The world's GPU time moves into the world "
    "frame's half of the pair: on a 60 Hz laptop post frames went from 14 to 8 ms and the "
    "picture's latency fell 3 to 6 ms, the picture the same. Where post frames drawn this way "
    "still run about a frame's time it skips more frames than without, so it measures both "
    "every few seconds and keeps the one that skips fewer. On draws it ahead on any GPU; off "
    "leaves it to native_world_ahead_auto, which does on an integrated GPU or a Steam Deck (a "
    "laptop's Radeon 680M at 60 Hz: 5 ms less latency, a sixth fewer repeated frames) and not "
    "on other GPUs, as on a fast desktop GPU at 120 Hz it made things worse. It sends GPU work "
    "it doesn't wait for, as the setup that once hung AMD GPUs did (not seen with this one). "
    "Not with native_present_pipeline on, or on Linux native_upload_pipeline (on by default), "
    "which wait differently");

REXCVAR_DEFINE_BOOL(native_world_ahead_auto, true, "Band3/Advanced/Native renderer",
    "Draw the world ahead (native_world_ahead) by itself on an integrated GPU (one that shares "
    "the system's memory, as a laptop's does) and on a Steam Deck: on a Radeon 680M at "
    "1920x1200, 60 Hz, a song's post frames went from 11.1 to 6.2 ms, latency from 22.5 to "
    "17.5 ms and repeated frames fell by a sixth, the picture the same. Not on other GPUs: a "
    "fast desktop one at 120 Hz did worse with it. native_world_ahead on draws it ahead on any "
    "GPU either way; with this off, only that does. On Linux neither does while "
    "native_upload_pipeline is on, its default. The log says which applied, with the GPU's "
    "name");

// read by src/Render/query_answers.cpp and the sync-only GPU
REXCVAR_DEFINE_INT32(native_query_sample_count, -1, "Band3/Advanced/Native renderer",
    "What the occlusion queries (the lens flares' visibility tests) report as drawn. -1, the "
    "default: the pixels each test covers on screen, as a 360 reports with nothing in front "
    "of the flare, so flares draw at their full strength, with either GPU (nothing hides "
    "them: what's in front isn't tested). 0 or more: every query reports that many with "
    "renderer native, and the emulated GPU its own query_occlusion_fake_sample_count (1000, "
    "what RB3 got before, which left a large flare at a few percent of its strength)")
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

REXCVAR_DEFINE_BOOL(native_gpu_timestamps, false, "Band3/Advanced/Native renderer",
    "Time each native renderer frame's parts on the GPU (the world, its texture passes, "
    "post-processing, the overlay...) with GPU timestamps, for the slow-frame log and the test "
    "harness's numbers. Direct3D 12 and Vulkan. On costs a few dozen timestamps a frame, 0.1 to "
    "0.2 ms of the frame's wait for the GPU, so it's off unless measuring");

REXCVAR_DEFINE_BOOL(native_gpu_draw_profile, false, "Band3/Advanced/Native renderer",
    "With native_gpu_timestamps, also time each world draw on the GPU (a timestamp before every "
    "one) and log every 10 seconds the meshes and materials that took the most, for finding what "
    "makes the world slow. Its timestamps cost GPU time of their own, so it's off unless "
    "profiling");

REXCVAR_DEFINE_BOOL(native_bc_textures, true, "Band3/Advanced/Native renderer",
    "Keep RB3's compressed textures (DXT1, DXT3, DXT5, DXN) compressed on the GPU, as BC1, BC2, "
    "BC3 and BC5, where it has those formats: the native renderer decodes a song's or a shot's "
    "new textures in a fraction of the time, and they take a quarter to an eighth of the video "
    "memory, upload and bandwidth. The picture differs only by the GPU's decoder rounding a "
    "colour by a step or two. Off decodes them to RGBA as before, to compare; applies to "
    "textures seen after the change");

REXCVAR_DEFINE_BOOL(native_r8_textures, true, "Band3/Advanced/Native renderer",
    "Keep RB3's one-channel textures (k_8: the planes of its movies, the intro, the menus' "
    "backgrounds and the music-video venues' screens, new each frame of a movie) a byte a "
    "texel on the GPU, as R8, where it has that format, rather than four: a quarter of the "
    "video memory, upload and decoding. The picture is the same. Off sends them as RGBA as "
    "before, to compare; applies to textures seen after the change");

REXCVAR_DEFINE_BOOL(native_keep_carried_passes, true, "Band3/Advanced/Native renderer",
    "Keep what the native renderer drew for a texture the game drew in an earlier frame and "
    "hasn't drawn since (the characters' outfits, skin and hair, carried into each frame's "
    "capture), rather than drawing it again every frame: it's drawn again when the game "
    "draws it again, or a setting that changes how it's drawn does. The picture is the "
    "same; at 1080p on a Steam Deck the texture passes took about 4 ms of the GPU a frame. "
    "Off draws them every frame, as before, to compare");

REXCVAR_DEFINE_INT32(native_deferred_decode_threads, 2, "Band3/Advanced/Native renderer",
    "How many threads help the native renderer decode the textures and meshes a song's first "
    "frames and a camera cut show for the first time (over 4 MB of them), so it holds the "
    "last frame for less time; they sleep the rest of the time. 0 decodes them on the "
    "renderer's own thread, as before. The picture is the same either way")
    .range(0, 3);

REXCVAR_DEFINE_BOOL(native_view_inline_mips, true, "Band3/Advanced/Native renderer",
    "Make the mips of the textures RB3 draws into (the crowd's, in a song, several times a "
    "frame) in the native renderer's own command buffers, exactly as SDL's mipmap "
    "generation makes them, rather than in one more command buffer each: half the command "
    "buffers a frame and a little less of the GPU waiting for the CPU. Direct3D 12 only. Off "
    "has SDL make them, as before, to compare");

REXCVAR_DEFINE_BOOL(native_view_premake_targets, true, "Band3/Advanced/Native renderer",
    "Make the textures RB3 draws into (outfits, faces, the crowd) as it loads them, a few "
    "between frames, rather than in the frame that first draws them: a song's first camera "
    "cut to the band made 25 of them in one frame (about 25 ms). Those without depth share "
    "one depth texture per size. The picture is the same; off makes them as before, to "
    "compare");

REXCVAR_DEFINE_BOOL(native_view_premake_arrays, true, "Band3/Advanced/Native renderer",
    "Make the GPU texture arrays the textures RB3 loads will go in as it loads them, a few "
    "between frames, rather than in the frame that first draws them: a song's first frame "
    "made 19 (about 12 ms). The picture is the same; off makes them as before, to compare");

REXCVAR_DEFINE_INT32(native_view_submit_points, 1, "Band3/Advanced/Native renderer",
    "Where the native renderer hands a frame to the GPU in parts, so the GPU draws the first "
    "while the worker records the rest: 0 = only after each texture pass with mips (the "
    "crowd's, in a song); 1 = also once the world is recorded, so the GPU draws it while "
    "the worker records post-processing and the HUD (in a song its post frames waited about "
    "0.3 ms less for the GPU; the picture is the same)")
    .range(0, 1);

REXCVAR_DEFINE_BOOL(native_view_mip_submits, true, "Band3/Advanced/Native renderer",
    "Hand the GPU a frame's work after each texture pass with mips (the crowd's, several "
    "times a frame in a song), so it draws them while the worker records the rest. Off "
    "keeps them in the frame's command buffer: post frames wait about 0.35 ms more for "
    "the GPU, but with AMD's Direct3D 12 driver a frame's 9 or 10 command buffers had it "
    "remap command memory now and then (6.5 ms a time in a VM), which off avoids. With "
    "native_view_inline_mips only; the picture is the same");

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

REXCVAR_DEFINE_BOOL(native_view_hardware_sampling, true, "Band3/Advanced/Native renderer",
    "Let the GPU's own texture samplers filter the textures that fill their texture array "
    "layer, in place of the shader filtering them as the game's samplers do: the same filters, "
    "mip levels, wrapping and anisotropy, rounded as the GPU rounds them, for much less GPU "
    "time. Textures the device's samplers can't do (padded into a larger layer, mirrored once, "
    "a border) keep the shader's. Off filters every texture in the shader, as before");

REXCVAR_DEFINE_BOOL(native_view_shader_variants, true, "Band3/Advanced/Native renderer",
    "Draw the common venue materials with smaller versions of the mesh shader, each with the "
    "features those materials don't use left out: the same picture for less GPU time. Off "
    "draws every material with the one shader that does them all");

REXCVAR_DEFINE_BOOL(native_view_instancing, true, "Band3/Advanced/Native renderer",
    "Draw runs of the same mesh that differ only in where each one stands (the crowd's "
    "cards) as one instanced draw: the same picture for much less CPU time. Off draws each "
    "by itself, to compare");

REXCVAR_DEFINE_BOOL(native_view_hoist_passes, true, "Band3/Advanced/Native renderer",
    "Draw the pictures RB3 draws into one texture again and again in a frame (the crowd's, "
    "one per kind of character) each into a layer of its own, ahead of the world, so the "
    "world draws in one go: the same picture for less CPU time. Off draws them where the "
    "game does, to compare");

REXCVAR_DEFINE_BOOL(vr, false, "Band3/Advanced/Native renderer",
    "Experimental: show the native renderer's picture in a VR headset through OpenXR "
    "(Windows), each eye drawn from where it is in the venue (vr_viewpoint), the track, the HUD "
    "and the menus on a panel in front of you (vr_hud). "
    "Needs the native renderer and an OpenXR runtime, such as SteamVR, which starting it "
    "opens; the window shows the left eye")
    .lifecycle(Lifecycle::kRequiresRestart)
    .debug_only();

REXCVAR_DEFINE_STRING(vr_viewpoint, "game_camera", "Band3/Advanced/Native renderer",
    "With vr: where you are in the venue during songs. game_camera: where the game's camera "
    "is, seeing the venue in 3D through a window that the game's picture fills "
    "(vr_window_width), dark around it, fading to black at each cut. front_row, on_stage, "
    "venue: not made yet; they use "
    "vr_anchor's place for now. Menus are always seen through the game camera's window. Takes "
    "effect at once")
    .allowed({"game_camera", "front_row", "on_stage", "venue"})
    .debug_only();

REXCVAR_DEFINE_DOUBLE(vr_window_width, 100.0, "Band3/Advanced/Native renderer",
    "With vr: how wide the game camera's window looks, in degrees across from where you were "
    "at the last recenter, its height following at 16:9. It stays this size at every cut: "
    "the game's picture fills it, so a narrow shot is magnified and its depth flattened, as "
    "a long lens does. Takes effect at once")
    .range(20.0, 150.0)
    .debug_only();

REXCVAR_DEFINE_DOUBLE(vr_window_dim, 1.0, "Band3/Advanced/Native renderer",
    "With vr: how dark it is around the game camera's window, from 0 (the venue around it "
    "shows as it is) to 1 (black). Takes effect at once")
    .range(0.0, 1.0)
    .debug_only();

REXCVAR_DEFINE_INT32(vr_cut_fade_ms, 120, "Band3/Advanced/Native renderer",
    "With vr: at each camera cut, and as the window comes and goes, the headset's picture goes "
    "black and fades back in over this many milliseconds (the panel doesn't fade); 0 cuts "
    "straight. Takes effect at once")
    .range(0, 2000)
    .debug_only();

REXCVAR_DEFINE_BOOL(vr_hud_in_window, false, "Band3/Advanced/Native renderer",
    "With vr and vr_hud: during songs, show the HUD (and the track, with vr_track_3d off) in "
    "the game camera's window where the game's picture has them, as menus are, instead of on "
    "the panel's own place "
    "(vr_hud_below, vr_hud_width). Takes effect at once")
    .debug_only();

REXCVAR_DEFINE_STRING(vr_anchor, "0,-1250,520,0", "Band3/Advanced/Native renderer",
    "With vr and a vr_viewpoint other than game_camera, during songs: where the headset's "
    "starting point is in the venue, as x,y,z in inches (x right, "
    "y towards the stage, z up) and a turn in degrees, counterclockwise seen from above (0 "
    "faces the stage); the default is above and behind the crowd in front of the stage. "
    "Empty follows the game's camera, facing where it looks")
    .debug_only();

REXCVAR_DEFINE_DOUBLE(vr_world_scale, 1.0, "Band3/Advanced/Native renderer",
    "With vr: how much smaller the venue looks than life size, so a head's movement goes "
    "this many times as far in it. 1 = life size")
    .range(0.1, 100.0)
    .debug_only();

REXCVAR_DEFINE_STRING(vr_game_rate, "auto", "Band3/Advanced/Native renderer",
    "With vr, while the headset's session runs: the rate the game runs at, in place of "
    "frame_cap. auto: the headset's refresh rate (120 on a 120 Hz headset), so the game's camera "
    "and everything else moves with each refresh; a number of Hz (24 to 240); off: frame_cap's "
    "as without VR. Takes effect at once")
    .validator([](std::string_view v) { return band3::pacing::ValidVrGameRate(v); })
    .debug_only();

REXCVAR_DEFINE_BOOL(vr_gpu_queue_wait, true, "Band3/Advanced/Native renderer",
    "With vr: hand each frame to the headset as soon as its drawing is submitted, the copy into "
    "the headset's images waiting for it on the GPU. Off, the CPU waits for the GPU to finish "
    "the eyes and the panel first, as before, to compare. Takes effect at once")
    .debug_only();

REXCVAR_DEFINE_BOOL(vr_fake_pose, false, "Band3/Advanced/Native renderer",
    "With vr: turn the head slowly from side to side instead of where the headset says it "
    "is, to test without one (SteamVR's null driver doesn't move)")
    .debug_only();

REXCVAR_DEFINE_BOOL(vr_hud, true, "Band3/Advanced/Native renderer",
    "With vr: show the HUD and the menus (and the track, with vr_track_3d off) on a flat panel "
    "in front of you, as the game draws them. Off shows the venue alone. Takes effect at once")
    .debug_only();

REXCVAR_DEFINE_DOUBLE(vr_hud_distance, 1.8, "Band3/Advanced/Native renderer",
    "With vr_hud: how far ahead of you the panel and the game camera's window hang, in "
    "metres, from where you faced at the last recenter. The window's size follows from "
    "vr_window_width, so further away it's bigger. Takes effect at once")
    .range(0.3, 20.0)
    .debug_only();

REXCVAR_DEFINE_DOUBLE(vr_hud_below, 0.25, "Band3/Advanced/Native renderer",
    "With vr_hud: how far below your eyes the panel's centre is, in metres (negative raises "
    "it), where it isn't in the game camera's window (songs, and screens with nothing of the "
    "venue); the panel tilts back to face you. Takes effect at once")
    .range(-5.0, 5.0)
    .debug_only();

REXCVAR_DEFINE_DOUBLE(vr_hud_width, 55.0, "Band3/Advanced/Native renderer",
    "With vr_hud: how wide the panel looks where it isn't in the game camera's window, in "
    "degrees across from where you are; its height follows at 16:9. Takes effect at once")
    .range(10.0, 160.0)
    .debug_only();

REXCVAR_DEFINE_BOOL(vr_hud_recenter, false, "Band3/Advanced/Native renderer",
    "With vr_hud: a button: each change of this setting (either way) moves the panel back in "
    "front of where you face now. The panel is also placed where you first faced, and again "
    "when the headset's own recenter moves its seated position")
    .debug_only();

REXCVAR_DEFINE_BOOL(vr_track_3d, true, "Band3/Advanced/Native renderer",
    "With vr, during songs: show the track (the highway with its gems, smashers, Overdrive "
    "meter and multiplier) as a 3D object in front of you, each eye seeing it from where it "
    "is, instead of flat on the HUD panel. Each player's track keeps its place beside the "
    "others' as on the screen. Takes effect at once")
    .debug_only();

REXCVAR_DEFINE_DOUBLE(vr_track_forward, 1.2, "Band3/Advanced/Native renderer",
    "With vr_track_3d: how far ahead of you the track's strikeline (where the gems are hit) "
    "is, in metres along the floor, from where you faced at the last recenter. Takes effect "
    "at once")
    .range(0.2, 10.0)
    .debug_only();

REXCVAR_DEFINE_DOUBLE(vr_track_down, 0.3, "Band3/Advanced/Native renderer",
    "With vr_track_3d: how far below your eyes the track's strikeline is, in metres (negative "
    "raises it). 0.3 at vr_track_forward 1.2 looks down onto it at the game's own angle, "
    "14 degrees. Takes effect at once")
    .range(-2.0, 3.0)
    .debug_only();

REXCVAR_DEFINE_DOUBLE(vr_track_pitch, 0.0, "Band3/Advanced/Native renderer",
    "With vr_track_3d: how far the track's far end is tilted up toward you, in degrees, from "
    "lying level as the game lays it out (as a music stand tilts). Takes effect at once")
    .range(-30.0, 80.0)
    .debug_only();

REXCVAR_DEFINE_DOUBLE(vr_track_scale, 30.0, "Band3/Advanced/Native renderer",
    "With vr_track_3d: how big the track is, as the game's units a metre: 30 makes it 0.65 m "
    "across, a smaller number bigger. Takes effect at once")
    .range(5.0, 200.0)
    .debug_only();

REXCVAR_DEFINE_DOUBLE(native_view_render_scale, 1.0, "Band3/Advanced/Native renderer",
    "For measuring: the native renderer draws its picture at this times the window's size "
    "(after native_max_height), its screen-sized passes with it, and the window shows it "
    "scaled to fit, to see what the GPU does at a size the window can't reach (VR's eyes). "
    "1 = the window's size")
    .range(0.25, 4.0)
    .debug_only();

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

REXCVAR_DEFINE_STRING(test_score_history, "", "Band3/Advanced/Test harness",
    "With test_port set: keep score history in this file, autoplay runs included, in place "
    "of score_history.jsonl, which a harness session never writes to (empty = keep none)")
    .lifecycle(Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(autoplay, false, "Band3/Advanced/Test harness",
    "The game plays every part itself, from the next song start: for repeatable profiling "
    "runs and for checking a song without playing it");

REXCVAR_DEFINE_BOOL(stagekit_fake, false, "Band3/Advanced/Test harness",
    "With stagekit_usb on: add a pretend USB Stage Kit that keeps the commands it's sent, for "
    "the harness's lights command");

REXCVAR_DEFINE_BOOL(music_video_test_pattern, false, "Band3/Advanced/Test harness",
    "Draw a test pattern in place of the video venues' movie frames (their "
    "backgrounds): a white border, a grey ramp, a moving bar and four colour quadrants");

REXCVAR_DEFINE_BOOL(midi_keys_test_device, false, "Band3/Advanced/Test harness",
    "With test_port set and midi_keys on: report a MIDI keyboard named harness, with no port "
    "open, from the first message the harness's midi command hands it")
    .lifecycle(Lifecycle::kRequiresRestart);

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

REXCVAR_DEFINE_STRING(settings_reference, "", "Band3/Advanced/Startup",
    "Write the settings reference (docs/settings-reference.md: every setting, its default, "
    "values and description, from this build) to this file and quit without starting the "
    "game. tools/settings_reference.py runs it")
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
        .guitar_type = REXCVAR_GET(guitar_type),
        .rnd_sync = REXCVAR_GET(rnd_sync),
        .disable_metamusic = REXCVAR_GET(disable_metamusic),
        .main_heap_size = REXCVAR_GET(main_heap_size),
        .char_heap_size = REXCVAR_GET(char_heap_size),
        .events_target = REXCVAR_GET(events_target),
        .events_port = REXCVAR_GET(events_port),
        .discord_enabled = REXCVAR_GET(discord_enabled),
        .lastfm_enabled = REXCVAR_GET(lastfm_enabled),
        .lastfm_session = REXCVAR_GET(lastfm_session),
        .listenbrainz_enabled = REXCVAR_GET(listenbrainz_enabled),
        .listenbrainz_token = REXCVAR_GET(listenbrainz_token),
        .lastfm_api_url = REXCVAR_GET(lastfm_api_url),
        .listenbrainz_api_url = REXCVAR_GET(listenbrainz_api_url),
        .http_enabled = REXCVAR_GET(http_enabled),
        .http_port = REXCVAR_GET(http_port),
        .http_address = REXCVAR_GET(http_address),
        .ha_mqtt_host = REXCVAR_GET(ha_mqtt_host),
        .ha_mqtt_port = REXCVAR_GET(ha_mqtt_port),
        .ha_mqtt_username = REXCVAR_GET(ha_mqtt_username),
        .ha_mqtt_password = REXCVAR_GET(ha_mqtt_password),
        .ha_discovery_prefix = REXCVAR_GET(ha_discovery_prefix),
        .ha_stagekit = REXCVAR_GET(ha_stagekit),
        .ha_webhook_url = REXCVAR_GET(ha_webhook_url),
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
        "controller_type", "guitar_type", "rnd_sync", "disable_metamusic", "main_heap_size", "char_heap_size",
        "events_target", "events_port", "discord_enabled", "lastfm_enabled", "lastfm_session",
        "listenbrainz_enabled", "listenbrainz_token", "lastfm_api_url", "listenbrainz_api_url",
        "http_enabled", "http_port",
        "http_address", "ha_mqtt_host", "ha_mqtt_port", "ha_mqtt_username", "ha_mqtt_password",
        "ha_discovery_prefix", "ha_stagekit", "ha_webhook_url", "rb3e_mode", "gocentral", "gocentral_address", "liveless",
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
