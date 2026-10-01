# slopband3 - AI Assisted band3 recompile experiment

Early recompilation of Rock Band 3 (title update 5). Playable, but just barely.

This is a fork of [ihatecompvir/band3_recomp](https://github.com/ihatecompvir/band3_recomp).
It targets ReXGlue SDK 0.10 and adds:

- An in-game settings menu (F4) backed by cvars, saved to `band3.toml`
- Per-device controller types, an SDL/XInput `input_backend` option, and an Instrument Lab (F6)
- RB3Enhanced-compatible network events (Stage Kit lighting, song/venue info) over UDP
- Native Discord Rich Presence
- A `refresh_rate` option for high-refresh monitors
- Unit tests, a compile check, and CI

## Prerequisites

Before building, ensure you have the following:

- [rexglue-sdk nightly 0.10.0.15-dev.g5cf287f](https://github.com/rexglue/rexglue-sdk/releases/tag/nightly-20260925-5cf287f4) (nightly-20260925-5cf287f4). The
  plain v0.10.0 release isn't enough: band3 needs this nightly's XInput changes. Unpack
  the zip for your platform and put the folder inside it (`win-amd64`, `linux-amd64`, ...)
  at `.rexglue-sdk` in the repository root, where the CMake presets look for it, so that
  `.rexglue-sdk/include/rex/version.h` exists.
- A copy of Rock Band 3 (Xbox 360) with Title Update 5 (TU5) XEX

## Building

### Windows

Prerequisites
   - [rexglue-sdk nightly-20260925-5cf287f4](https://github.com/rexglue/rexglue-sdk/releases/tag/nightly-20260925-5cf287f4), in `.rexglue-sdk` (see above)
   - Visual Studio with "Desktop development with C++" installed
   - cmake
   - ninja
   - clang

1. Clone the repository:
   ```
   git clone https://github.com/idio-sync/band3_recomp
   cd band3_recomp
   ```

2. Set up assets:
   - Create an `assets` folder in the root of the repository
   - Place the Rock Band 3 TU5 `default.xex` inside `assets`
   - Place the Xbox `gen` folder inside `assets`
     - The `gen` folder must contain both `main` and `patch` ARK files

3. Build:

   From the root of the repository in Command Prompt, run:

   ```
   rexglue codegen band3_manifest.toml
   cmake --preset win-amd64-release
   cmake --build --preset win-amd64-release
   ```

### Linux

Prerequisites
   - build-essential
   - git
   - cmake
   - ninja
   - clang
   - [rexglue-sdk nightly-20260925-5cf287f4](https://github.com/rexglue/rexglue-sdk/releases/tag/nightly-20260925-5cf287f4), in `.rexglue-sdk` (see above)

1. Install required packages:
   ```
   sudo apt install build-essential git cmake ninja-build clang
   ```

2. Clone the repository:
   ```
   git clone https://github.com/idio-sync/band3_recomp
   cd band3_recomp
   ```

3. Set up assets:
   - Create an `assets` folder in the root of the repository
   - Place the Rock Band 3 TU5 `default.xex` inside `assets`
   - Place the Xbox `gen` folder inside `assets`
     - The `gen` folder must contain both `main` and `patch` ARK files

4. Build:

   From the root of the repository, run:

   ```
   rexglue codegen band3_manifest.toml
   cmake --preset=linux-amd64-release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
   ninja -C out/build/linux-amd64-release
   ```

## Checks

These run on every push (`.github/workflows/ci.yml`) and don't need the game.

Unit tests (no SDK needed):

```
cmake -S tests -B out/tests
cmake --build out/tests
ctest --test-dir out/tests --output-on-failure
```

Compile check: compiles everything in `src/` against the ReXGlue SDK without codegen,
using a stand-in for `generated/band3_init.h`. On Windows, run it from a Visual Studio
developer prompt:

```
cmake -S tools/compile_check -B out/compile_check -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_PREFIX_PATH=<path to the rexglue SDK>
cmake --build out/compile_check
```

## Settings

Press **F4** in game to open the settings menu. band3's own options are under the
**Band3** categories (Game, Graphics, Integrations, Debug), next to the SDK's window,
graphics, audio and input settings. Settings marked as needing a restart take effect
the next time the game starts; the others apply immediately, or from the next time the
game loads what they affect (for example, a forced venue applies from the next venue load).

Without a keyboard, hold both stick clicks on a controller for a second to open the
settings menu, or both stick clicks and the left bumper for the Instrument Lab. The same
chord closes them. `menu_shortcut` (Band3 → Game) turns this off. It reads controllers
through SDL, so it doesn't work with `input_backend = xinput`.

**Save to config** writes the changed settings to `band3.toml` next to the executable.
Any setting can also be passed on the command line, e.g. `--forced_venue=arena_04`.

`band3_config.ini` is still read and documents every option. Where the same setting is
set in more than one place, the command line wins over `band3.toml`, which wins over
`band3_config.ini`.

### Instrument Lab

Press **F6** to open the Instrument Lab. It connects a virtual Xbox 360 instrument
(guitar, drums, keys, or a Mustang or Squier pro guitar) as its own player (player 2
by default) and plays it with the mouse, showing the exact data it sends. It is a tool
for checking how the game reads each instrument without the hardware.

### Test harness

`tools/band3ctl.py` drives a running band3 from a script or a terminal: it presses
the virtual instrument's buttons, waits for screens, reads the game's state and takes
screenshots. With `test_port` set, band3 takes its commands on that port, on this
machine only, and connects the virtual instrument as player 1.

```
python tools/band3ctl.py launch --fresh       # start band3 minimized, on a fresh test profile
python tools/band3ctl.py state                # screen, song, venue, band, frame count
python tools/band3ctl.py press green+strum_down
python tools/band3ctl.py wait screen=splash_screen timeout=60s
python tools/band3ctl.py screenshot menu      # saved under screenshots/ next to the exe
python tools/band3ctl.py run tests/game/boot.b3t
python tools/band3ctl.py "hold up+orange; wait frames=60; pad; release all"
```

`launch` starts the game minimized without taking focus (`--show` to watch it), and
gives it its own saves in `out/test_user_data`, so tests never touch your profile. A
`.b3t` script is these commands one per line, with `#` comments; `run` stops at the
first one that fails, saves a screenshot of the moment and exits 1. Commands joined
with `;` share one connection, which `hold` needs: band3 lets go of everything held
when a client disconnects.

Render checks of the native view launch with `-- --native_view_record_targets=true`
(and `--readback_resolve=full` for the variant that compares against guest memory's
textures): RB3 composes a band's outfits once, in the main menu, so the passes it
draws into textures have to be recorded from launch, before any capture. It's off by
default, so a game that never uses the native view doesn't pay for it.
A fresh profile's band is made up at random, so each launch has different characters;
`--test_random_seed=<n>` (0, the default, is off) seeds RB3's random numbers with `n`
instead of the clock, so the same seed gives the same band on every launch (and on
every machine) and another seed another band. It fixes who is in the band and the song's
shot categories, but not every frame: animations and particles draw random numbers as
the frame timing gives, so poses and camera angles still vary a little between runs.
With `--test_random_seed=21`, Futurama's Fry (a cel-shaded Deluxe character) plays
guitar in the render songs below, in their 25 s capture.
`tests/game/render_song.b3t` plays a song with even/odd rendering off (every frame
draws everything) and captures four points through it; `render_song_evenodd.b3t` does
the same with it on, as the game ships, where a frame draws the world and the next
post-processes it and presents it with its own overlay: there each capture is such a
post frame's, with the world frame's world in front of its overlay, as the game shows
it, and its `capture <name> composed` fails if it isn't. Launch that one without
`--readback_resolve=full`.

| Command | |
|---|---|
| `instrument guitar\|drums\|keys\|mustang\|squier` | plugs in player 1's instrument, or replugs it as another |
| `unplug` | takes it out |
| `press <inputs> [ms]` | press and let go (100 ms); join inputs with `+` |
| `hold <inputs>`, `release <inputs>\|all` | |
| `hit <pad\|cymbal\|keyN\|string> [velocity] [fret]` | drums, keys `key0`-`key24`, pro guitar strings `low_e a_str d_str g_str b_str high_e` |
| `axis whammy\|tilt <0..1>` | |
| `state` | the screen, song, venue, band, frame count, each player's instrument, the band's `score` (the song's scoreboard, 0 when a song starts) and, while `usb_mics` records, the `mics` slots: what records each, whether the game connected it and the bytes `fed` to it |
| `pad [player]` | the buttons, triggers and sticks the game reads from a player (1-4) |
| `wait <condition> [timeout=30s]`, `expect <condition> [timeout=5s]` | `screen=`, `screen~` (contains), `in_game`, `menus`, `song=<shortname>`, `frames=<n>`, `score>=<n>`, `mic=<slot>` (connected and fed audio since the wait began) |
| `screenshot [name]` | |
| `capture [name] [composed]` | a screenshot and the native view's capture (`.cap`) of the same full frame, for render checks: `out/native_view_replay.exe <name>.cap side.png --compare <name>.png` puts the two side by side, and its `--list` and `--shade <draw>` show what each draw's shader was given (option word, constants, maps), `--pick X,Y` (with `--size`) which draw left a pixel, `--legacy-light` draws it with the placeholder lighting from before the game's and `--no-cull` without the cull mode each draw had (`--list`'s `cull`: 2 is `D3DCULL_CW`, 6 `D3DCULL_CCW`), which both backends apply as the game's device did; `skipped_shadow` and `skipped_pass` in the reply count the draws left out because their pass isn't the back buffer's colour (shadow casters; velocity and the rest). The capture also has the passes RB3 drew into textures (outfit composites, the crowd's impostors, blurs, post-processing): `--list` prints them, and both of the native view's backends draw those the frame samples (all but post-processing's) into render targets of their own, which the draws after them sample; `--dump-rt <DxTex hex>[:<version>]` writes what one holds (`.alpha.png` its alpha), `--rt-none` never uses guest memory's pixels for a render target and `--rt-guest` draws no texture passes, sampling guest memory alone. `passes` counts them and `passes_carried` those carried in from earlier frames (a band's outfits are composed once, in the main menu, so they're there only if the game was launched with `-- --native_view_record_targets=true`), `rt_sampled` the render target versions the draws sample, `rt_filtered` those whose pass the capture left out because all its draws were (shadow maps and the velocity buffer, for their draw mode; the spotlights' depth volume, whose draws have no material or geometry the capture draws) and `rt_missing` the others no pass in the capture made, so 0 means none it could have had is missing, and `rt_fallback` is `native_view_rt_fallback`: `guest` (the default) also keeps what guest memory holds of a render target, `none` only which texture and version it is. `proc_cmds` is what the frame drew (7 everything; with even/odd rendering 1 the world, 2 post-processing; -1 unknown), and `composed` says the capture has the world of `world_frame` in front of the overlay of its own `game_frame` (a post frame's, which shows the world frame before it; the capture waits for one, and `held_fallback` says none came in 30 frames, so it took the last). With `composed` after the name the command fails unless the capture is such a post frame (`proc_cmds` 2, `composed` true); its files are written either way. Also `<name>.gpu.png`, the native view's GPU backend drawing the capture at the screenshot's size (`gpu`, `gpu_ms` in the reply, `gpu_passes` the texture passes it drew and `gpu_rt_missing` its draws that sampled a render target nothing had drawn, which draw transparent black; or `gpu_error` when there's no GPU device or `native_view_backend` is `cpu`): `--compare <name>.png --image <name>.gpu.png` shows it beside the game's, `--diff <name>.gpu.png` measures it against the CPU's drawing. Beside it, `<name>.gpu.alpha.png` and `<name>.gpu.depth.png` are the GPU's scene target where the world's draws left it, before the overlay: its alpha (the bloom weight RB3's shaders write) and depth as grey, which replay's `--dump-alpha <png>` and `--dump-depth <png>` draw on the CPU (`--view alpha` or `--view depth` with `--diff` compares them). Replay's `post:` line says what post-processing was set to do (colour matrix, bloom, depth of field, the world camera) and its `check:` line whether the constants RB3's composite drew with agree. Both of the native view's backends apply it (depth of field, bloom or glare, the colour matrix; `src/Render/post_model.h`) where the world's draws end, before the overlay's; replay's `--no-post` leaves it out and `--post-only xfm|dof|bloom` applies one effect alone, and F7's Post-processing box turns it off in the live view. Launch with `-- --native_view_record_targets=true` for these, or a band's outfits (composed in the main menu) are missing; `--readback_resolve=full` isn't needed any more, but makes guest memory's textures of what RB3 renders (otherwise garbage) the fallback |
| `native_view on [<width>x<height>] [nopost]\|off\|stats` | the native view live, as F7 draws it (same worker, same `native_view_backend`) but without its window, at 1280x720 unless given a size, and without RB3's post-processing with `nopost` (`post` in the reply), to see what it costs; `off` stops it and the capturing with it. Each reply has `stats`: the backend, frames `captured` and `rendered` since `on`, `skipped_busy` (captured while it was still drawing another), each drawn frame's time in `ms` (`mean`, `p50`, `p95`, `max`; on the GPU the whole frame, uploads and reading back included) and the part after submitting in `wait_ms`, and the game's own `game_frames` and `game_fps` since the last `on` or `off`, and `rt_recording`: whether `native_view_record_targets` is `on`, and the texture passes the game drew in that time while capture was off, how many of them were recorded (those into textures that aren't drawn every frame or every other), their draws, and the game thread's `ms` recording them. `off`'s reply is the run it ends; `run` prints replies with `stats`. `tests/game/render_song_live.b3t` measures a song with it |
| `set <setting> <value>` | Band3 settings only |
| `quit` | |

Each player can have a virtual instrument: start a controller command with `p2`,
`p3` or `p4` for that player's (`p2 instrument drums`, `p2 hit red_pad`, `pad 2`);
without one it's player 1's. `state` lists every player's instrument.
`tests/game/multiplayer.b3t` plays a two-player song. Players 2-4 aren't signed in,
so RB3 asks each to choose a profile when they join a song: No Profile plays as a
guest.

`tests/game/usb_mic.b3t` sings a song's vocals through the USB mics' test tone (launch
with `-- --usb_mics=true --usb_mic_test_tone=220`) and waits for the score to go up.

Every instrument has `a b x y start back up down left right`; guitars add `green red
yellow blue orange strum_up strum_down solo`, drums `red_pad yellow_pad blue_pad
green_pad yellow_cym blue_cym green_cym kick kick2`, keys `overdrive`. Presses during
a screen transition are dropped, so wait for the screen you act on (and give a dialog
a moment to finish appearing). Submenus inside one screen (Play Now, Quickplay) don't
change the screen name. A song counts as `in_game` until you leave its results, and
`state` keeps the last song's details after that. Switching `instrument` reconnects
it, so press Start to join again, as a player would.

### Pro Keys and Pro Guitar (untested)

RB3 reads a keytar's keys and a pro guitar's frets and strings through an Xbox 360 system
call that ReXGlue doesn't implement, so band3 hands the game that data itself, for any
keytar or pro guitar the input system reports: the Instrument Lab's, or a real one on
Windows with `input_backend = xinput`. This hasn't been run against the game yet; the
Instrument Lab's keys and pro guitars are the way to check it, and its **Pro instruments**
tab shows, per player, the instrument type the game sees and the bytes band3 gave it. Keep
the SDK's stick
deadzones (`left_stick_deadzone_percentage`, `right_stick_deadzone_percentage`) at 0,
since these instruments send their keys and frets in the stick values.

### PlayStation and Wii instruments (experimental)

Turn on `hid_instruments` (F4, Band3 → Game, then restart) to play Rock Band guitars and
drum kits through their USB dongles:

- PS3 and Wii guitars and drum kits, and a PS3 or Wii MIDI Pro Adapter in drum mode
- PS4 guitars (MadCatz Stratocaster, PDP Jaguar) and drum kits (MadCatz, PDP)
- PDP Riffmaster and CRKD Gibson SG, in PS4 or PS5 mode

They show up as Xbox 360 instruments, each as its own player. This is new and hasn't been
tried on every model yet. The Instrument Lab's **Connected instruments**
tab (F6) shows each one's raw reports next to what the game receives; if one misbehaves,
press **Save a 5 second capture** while playing the part that goes wrong, and include the
file it writes to `logs/` (next to the executable) with the report.

On Linux the dongles need to be readable by your user; install
`tools/linux/70-band3-rock-band-instruments.rules` as described at the top of that file.

### MIDI drum kits

Turn on `midi_drums` (F4, Band3 → MIDI drums, then restart) to play an electronic drum kit
over MIDI as a Rock Band pro drum kit, without a MIDI Pro Adapter. It uses the first MIDI
input unless `midi_drums_device` names one (or part of one's name), and picks up a kit
plugged in after the game starts.

Notes follow the MIDI Pro Adapter's layout, as in RPCS3: snare 38, toms 48/45/41, hi-hat
42/46, ride 51, crash 49, kick 36, hi-hat pedal 44 (the second pedal). Change any of them
with `midi_drums_notes`, e.g. `44=Kick,40=Snare`, the same format as RPCS3's overrides.
The Instrument Lab's **MIDI drums** tab (F6) shows what each note you hit played.

The kit has no menu buttons, so as in RPCS3: hi-hat pedal three times then snare is Start,
then the rim is Select, and then kick holds the kick for the song category menu (snare or
floor tom lets go). Turn these off with `midi_drums_combos`.

### Microphones (experimental)

Turn on `usb_mics` (F4, Band3 → Microphones, then restart) to sing through microphones on
this PC as Xbox 360 USB microphones. The first mic slot uses the system's default
recording device unless `usb_mic_devices` names microphones (or parts of their names),
comma separated, one per slot, for harmonies. Microphones plugged in after the game starts
are picked up.

The game connects the mic, offers the vocal parts (Solo, Harmony) and scores what it hears:
`tests/game/usb_mic.b3t` sings a song's vocals with the test tone and checks the score goes
up. A USB microphone records and connects the same way; singing into one hasn't been
scored in a test yet. To check the game hears a mic slot without a microphone, set
`usb_mic_test_tone` to a pitch in Hz (e.g. 220): the first slot then sings that steady
tone, which the vocal track's pitch arrow holds. The Instrument Lab's **Microphones** tab
(F6) shows what records each slot, whether the game has connected it and how much audio it
has taken; the log reports the same steps (`USB mics: ...`).

### Controller lag

On top of calibration, RB3 builds in extra lag for each controller type, from the Xbox
hardware's own delay (45 ms for an Xbox guitar, 36 ms for Xbox drums). band3's PlayStation,
Wii and MIDI instruments reach the game as Xbox ones, so they get those numbers too. The
Instrument Lab's **Lag** tab (F6) shows, per player, the type the game sees and the lag it
uses. `joypad_lag` (Band3 → Game, then restart) changes it per type, as `type=ms`, comma
separated: `5=20,8=30` gives Xbox guitars 20 ms and Xbox drums 30 ms. `type=ms/video/audio`
also sets the lag the calibration tests assume, and a blank part keeps the game's number
(`8=/30/`). It's per type, so a real Xbox instrument of the same type changes too.

### Steam Deck

On a Steam Deck, band3 starts fullscreen and letterboxed (the game is 16:9, the screen
16:10), with vsync on and the FPS counter off, since Steam's performance overlay does
that job. These only fill in settings that `band3.toml` and the command line leave unset,
but they win over `band3_config.ini`, whose window settings are for a desktop. Turn
`steam_deck_defaults` off (Band3 → Game, then restart) to go back to the ini's.

band3 recognises a Deck from the `SteamDeck=1` Steam sets, or on Linux from the Deck's
hardware ids. Keep `resolution_scale` at 1: the Deck's screen can't show more than the
console's 720p, and scaling costs a lot of GPU time. The log warns when it's higher.

Instruments on a Deck:

- The Deck has one USB-C port, so more than one instrument or dongle needs a hub.
- For PlayStation and Wii dongles, install the udev rules above from Desktop Mode. SteamOS
  has no password for `sudo` until you set one with `passwd`.
- The log says what type each controller reports and what it plays as (for example
  "A controller reports type 1 (gamepad); playing as 7 (guitar)"). If an instrument
  shows up as a gamepad, turn off Steam Input for band3 (its controller settings in
  Steam) and try again, and include the log line if you report it.

### Profiling

Profiling is compiled into every build except Release. Build the `relwithdebinfo` preset
(e.g. `cmake --preset win-amd64-relwithdebinfo`, then
`cmake --build --preset win-amd64-relwithdebinfo`) and connect the
[Tracy](https://github.com/wolfpld/tracy) 0.13.1 profiler to the running game. Next to the
SDK's own zones, band3 marks RB3's engine systems (`RB3 Game::Poll`,
`RB3 WorldCrowd::DrawShowing`, `RB3 DxRnd::DoPostProcess` and so on), so a capture shows
where each frame goes. For captures that can be compared, turn on `autoplay`
(Band3 → Debug) and play the same song in the same venue each time. Turning `autosave`
off (Band3 → Game) keeps those runs out of your profile; it then only saves from the
options menu.

## Notes

- This project is in an early state and may not build or run correctly in all applications.
- Documentation will be improved as development progresses.
