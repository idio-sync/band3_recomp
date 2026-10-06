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
window off. **Frame rate cap** is [`frame_cap`](#config-files); **VSync** shows only
with the cap off, since the cap turns it off.

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

To get back to it, hold Shift while band3 starts, start it with `--launcher`, or tick
**Show the launcher at startup** in the [in-game settings](#in-game-settings-f4) and save.
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

Some settings are only in the [in-game settings](#in-game-settings-f4): the Advanced tab's
(debug options, heap sizes, the native renderer's debug options, the test harness's and
the virtual instrument) and each tab's **More settings**.

## In-game settings (F4)

Press **F4** in game, or hold both stick clicks on a controller for a second, to open
band3's settings over the game. They have the launcher's tabs and rows, from the same
list, showing what the chosen renderer uses as the launcher does, and an **Advanced** tab.
Changes apply as you make them: at once, or from the next time the game loads what they
affect (a forced venue from the next venue load). A row whose change applies only from
the next start says **Applies at the next start** once you change it: the folders, most of
the Online tab, the instrument drivers, the input backend, the microphones, the emulated
GPU's anti-aliasing, and the renderer from Native to the other two or back. Pointing at a
setting shows what it does, and says if it's one of those. **Save** writes `band3.toml` as
the launcher does: only what you changed, the file's other lines kept. The footer says
when something isn't saved, and **Close** asks first; closing without saving keeps the
changes until band3 closes. **Show the launcher at startup** is the launcher's
[box](#the-launcher), saved with the rest.

While they're open the game gets no input: the keyboard and the controllers drive the
settings (arrows or the d-pad move, Enter or A picks, LB and RB switch tabs, and B, Start
or Escape go back to the game), and the game reads nothing pressed. They don't pause the
game, so a song keeps playing underneath: open them from a menu. They're drawn at the
launcher's scale: at 720p and on a Steam Deck's 1280x800 screen they fill most of it, and
they grow with the window. F4 does nothing while the launcher shows, as F6 and F9 don't.

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
note under it says so. F4, or its note's buttons, close it.

Without a keyboard, hold both stick clicks on a controller for a second to open the
settings, or both stick clicks and the left bumper for the
[Instrument Lab](instruments.md#instrument-lab). The same chord closes them.
`menu_shortcut` (the Controllers tab's More settings) turns this off. It works with either input backend: it
watches the buttons the game reads from each player's controller, so it needs the
controller connected as a player, and works once the game is running (not on the
launcher). Instruments don't count, since a guitar's solo frets and a drum kit's pads and
second kick send stick clicks too.

## Config files

`band3.toml` next to the executable holds band3's settings; the [launcher](#the-launcher)
and the [in-game settings](#in-game-settings-f4) write it. Any setting can also be passed
on the command line, e.g.
`--forced_venue=arena_04`, or by a `REX_*` environment variable.

The SDK's settings menu (F4, **All settings...**) has a **Save to config** that writes
`band3.toml` too, but differently: it rewrites the whole file with every setting that
differs from the SDK's default, whatever set it, so values from `band3_config.ini`, the
Steam Deck settings and the command line are frozen into the file from then on, and
anything else in the file is dropped. Prefer band3's Save, the launcher's or F4's, which
writes only what you change.

`band3_config.ini` is still read and documents every option. Where the same setting is
set in more than one place, the first of these wins:

1. the command line
2. `REX_*` environment variables
3. `band3.toml`
4. the [Steam Deck](#steam-deck) settings, on a Deck
5. `band3_config.ini`
6. the defaults

Some options worth knowing about, by the names the command line and F4's All settings... use:

| Option | |
|---|---|
| `frame_cap` | what paces the game's frames, in place of the console's vertical blank. `display` (the default) runs the game at the display's exact refresh rate (119.88 Hz, not 120), best on a fixed-refresh display. `auto` runs it 5% under (at least 4 fps: 114 at 120 Hz), best on a VRR display (G-Sync, FreeSync), keeping every frame inside its range. A number (24 to 240) caps there. `off` leaves the console's vertical blank to pace it, as `vsync` and `refresh_rate` say. With the cap on, `vsync` is off for the session (band3's Save keeps your own; the SDK's Save to config, under F4's All settings..., writes `vsync = false`) and `refresh_rate`, unless set, follows the cap. `display` and `auto` are off when the display's rate can't be told (on Linux, until the native view has run). Changes apply at once |
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
`steam_deck_defaults` off (the Steam Deck banner on the launcher or in F4, then restart) to go
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
