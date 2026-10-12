# Settings, folders and songs

## The launcher

The launcher is band3's setup screen. It opens in the game window before the game starts,
with tabs for the game and its folders, graphics, audio, controllers, online features, Stage
Kit lights and music videos. It works with the mouse, the keyboard or a controller, and
pointing at a setting shows what it does.

- Changes apply as you make them. **Save** keeps them for next time, **Play** saves and starts
  the game, and **Close** quits, asking first if something isn't saved.
- A changed setting's **Reset** button puts it back to its default.
- Devices (mics, MIDI ports, monitors) are picked from lists of what's connected. A saved
  device that isn't plugged in shows as "(not connected)" and stays chosen; **Other...** takes
  a typed name or part of one.
- A setting given on the command line or by a `REX_*` environment variable is shown locked,
  since a saved value wouldn't win over it.
- Some settings are only in the [in-game settings](#pause-menu-and-in-game-settings): the
  Advanced tab and each tab's **More settings**.

**Controller navigation:** the d-pad or left stick moves, A picks, B backs out, LB and RB
switch tabs, and Start plays. A guitar's frets and a kit's pads act as the face buttons
(green is A, red is B), and strumming moves up and down.

### Tabs

- **Graphics** starts with the settings that trade lag ([Playing with the least
  lag](#playing-with-the-least-lag)), then **Renderer**: Native (the default on Windows and on a
  Steam Deck), Emulated (the default on other Linux machines) or Native + emulated (debug). A
  change to or from Native restarts band3 at Play. The tab shows only the sections the chosen
  renderer uses ([which settings apply](native-renderer.md#which-settings-apply)).
  - **Fill the window** (on by default) draws the game at the window's shape with the native
    renderer, with no black bars ([details](native-renderer.md#filling-the-window)).
  - **Resolution limit** caps the lines the native renderer draws, for a GPU that can't keep up
    at the window's size.
  - **Aspect** (letterbox or stretch) applies to the game's 16:9 picture: the emulated GPU's,
    or the native renderer's with Fill the window off.
  - **VSync** shows only with the frame rate cap off, since the cap turns it off.
- **Audio** lists the PC's microphones for each mic slot, with a level meter beside each.
- **Controllers** lists every controller and instrument band3 sees and which player each one
  is. Pick one to test it live: frets, strum, whammy and tilt; pads and cymbals flashing as
  hard as they're hit; buttons and sticks, and what "Gamepads play as" makes it. While tested
  it doesn't drive the launcher; press Back, click Stop or leave the tab to end the test. It
  reads devices through band3's own drivers, so what it shows is what the game will read.
  Settings apply at once, except **Input backend** (SDL or XInput), which restarts band3 at
  Play (not under the [test harness](test-harness.md)).
- **Online** turns on the web song browser, Discord, RB3Enhanced's events, [Home
  Assistant](integrations.md#home-assistant), [GoCentral and
  Liveless](integrations.md#gocentral-rock-central). They're read as the game starts, so Play
  applies them without a restart.

### When it opens

- the first time band3 starts, including after updating to a version that has it;
- when the game data folder doesn't look like Rock Band 3's (the Game tab says what's wrong);
- when Shift is held as band3 starts (Windows only), or with `--launcher`;
- otherwise only if **Show this screen at startup** was ticked at the last Save or Play. It
  starts unticked, so after the first Play band3 goes straight into the game.

It never opens when RB3E's `rb3e_relaunch_game` restarts the game, nor for a [test
harness](test-harness.md) run unless started with `--launcher`. To get it back, hold Shift,
start with `--launcher`, or tick **Show the launcher at startup** in the in-game settings. On a
Steam Deck in Game Mode, add `--launcher` to band3's launch options in Steam for one start.

### What it saves

The launcher saves to `band3.toml` beside the executable: only the settings that differ from
their default, plus **Show this screen at startup**, keeping the file's other lines. Folders
inside the install are saved as relative paths, so the install still moves as one folder.

- It doesn't keep comments: saving over a `band3.toml` with comments, or one it couldn't read,
  keeps the old file as `band3.toml.bak`.
- If `band3.toml` can't be written, Play offers to play with the settings as they are, for that
  session only.
- On a Steam Deck a banner shows the `steam_deck_defaults` switch: on, the [Deck's
  settings](#steam-deck) are the defaults and what you change wins over them.

## Playing with the least lag

Rock Band 3 judges each hit on the game's frames, so the game's frame rate, not the display's,
sets how finely hits are timed: within 16.7 ms at 60 fps, 8.3 ms at 120 and 4.2 ms at 240.
The Graphics tab's **Latency** section has the settings that trade lag:

- **Frame rate cap** (`frame_cap`) paces the game, up to 240, and can run it faster than the
  display: each refresh then shows the newest frame, so a higher cap also means a fresher
  picture.
- **Smooth frame pacing** (`native_present_pacing`, native renderer, on by default) hands each
  frame to the window a steady delay after it's made, so motion steps evenly. Off is about 3 to
  4 ms sooner at 120 fps, with the occasional uneven step. It only holds frames with the cap
  under 1.5 times the display's refresh rate and not `auto`.
- **Lowest latency** sets the cap to the most whole refreshes' worth of frames up to 240 (240
  on a 60 or 120 Hz display, 144 on a 144 Hz one), and over 160 Hz turns smooth frame pacing
  off too. If the game can't hold that rate, lower the cap.

The FPS counter (`debug_overlay`) shows the game's frame rate (**Game**), the window's
(**Window**, held back by the display's refresh rate), and with the native renderer **Frame to
window**, how long a frame waits before it's shown. On a 60 Hz display that wait was 12.6 ms at
a 120 cap with smooth frame pacing, 9.0 ms without, and 7 to 8 ms at 240.

After changing any of these, run the game's calibration again (in its Options).

## Pause menu and in-game settings

Press **Escape**, or hold both stick clicks on a controller for a second, to open band3's pause
menu: **Resume**, **Settings**, **Instrument Lab** and **Quit game**.

- During a song it pauses the song too, with the song's own pause menu underneath; **Resume**
  (or Escape, B or Start) resumes it. In the first couple of seconds of a song, before the game
  takes a pause, the menu opens over the song as it plays.
- In the game's menus nothing is paused, but the game reads no input while the menu is open.
- **Instrument Lab** opens the [Instrument Lab](instruments.md#instrument-lab), leaving a song
  paused. **Quit game** asks, then closes band3.

| Shortcut | Opens |
|---|---|
| Escape (`bind_pause_menu`) | the pause menu; not while the launcher shows |
| both stick clicks, held a second | the pause menu |
| both stick clicks and LB, held a second | the [Instrument Lab](instruments.md#instrument-lab) |

The same chords close them. They work with either input backend once the game is running, from
a controller connected as a player; instruments don't count, since their solo frets and pads
send stick clicks too. `menu_shortcut` (Controllers → More settings) turns them off.

### Settings

**Settings** opens band3's settings over the game, with the launcher's tabs and rows plus an
**Advanced** tab. While they're open the game gets no input: arrows or the d-pad move, Enter or
A picks, LB and RB switch tabs, and B, Start or Escape go back.

- Changes apply as you make them, or from the next time the game loads what they affect. A row
  that needs a restart says **Applies at the next start** once changed: the folders, most of
  the Online tab, the instrument drivers, the input backend, the microphones, the emulated
  GPU's anti-aliasing, and the renderer to or from Native.
- **Save** writes `band3.toml` as the launcher does. **Back** asks first if something isn't
  saved; unsaved changes last until band3 closes.
- Not here, since the game is using the devices: the controller tester (use the Instrument Lab,
  F6) and the mic level meters.
- **Advanced** lists band3's technical settings by group: Game code, Graphics, Logging, Memory
  (the heap sizes), Native renderer, Startup (read-only) and Test harness. Each tab also ends
  with **More settings**: settings the launcher doesn't show, such as `liveless_rooms` and the
  FPS counter.
- **All settings...** opens the SDK's own menu, with every setting by category (band3's under
  **Band3**) and the key binds. Its **Save to config** writes `band3.toml` differently from
  band3's Save ([below](#config-files)).

## Config files

`band3.toml` next to the executable holds band3's settings; the launcher and the in-game
settings write it. Any setting can also be passed on the command line (`--forced_venue=arena_04`)
or by a `REX_*` environment variable. The [settings reference](settings-reference.md) lists
every one, with its default and what it does, generated from the build.

Prefer band3's Save to the SDK menu's **Save to config**. That one rewrites the whole file with
every setting that differs from the SDK's default, whatever set it, freezing in values from
`band3_config.ini`, the Steam Deck and the command line, and drops anything else in the file.

`band3_config.ini` is the settings file from before `band3.toml`. band3 still reads it (in its
working directory, then beside the executable), but releases no longer include it and it has
only [some of the settings](settings-reference.md#band3_configini). It also marks the folder
that relative paths start from ([Folders](#folders)). Where a setting is set in more than one
place, the first of these wins:

1. the command line
2. `REX_*` environment variables
3. `band3.toml`
4. the [Steam Deck](#steam-deck) settings, on a Deck
5. `band3_config.ini`
6. the defaults

Settings worth knowing about:

- `frame_cap`, `background_fps` ([Graphics](settings-reference.md#graphics)) and
  `video_mode_refresh_rate` ([the SDK's](settings-reference.md#more-of-the-sdks-settings)):
  pace the game and its venues.
- `forced_venue`, `song_speed`, `track_speed` ([Game](settings-reference.md#game)).
- `controller_type`, `input_backend` ([Controllers](settings-reference.md#controllers)).
- RB3Enhanced's `unlock_clothing` and `gold_on_all_difficulties`
  ([Integrations](integrations.md#rb3enhanced-compatibility)), and `game_origin_icons` ([Song
  source icons](integrations.md#song-source-icons)).

## Folders

| Setting | Default | Holds |
|---|---|---|
| `game_data_root` | `assets` | the game's files; never written |
| `user_data_root` | `Documents\band3` | saves, profiles and the `game` folder |
| `cache_root` | `cache` in the user data folder | the shader cache |

Set them on the launcher's Game tab, in `band3.toml` or in `band3_config.ini`. A relative path
is relative to the ini's folder, or the executable's without one, so `user_data_root =
user_data` makes a portable install.

Files the game writes to its own folder (`game:\`) go to `game` in the user data root. Rock Band
3 Deluxe keeps its settings, modifiers and playlists there (`dx_settings.dta` and so on), so
deleting that folder resets them. Test runs have their own user data.

## DLC and custom songs

band3 reads DLC and custom songs (Xbox 360 `CON`, `LIVE` and `PIRS` packages, Rock Band and
Rock Band 2 DLC included) straight from the folders `content_folders` names (the Game tab;
`songs` beside the ini by default). Nothing is installed or unpacked, and band3 never writes
there.

- Separate folders with `|`. Subfolders count, and relative folders work as above.
- Changes apply at the next launch, except songs the web page's
  [RhythmVerse](integrations.md#rhythmverse) tab downloads or finds, which the game takes in as
  it runs. A song taken out stays listed until the next launch.
- A local folder is safer and faster than a network one: songs stream from their package as
  they play, and a library that takes over a minute to read isn't listed that session.
- A custom song whose `song_id` is text gets the number RB3Enhanced gives it (the text's CRC-32
  mod 9999999, plus 2130000000), so IDs agree with RB3Enhanced's.

## Music videos

<img src="images/music-video.jpg" alt="No One Knows played with Queens of the Stone Age's music video behind the highway, the band hidden: the video's own band in place of the venue" width="640">

With `music_videos` on (the default), a song's own music video plays in the video venues in
place of their background clips, in time with the song, through pauses, restarts and practice
sections. Put the video in the `videos` folder beside the ini (or `music_videos_folder`), named
after the song's shortname: `20thcenturyboy.mp4` for 20th Century Boy. The log names the
shortname as a song starts (`No music video for 20thcenturyboy in music_videos_folder`).

| Setting (Videos tab) | Default | |
|---|---|---|
| `music_video_venue_chance` | `100` | how often, in percent, a song with a video goes to one of the seven video venues: 100 (always), 75, 50, 25, or 0 to leave the venue to the game. A forced venue and the Black Background modifier win. `forced_venue = video` puts every song in a video venue |
| `music_video_hide_band` | on | the band isn't drawn while a video plays; cameras, lights and [Stage Kits](integrations.md#stage-kit-lights) go on as before |
| `music_video_as_is` | on | the venue's colour filter, film grain, glow and trails are left off, so the video looks as it does in a player |
| `music_video_full_rate` | on | a 60 fps video plays at 60 fps, drawing the venue that fast (up to the game's rate). Costs about twice the venue's drawing and decoding |
| `music_video_fit` | `fit` | a video that isn't 16:9: `fit` with black bars, `fill` cutting its edges, or `stretch` |

Turning `music_videos` off stops the video at once; on starts videos from the next song. A
video's sound isn't played. The screen is black before the video starts, and its last frame
stays after it ends. As a song ends, the log says how the video played; "draws over two frames
behind" means the decoder didn't keep up.

**Formats:** FFmpeg decodes `.mp4`, `.m4v`, `.mov`, `.mkv`, `.webm`, `.avi` and `.wmv` in H.264,
HEVC, VP9, AV1 and most other codecs, so YouTube downloads play as they are.

- On Windows FFmpeg's DLLs come with band3. Without them, Windows decodes the videos itself
  (H.264; HEVC, VP9 and AV1 only with their Microsoft Store extensions).
- On Linux band3 uses the system's FFmpeg libraries, of the version it was built with.
- A video that can't be decoded is logged (`Music video ...`), and the venue keeps its clips.
- A 1080p video costs about a fifth of a CPU core besides decoding.

### Syncing a video

A video's start is set by `video_start_time = <milliseconds>` in `<shortname>.ini` beside it
(Clone Hero's key, as in a chart's `song.ini`). A positive value starts the video that far in;
a negative one starts it that far into the song.

- **Automatically** (`music_video_auto_sync`, on): the first time a song plays with its video,
  band3 matches the rhythm of the song's first 45 seconds against the video's soundtrack. When
  the match is clear it writes `video_start_time` and moves the video at once. A song whose
  structure repeats can match in several places; then nothing is written and the log gives
  the best guess. An offset already in the `.ini` is never replaced. Its working files are in
  a `.sync` folder beside the videos, safe to delete.
- **By hand:** while a video plays, `[` shows it 50 ms earlier and `]` 50 ms later
  (`bind_music_video_earlier`, `bind_music_video_later`); each press is saved to the `.ini`.
  Earlier is for a singer whose lips move after the sound.
- **On the Videos tab:** each video's offset (editable) and where it came from: synced, set by
  hand, not sure (with **Use it** for the best guess), or not played yet. **Align** matches a
  video with its song again, after replacing it with another cut. **Open the videos folder**
  opens it.

## Loose files (mods)

As with RB3Enhanced, a file on disk replaces the same file in the ARK, so mods such as custom
characters install without rebuilding the ARK. Put the file in the game data folder (`assets`,
or `game_data_root`) at its path inside the ARK, with its Xbox name (`gen` folders and `_xbox`
extensions). Files packaged for RB3Enhanced go in as they are, `(..)` folders included.

- Files in the user data root's `game` folder ([Folders](#folders)) replace ARK files too.
- band3 lists these folders as it starts: a new file is used from the next launch, a changed
  one the next time the game loads it.
- A path over 250 characters is left out, with a warning, since the game can't open it.
- Each replaced file is logged as `NewFile: <path>`. If a mod doesn't show up, set `log_level =
  debug` to also list every file read from the ARK, marked `(ARK)`.

## Steam Deck

On a Steam Deck, band3 defaults to:

- fullscreen and letterboxed (the game is 16:9, the screen 16:10);
- `frame_cap` 60 with vsync on, and the FPS counter off (Steam's overlay does that job);
- the [native renderer](native-renderer.md), which holds 60 fps in a song where the emulated
  GPU managed 48.

These fill in only what `band3.toml` and the command line leave unset, but win over
`band3_config.ini`. Turn `steam_deck_defaults` off (the Steam Deck banner, then restart) to go
back to the ini's. band3 recognises a Deck from Steam's `SteamDeck=1`, or on Linux from its
hardware ids. Keep `resolution_scale` at 1: the screen can't show more than 720p, and scaling
costs a lot of GPU time.

Instruments on a Deck:

- The Deck has one USB-C port, so more than one instrument or dongle needs a hub.
- For PlayStation and Wii dongles, install the
  [udev rules](instruments.md#playstation-and-wii-instruments-experimental) from Desktop
  Mode. SteamOS has no password for `sudo` until you set one with `passwd`.
- The log says what type each controller reports and what it plays as (for example
  "A controller reports type 1 (gamepad); playing as 7 (guitar)"). If an instrument
  shows up as a gamepad, turn off Steam Input for band3 (its controller settings in
  Steam) and try again, and include the log line if you report it.
