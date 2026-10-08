# Settings, folders and songs

## The launcher

The launcher is band3's setup screen. It opens in the game window before the game starts,
with tabs for the game and its folders, graphics, audio, controllers and online features,
and works with the mouse, the keyboard or a controller. Point at a setting (or move to it)
to see what it does. Changes
apply as you make them; **Save** keeps them for next time, **Play** saves and starts the
game, and **Close** quits, asking first if something isn't saved.

Devices are picked from lists of what's connected: each mic slot on the Audio tab lists the
PC's microphones, with a level meter beside it while the tab is open; the Controllers tab
lists the MIDI ports; the Graphics tab names the monitors and offers the chosen monitor's
display modes as resolutions. A saved device that isn't plugged in shows as "(not
connected)" and stays chosen until you pick another, and **Other...** takes a typed name
or part of one.

The Graphics tab's **Renderer** picks what draws the game's picture: **Native**, band3's own
[native renderer](native-renderer.md) alone (the default on Windows, and offered there
only), **Emulated**, the emulated Xbox 360 GPU alone (the default on Linux), or **Native +
emulated (debug)**, both, F8 switching between their pictures in game. A change to or from
Native applies when band3 starts, so Play restarts band3 for it. The tab shows only the
sections the chosen renderer uses ([which settings apply](native-renderer.md#which-settings-apply)).
The native renderer's **Resolution limit** ([`native_max_height`](native-renderer.md)) caps
the lines it draws, scaling a taller window's picture up, for a GPU that can't keep up at
the window's size. **Fill the window** ([`native_fill_window`](native-renderer.md#filling-the-window),
on by default) has the game draw at the window's shape with the native renderer: an
ultrawide window shows more to the sides and a 16:10 one more above and below, the HUD and
highways at their size in the middle, with no black bars. **Aspect** (letterbox or stretch)
is for the game's 16:9 picture: the emulated GPU's, or the native renderer's with Fill the
window off. **VSync** shows only with the frame rate cap off, since the cap turns it off.
The tab starts with the settings that trade lag ([Playing with the least lag](#playing-with-the-least-lag)).

The Controllers tab starts with a list of every controller and instrument band3 sees (Xbox
pads and instruments, PS3/Wii/PS4/PS5 instruments on their dongles, a MIDI kit, the
keyboard) and the player each one is, or "Not playing", and shows the selected one live
below. Pick one (click its row, or press A on it) to test it: until you press Back on it, or
click Stop, it only drives the test view, not the launcher. A guitar shows its frets, solo
frets, strum, whammy, tilt and pickup switch; a drum kit flashes each pad, cymbal and kick
pedal as brightly as it was hit (an RB1 kit has no velocity, so every hit is full
strength); a controller shows its buttons, sticks and triggers, and what "Gamepads play as"
makes it in the game (with the SDL backend, an Xbox 360 guitar or kit shows as a controller
but plays as its instrument). A MIDI kit also lists its last notes and what they play. It reads the
devices through band3's own input drivers, so what it shows is what the game will read.

Any controller can drive the launcher: the d-pad or left stick moves, A picks, B backs out,
LB and RB switch tabs, and Start plays. A guitar's frets and a kit's pads act as the face
buttons (green is A, red is B), and strumming moves up and down, except while it's being
tested. Leaving the Controllers tab, unplugging the device or pressing Play ends a test too.

The instrument settings on the Controllers tab apply as you change them, except **Input
backend** (SDL or XInput): changing it applies when you press Play, which saves and restarts
band3 on the new backend (the row says so). Under the [test harness](test-harness.md)
band3 doesn't restart itself, so the game keeps the old backend until the next start. A
changed MIDI port reopens the kit a moment after you stop changing it.

The Online tab turns on the web song browser, Discord, RB3Enhanced's events,
[Home Assistant](integrations.md#home-assistant), and online play:
[GoCentral and Liveless](integrations.md#gocentral-rock-central) (Windows only).
Their settings are read as the game starts, so Play applies them without a restart.

It opens:

- the first time band3 starts, including the first start after updating to a version that
  has it;
- whenever the game data folder doesn't look like Rock Band 3's (the folder, `default.xex`
  or `gen/main_xbox.hdr` is missing); the Game tab says what's wrong;
- when Shift is held as band3 starts (Windows only);
- when band3 is started with `--launcher`;
- otherwise only when **Show this screen at startup** was ticked the last time you pressed
  Save or Play. It starts unticked until you tick it, so after the first Play band3 goes
  straight into the game; once ticked, it stays ticked until you untick it.

It never opens when RB3E's `rb3e_relaunch_game` restarts the game, nor for a
[test harness](test-harness.md) run (`test_port`) unless that is started with `--launcher`.

To get back to it, hold Shift while band3 starts, start it with `--launcher`, or tick
**Show the launcher at startup** in the [in-game settings](#pause-menu-and-in-game-settings) and save.
On a Steam Deck in Game Mode, add
`--launcher` to band3's launch options in Steam (Properties → Launch Options), and take it
out again afterwards. `launcher = true` in `band3.toml` is ignored: it's for one start, from
the command line.

The launcher saves to `band3.toml` beside the executable, writing only the settings that
differ from their default (what band3 would use without the file: the Steam Deck setting on
a Deck, then `band3_config.ini`'s, then band3's own) and keeping the file's other lines.
**Show this screen at startup** is always written. A changed setting's **Reset** button
puts it back to its default. Folders inside the ini's folder (or the executable's, without
an ini) are saved relative to it, so the install still moves as one folder. The launcher
doesn't keep comments: saving over a `band3.toml` that has comments, or that it couldn't
read, keeps the old file as `band3.toml.bak`. If `band3.toml` can't be written, Play offers
to play anyway with the settings as they are, for that session only.

A setting given on the command line or by a `REX_*` environment variable is shown but
locked ("Set on the command line", "Set by an environment variable"), since a saved value
wouldn't win over it. On a Steam Deck a banner at the top says so, with the
`steam_deck_defaults` switch: on, the [Deck's settings](#steam-deck) are the defaults and
what you change wins over them.

Some settings are only in the [in-game settings](#pause-menu-and-in-game-settings): the Advanced tab's
(debug options, heap sizes, the native renderer's debug options, the test harness's and
the virtual instrument) and each tab's **More settings**.

## Playing with the least lag

Rock Band 3 judges each hit on the game's frames: band3 reads the instrument every 4 ms, and
the game counts a press at its next frame. So the game's frame rate, not the display's,
sets how finely hits are timed: to within 16.7 ms at 60 fps, 8.3 ms at 120 and 4.2 ms at
240. The Graphics tab's **Latency** section has the settings that trade lag:

- **Frame rate cap** ([`frame_cap`](#config-files)) paces the game, and can run it faster
  than the display refreshes: the window then shows the newest frame at each refresh, so
  a higher cap also means a fresher picture. Up to 240, the most the SDK's video mode takes.
- **Smooth frame pacing** (`native_present_pacing`, native renderer, on by default) hands
  each frame to the window a steady delay after the game made it, so motion steps evenly.
  Off hands it over as soon as it's drawn: about 3 to 4 ms sooner at 120 fps, with now and
  then an uneven step. It holds frames only with the cap under 1.5 times the display's
  refresh rate and not `auto`: faster, each refresh shows the newest frame anyway, and a
  variable refresh display shows each frame as it comes, so holding one only adds lag.
- **Lowest latency** sets the cap at the most whole refreshes' worth of frames up to 240
  (240 on a 60 or 120 Hz display, 144 on a 144 Hz one), so the picture still steps evenly,
  and where that cap is the display's own rate (over 160 Hz) smooth frame pacing off too. If
  the game can't hold that rate, lower the cap.

The debug overlay (`debug_overlay`) shows the game's frame rate (**Game**), the window's
(**Window**, which the display's refresh rate holds back: a 60 Hz TV shows 60 whatever the
cap), and with the native renderer **Frame to window**, how long a frame waits before a
paint shows it. On a 60 Hz display in testing, that wait was 12.6 ms at a 120 cap with
smooth frame pacing, 9.0 ms without, and 7 to 8 ms at 240.

After changing any of these, run the game's calibration again (in its Options), so your
hits are judged against the lag you have now.

## Pause menu and in-game settings

Press **Escape** in game, or hold both stick clicks on a controller for a second, to open
band3's pause menu: **Resume**, **Settings**, **Instrument Lab** and **Quit game**. During
a song it pauses the song too, the way your Start button would, so the song's own pause
menu is underneath; **Resume** (or Escape, B or Start) closes the menu and resumes the
song. A song you'd already paused yourself stays paused. On the song's first couple of
seconds, when the game doesn't take a pause yet, the menu opens over the song as it plays.
In the game's menus nothing is paused, but the game reads no input while the menu is open.
**Instrument Lab** closes the pause menu and opens the
[Instrument Lab](instruments.md#instrument-lab), leaving a song paused for its own pause
menu to resume. **Quit game** asks, then closes band3. Escape does nothing while the
launcher shows, as F6 and F9 don't.

Escape isn't one of the keys the keyboard plays the controller with (B, back, is
Backspace), so nothing else moves for it. The pause menu's key is `bind_pause_menu`, with
the other key binds in **All settings...**.

**Settings** opens band3's settings over the game. They have the launcher's tabs and rows, from the same
list, showing what the chosen renderer uses as the launcher does, and an **Advanced** tab.
Changes apply as you make them: at once, or from the next time the game loads what they
affect (a forced venue from the next venue load). A row whose change applies only from
the next start says **Applies at the next start** once you change it: the folders, most of
the Online tab, the instrument drivers, the input backend, the microphones, the emulated
GPU's anti-aliasing, and the renderer from Native to the other two or back. Pointing at a
setting shows what it does, and says if it's one of those. **Save** writes `band3.toml` as
the launcher does: only what you changed, the file's other lines kept. The footer says
when something isn't saved, and **Back** asks first; going back without saving keeps the
changes until band3 closes. **Show the launcher at startup** is the launcher's
[box](#the-launcher), saved with the rest.

While they're open the game gets no input: the keyboard and the controllers drive the
settings (arrows or the d-pad move, Enter or A picks, LB and RB switch tabs, and B, Start
or Escape go back to the pause menu), and the game reads nothing pressed. They're drawn at
the launcher's scale: at 720p and on a Steam Deck's 1280x800 screen they fill most of it,
and they grow with the window.

Two things of the launcher's aren't here, since the game is using the devices: the
Controllers tab's device list and tester (the [Instrument Lab](instruments.md#instrument-lab),
F6, shows what the game reads from each player), and the level meters beside the mic
slots, which would record from the microphones the game captures from. The mic slots'
dropdowns are.

**Advanced** lists band3's technical settings by group, by the names the command line uses:
Game code, Graphics, Logging, Memory (the heap sizes), Native renderer, Startup (read-only:
`launcher` and `relaunch_wait_pid` are only set as band3 starts) and Test harness. Each tab
also ends with **More settings**: band3's settings the launcher doesn't show, such as
`liveless_rooms` and `http_address` on the Online tab and the FPS counter
(`debug_overlay`) on Graphics. Both are made from band3's settings as they're registered,
so a setting band3 adds shows up there by itself.

**All settings...** opens the SDK's own settings menu, for the SDK's settings and the key
binds: every setting by category, each by its name, with what it does when pointed at.
band3's are under **Band3**, in the in-game settings' groups: Game, Graphics (with Native
and Emulated), Audio, Controllers (with MIDI drums), Online, and Advanced (with Game code,
Graphics, Logging, Memory, Native renderer, Retired, Startup and Test harness). Its **Save
to config** writes `band3.toml` differently from band3's Save ([below](#config-files)); a
note under it says so. Escape, or its note's buttons, close it and go back to the pause
menu.

Without a keyboard, hold both stick clicks on a controller for a second to open the
pause menu, or both stick clicks and the left bumper for the
[Instrument Lab](instruments.md#instrument-lab). The same chord closes them.
`menu_shortcut` (the Controllers tab's More settings) turns this off. It works with either input backend: it
watches the buttons the game reads from each player's controller, so it needs the
controller connected as a player, and works once the game is running (not on the
launcher). Instruments don't count, since a guitar's solo frets and a drum kit's pads and
second kick send stick clicks too.

## Config files

`band3.toml` next to the executable holds band3's settings; the [launcher](#the-launcher)
and the [in-game settings](#pause-menu-and-in-game-settings) write it. Any setting can also be passed
on the command line, e.g.
`--forced_venue=arena_04`, or by a `REX_*` environment variable. The
[settings reference](settings-reference.md) lists every one, with its default and what it
does; band3 writes it from its settings registry (`python tools/settings_reference.py`), so
it matches the build.

The SDK's settings menu (the in-game settings' **All settings...**) has a **Save to config** that writes
`band3.toml` too, but differently: it rewrites the whole file with every setting that
differs from the SDK's default, whatever set it, so values from `band3_config.ini`, the
Steam Deck settings and the command line are frozen into the file from then on, and
anything else in the file is dropped. Prefer band3's Save, the launcher's or the in-game settings', which
writes only what you change.

`band3_config.ini` is the settings file from before `band3.toml`. band3 still reads one
if it finds it (in its working directory, then beside the executable), but releases no
longer include it, and it has only some of the settings: the
[reference](settings-reference.md#band3_configini) lists its keys. In this repository it
also marks the folder that relative paths start from (see [Folders](#folders)). Where the
same setting is set in more than one place, the first of these wins:

1. the command line
2. `REX_*` environment variables
3. `band3.toml`
4. the [Steam Deck](#steam-deck) settings, on a Deck
5. `band3_config.ini`
6. the defaults

The [settings reference](settings-reference.md) has every setting, its default and what it
does, by the names the command line and `band3.toml` use. Ones worth knowing about:
`frame_cap` and `background_fps`, which pace the game and its venues
([Graphics](settings-reference.md#graphics)), with `video_mode_refresh_rate`
([the SDK's](settings-reference.md#more-of-the-sdks-settings)); `forced_venue`, `song_speed` and
`track_speed` ([Game](settings-reference.md#game)); `controller_type` and `input_backend`
([Controllers](settings-reference.md#controllers)). RB3Enhanced's `unlock_clothing` and
`gold_on_all_difficulties` are in [Integrations](integrations.md#rb3enhanced-compatibility),
and `game_origin_icons` in [Song source icons](integrations.md#song-source-icons).

## Folders

Files the game writes to its own folder (`game:\`) go to `game` in the user data root
(`Documents\band3\game` on Windows, or under `--user_data_root`), and the game reads them
back from there; `assets` is never written. Rock Band 3 Deluxe keeps its settings,
modifiers and playlists there (`dx_settings.dta`, `dx_playlist.dta` and so on), so
deleting that folder resets them. Test runs have their own user data, so they don't
change yours.

`game_data_root`, `user_data_root` (saves, profiles and the `game` folder;
`Documents\band3` by default) and `cache_root` (the shader cache; `cache` in the user data
folder by default) move band3's folders, from the launcher's Game tab, `band3.toml` or
`band3_config.ini`. A relative path is relative to the ini's folder, or to the executable's
when there is no ini, so `user_data_root = user_data` keeps everything band3 writes beside
it, for a portable install. band3 looks for the ini in its working directory, then beside
the executable.

## DLC and custom songs

band3 reads DLC and custom songs (Xbox 360 `CON`, `LIVE` and `PIRS` packages, Rock Band
and Rock Band 2 DLC included, as RB3 reads them) straight from the folders
`content_folders` names (the Game tab, or `[game]` in `band3_config.ini`);
nothing is installed or unpacked, and band3 never writes there. Separate folders with `|`;
subfolders count, and a relative folder is relative to the ini's folder, as above (`songs`
beside it by default). Changes apply at the next launch, except songs the web page's
RhythmVerse tab downloads or finds added ([RhythmVerse](integrations.md#rhythmverse)), which
the game takes in as it runs; a song taken out stays listed until the next launch.

A network folder works, but a local one is safer for audio: a song streams from its
package while it plays. A local folder is also faster to read: a large library on a slow
network folder can delay the song list on the first boot, and one that takes over a minute
isn't listed that session.

A custom song whose `song_id` is text instead of a number gets the number RB3Enhanced
gives it (the text's CRC-32 mod 9999999, plus 2130000000), so IDs agree with
RB3Enhanced's.

## Music videos

With `music_videos` on (the default), a song's own music video plays in the video venues in
place of their background clips: behind the band and under the venue's colour filters, in
time with the song, standing still while it's paused and following it when it restarts or
practice moves to another section. Put the video in the `videos` folder beside the ini (or
the folder `music_videos_folder` names), named after the song's shortname:
`20thcenturyboy.mp4` for 20th Century Boy. A song's shortname is in the log as it starts
(`No music video for 20thcenturyboy in music_videos_folder`). `forced_venue = video` (the
Game tab's Forced venue, Video venue) puts every song in a video venue. Turning `music_videos` off
stops the video at once; turning it on starts videos from the next song.

The video starts with the song. `<shortname>.ini` beside it with Clone Hero's
`video_start_time = <milliseconds>` (as in a chart's `song.ini`) starts it that far in at the
song's start; a negative one starts it that far into the song. The screen is black before the
video starts, and its last frame stays after it ends. `music_video_fit` keeps a video whose
shape isn't 16:9 whole with black bars (`fit`), fills the screen and cuts its edges off
(`fill`), or stretches it. Its sound isn't played.

Videos are decoded by Windows (Media Foundation), on Windows only for now: `.mp4`, `.m4v`,
`.mov`, `.mkv`, `.webm`, `.avi` or `.wmv`, in a codec Windows has. H.264 works everywhere
but Windows N editions (which need the Media Feature Pack); HEVC, VP9 and AV1 need their
Microsoft Store extensions. A video Windows can't decode is logged (`Music video ...`) and
the venue keeps its own clips. Decoding takes a share of a CPU core of its own, about a
fifth of one for a 1080p video, besides the decoder's.

## Loose files (mods)

As with RB3Enhanced, a file on disk replaces the same file in the ARK, so mods such as
custom characters can be installed without rebuilding the ARK. Put the file in the game
data folder (`assets` by default, or `game_data_root`) at the path it has inside the ARK,
with its Xbox name (`gen` folders and `_xbox` extensions as in the ARK). A `..` in an ARK
path becomes a folder named `(..)`, as RB3Enhanced lays them out, so files packaged for
RB3Enhanced go in as they are. Files the game writes to its own folder (the `game` folder
in the user data root, see [Folders](#folders)) replace ARK files the same way. band3 lists
these folders when it starts, so a file added while it runs is used from the next launch;
a changed file that was already there is read again the next time the game loads it. A
file whose path (with its `(..)` folders) is over 250 characters is left out, with a
warning in the log, since the game can't open a path that long from disk.

Each file read from disk instead of the ARK is logged as `NewFile: <path>`. If a mod
doesn't show up, set `log_level = debug`: the log then also lists every file read from the
ARK, marked `(ARK)`, with the path its replacement needs.

## Steam Deck

On a Steam Deck, band3 starts fullscreen and letterboxed (the game is 16:9, the screen
16:10), at the console's 60 Hz (`frame_cap` 60) with vsync on and the FPS counter off, since Steam's performance overlay does
that job. These only fill in settings that `band3.toml` and the command line leave unset,
but they win over `band3_config.ini`, whose window settings are for a desktop. Turn
`steam_deck_defaults` off (the Steam Deck banner on the launcher or in the in-game settings, then restart) to go
back to the ini's.

band3 recognises a Deck from the `SteamDeck=1` Steam sets, or on Linux from the Deck's
hardware ids. Keep `resolution_scale` at 1: the Deck's screen can't show more than the
console's 720p, and scaling costs a lot of GPU time. The log warns when it's higher.

Instruments on a Deck:

- The Deck has one USB-C port, so more than one instrument or dongle needs a hub.
- For PlayStation and Wii dongles, install the
  [udev rules](instruments.md#playstation-and-wii-instruments-experimental) from Desktop
  Mode. SteamOS has no password for `sudo` until you set one with `passwd`.
- The log says what type each controller reports and what it plays as (for example
  "A controller reports type 1 (gamepad); playing as 7 (guitar)"). If an instrument
  shows up as a gamepad, turn off Steam Input for band3 (its controller settings in
  Steam) and try again, and include the log line if you report it.
