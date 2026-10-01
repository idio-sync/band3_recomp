# band3 recompiled

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

| Command | |
|---|---|
| `instrument guitar\|drums\|keys\|mustang\|squier` | plugs in player 1's instrument, or replugs it as another |
| `unplug` | takes it out |
| `press <inputs> [ms]` | press and let go (100 ms); join inputs with `+` |
| `hold <inputs>`, `release <inputs>\|all` | |
| `hit <pad\|cymbal\|keyN\|string> [velocity] [fret]` | drums, keys `key0`-`key24`, pro guitar strings `low_e a_str d_str g_str b_str high_e` |
| `axis whammy\|tilt <0..1>` | |
| `state` | |
| `pad [player]` | the buttons, triggers and sticks the game reads from a player (1-4) |
| `wait <condition> [timeout=30s]`, `expect <condition> [timeout=5s]` | `screen=`, `screen~` (contains), `in_game`, `menus`, `song=<shortname>`, `frames=<n>` |
| `screenshot [name]` | |
| `set <setting> <value>` | Band3 settings only |
| `quit` | |

Each player can have a virtual instrument: start a controller command with `p2`,
`p3` or `p4` for that player's (`p2 instrument drums`, `p2 hit red_pad`, `pad 2`);
without one it's player 1's. `state` lists every player's instrument.
`tests/game/multiplayer.b3t` plays a two-player song. Players 2-4 aren't signed in,
so RB3 asks each to choose a profile when they join a song: No Profile plays as a
guest.

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

### Microphones (experimental, untested)

Turn on `usb_mics` (F4, Band3 → Microphones, then restart) to sing through microphones on
this PC as Xbox 360 USB microphones. The first mic slot uses the system's default
recording device unless `usb_mic_devices` names microphones (or parts of their names),
comma separated, one per slot, for harmonies. Microphones plugged in after the game starts
are picked up.

This hasn't been run against the game yet. To check the game hears a mic slot without a
microphone, set `usb_mic_test_tone` to a pitch in Hz (e.g. 220): the first slot then sings
that steady tone, which the vocal track's pitch arrow should hold. The Instrument Lab's
**Microphones** tab (F6) shows what records each slot, whether the game has connected it and
how much audio it has taken; the log reports the same steps (`USB mics: ...`).

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
