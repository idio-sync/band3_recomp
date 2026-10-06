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
only), **Emulated**, the emulated Xbox 360 GPU alone (the default on Linux, where the native
renderer hasn't been run yet), or **Native + emulated (debug)**, both side by side, F8
switching between their pictures in game. A line under it says what the chosen one does.
Native to or from the other two applies when band3 starts, so Play restarts band3 for it.
The tab's sections show only what the chosen renderer uses: **Display** and **Game** for
all three, **Native renderer** (its anti-aliasing, anisotropic filtering and **Resolution
limit**, [`native_max_height`](native-renderer.md): none, the window's size, the default, or
the most lines the native renderer draws, a taller window's picture scaled up to fill it,
for a GPU that can't keep up at the window's size; **Other...** takes a typed height) for
Native and Native + emulated, **Emulated GPU** (render scale, FXAA,
anisotropic filtering, VSync, and under Native + emulated what the emulated GPU still does
while the native picture shows) for Emulated and Native + emulated, as does **Compress
character textures**. The emulated GPU's own settings exist only while it runs, so
choosing it from Native shows a note in their place until Play has restarted band3
([which settings apply](native-renderer.md#which-settings-apply)). The tab's **Frame rate
cap** is [`frame_cap`](#config-files): the display's refresh rate (the default), Auto for a
VRR display, Off, a rate from the list, or **Other...** for a typed one. **VSync** shows
only with the cap off, since the cap turns it off.

The Controllers tab starts with a list of every controller and instrument band3 sees (Xbox
pads and instruments, PS3/Wii/PS4/PS5 instruments on their dongles, a MIDI kit, the
keyboard) and the player each one is, or "Not playing", and shows the selected one live
below. Pick one (click its row, or press A on it) to test it: until you press Back on it, or
click Stop, it plays the test view only and doesn't move around the launcher, so holding a
fret or hitting a pad presses nothing. A guitar shows its frets, solo frets, strum, whammy,
tilt and pickup switch as you play; a drum kit flashes each pad and cymbal as brightly as it
was hit, with its velocity (an RB1 kit has none, so every hit shows at full strength), and
both kick pedals; a controller shows its buttons, sticks and triggers, and the instrument
"Gamepads play as" makes it in the game. With the SDL input backend an Xbox 360 guitar or
kit shows as a controller too, and plays as that instrument. A MIDI kit also lists its last
notes with their velocity and what they play. What it shows is what the game will read: the
launcher reads the devices through band3's own input drivers.

Any controller can drive the launcher: the d-pad or left stick moves, A picks, B backs out,
LB and RB switch tabs, and Start plays. A guitar's frets and a kit's pads act as the face
buttons (green is A, red is B), and strumming moves up and down, except while it's being
tested. Leaving the Controllers tab, unplugging the device or pressing Play ends a test too.

The instrument settings on the Controllers tab apply as you change them, except **Input
backend** (SDL or XInput): changing it applies when you press Play, which saves and restarts
band3 on the new backend (the row says so). Under the [test harness](test-harness.md)
band3 doesn't restart itself, so the game keeps the old backend until the next start. A
changed MIDI port reopens the kit a moment after you stop changing it.

The Online tab turns on the web song browser, Discord, RB3Enhanced's events, and online
play: [GoCentral and Liveless](integrations.md#gocentral-rock-central) (Windows only).
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

To get back to it, hold Shift while band3 starts, start it with `--launcher`, or turn
`show_launcher` on in F4 (Band3 → Launcher). On a Steam Deck in Game Mode, add
`--launcher` to band3's launch options in Steam (Properties → Launch Options), and take it
out again afterwards. `launcher = true` in `band3.toml` is ignored: it's for one start, from
the command line.

The launcher saves to `band3.toml` beside the executable, and writes only the settings you
changed: one that matches its default (what band3 would use without the file: the Steam
Deck setting on a Deck, then `band3_config.ini`'s, then band3's own) is left out, and the
file's other lines are kept. **Show this screen at startup** is the exception: every save
writes it, as `show_launcher = true` or `false`. A changed setting has a **Reset** button that puts it back to
its default and takes it out of the file at the next save. Folders are saved with forward
slashes, and a folder inside the ini's folder (or the executable's, without an ini) is
saved relative to it, so the install still moves as one folder. If `band3.toml` can't be
read, the launcher says why; saving then replaces it and keeps the old file as
`band3.toml.bak`. The launcher doesn't keep comments: the first time it saves over a
`band3.toml` you wrote comments in, it keeps that file as `band3.toml.bak` too. If
`band3.toml` can't be written (a read-only folder, say), Play says why and offers to play
anyway with the settings as they are, for that session only.

A setting given on the command line or by a `REX_*` environment variable is shown but
locked ("Set on the command line", "Set by an environment variable"), since a saved value
wouldn't win over it. On a Steam Deck a banner at the top says so, with the
`steam_deck_defaults` switch: on, the [Deck's settings](#steam-deck) are the defaults and
what you change wins over them.

Some settings stay in F4 only: debug options, heap sizes, the native renderer's debug
options and the virtual instrument.

## The settings menu

Press **F4** in game to open the settings menu. band3's own options are under the
**Band3** categories (Game, Graphics, Integrations, Online, MIDI drums, Microphones, Debug), next
to the SDK's window, graphics, audio and input settings. Settings marked as needing a
restart take effect the next time the game starts; the others apply immediately, or from
the next time the game loads what they affect (for example, a forced venue applies from
the next venue load).

Without a keyboard, hold both stick clicks on a controller for a second to open the
settings menu, or both stick clicks and the left bumper for the
[Instrument Lab](instruments.md#instrument-lab). The same chord closes them.
`menu_shortcut` (Band3 → Game) turns this off. It works with either input backend: it
watches the buttons the game reads from each player's controller, so it needs the
controller connected as a player, and works once the game is running (not on the
launcher). Instruments don't count, since a guitar's solo frets and a drum kit's pads and
second kick send stick clicks too.

## Config files

`band3.toml` next to the executable holds band3's settings; the [launcher](#the-launcher)
writes it. Any setting can also be passed on the command line, e.g.
`--forced_venue=arena_04`, or by a `REX_*` environment variable.

F4's **Save to config** writes `band3.toml` too, but differently: it rewrites the whole file
with every setting that differs from the SDK's default, whatever set it, so values from
`band3_config.ini`, the Steam Deck settings and the command line are frozen into the file
from then on, and anything else in the file is dropped. Prefer the launcher, which writes
only what you change.

`band3_config.ini` is still read and documents every option. Where the same setting is
set in more than one place, the first of these wins:

1. the command line
2. `REX_*` environment variables
3. `band3.toml`
4. the [Steam Deck](#steam-deck) settings, on a Deck
5. `band3_config.ini`
6. the defaults

Some options worth knowing about, by the names F4 and the command line use:

| Option | |
|---|---|
| `frame_cap` | what paces the game's frames, in place of the emulated console's vertical blank. `display` runs the game at the display's refresh rate exactly (119.88 Hz, not 120): best on a fixed-refresh display, and the default. `auto` runs it a little under (5% less, at least 4 fps: 114 at 120 Hz, 136.8 at 144): best on a VRR display (G-Sync, FreeSync), where it keeps every frame inside the display's range; on a fixed-refresh display a cap under its refresh rate shows a frame twice every 1/(refresh − cap) seconds. A number (24 to 240, e.g. `117`) caps there. `off` leaves the console's vertical blank to pace it, as `vsync` and `refresh_rate` say. With the cap on, `vsync` is turned off for the session (F4's Save to config then writes `vsync = false`, which matters only if `frame_cap` is later `off`), and `refresh_rate`, unless set, is set to the cap's rate. `display` and `auto` are off when the display's rate can't be told (on Linux, until the native view has run). Changes apply at once; the display is read again every 2 seconds |
| `refresh_rate` | the rate the game runs at, e.g. 120 for a 120 Hz monitor (0 keeps the console's 60, or follows `frame_cap`) |
| `background_fps` | the venue's frame rate: 0 keeps the venue's own (30 in most) at any `refresh_rate`, or set one, up to `refresh_rate`. Rates that divide `refresh_rate` (30 at 120, 180 or 240) draw evenly |
| `forced_venue` | a venue, a class of venues, or a comma-separated mix to pick from at random |
| `song_speed`, `track_speed` | play songs faster or slower, or scroll the highway faster |
| `controller_type`, `input_backend` | what gamepads play as, and SDL (the default) or XInput |
| `fast_start`, `disable_metamusic`, `lang`, `username` | skip the splash screens, silence the menu music, force a language, set the displayed name |
| `unlock_clothing`, `gold_on_all_difficulties` | RB3Enhanced's unlock options; see [Integrations](integrations.md#rb3enhanced-compatibility) |
| `game_origin_icons` | on (the default), each song in the song list shows an icon for the game or pack it came from, given the files it needs; see [Song source icons](integrations.md#song-source-icons) |
| `autosave` | off keeps play out of your profile; it then only saves from the options menu |
| `skip_profile_prompt` | on (the default), players who join without a profile join as guests at once; off, the game asks each to choose a profile, as on a console |

## Folders

Files the game writes to its own folder (`game:\`) go to `game` in the user data root
(`Documents\band3\game` on Windows, or under `--user_data_root`), and the game reads them
back from there; `assets` is never written. Rock Band 3 Deluxe keeps its settings,
modifiers and playlists there (`dx_settings.dta`, `dx_playlist.dta` and so on), so
deleting that folder resets them. Test runs have their own user data, so they don't
change yours.

`band3_config.ini` can also move band3's other folders: `user_data_root` (saves, profiles
and the `game` folder; `Documents\band3` by default) and `cache_root` (the shader cache;
`cache` in the user data folder by default), and so can `band3.toml`
(`game_data_root`, `user_data_root`, `cache_root`; the launcher's Game tab), which wins over
the ini. A relative path in either is relative to the ini's folder, or to the executable's
folder when there is no ini, so `user_data_root = user_data` keeps everything band3 writes
beside it, for a portable install. band3 looks for the ini in its working directory, then
beside the executable.

## DLC and custom songs

band3 reads DLC and custom songs (Xbox 360 `CON`, `LIVE` and `PIRS` packages, Rock Band
and Rock Band 2 DLC included, as RB3 reads them) straight from the folders
`content_folders` names (Band3 → Game, or `[game]` in `band3_config.ini`);
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
16:10), at the console's 60 Hz (`frame_cap` off) with vsync on and the FPS counter off, since Steam's performance overlay does
that job. These only fill in settings that `band3.toml` and the command line leave unset,
but they win over `band3_config.ini`, whose window settings are for a desktop. Turn
`steam_deck_defaults` off (Band3 → Game, then restart) to go back to the ini's.

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
