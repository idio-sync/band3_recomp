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

- [rexglue-sdk](https://github.com/rexglue/rexglue-sdk/releases) v0.10.0
- A copy of Rock Band 3 (Xbox 360) with Title Update 5 (TU5) XEX

## Building

### Windows

Prerequisites
   - [rexglue-sdk](https://github.com/rexglue/rexglue-sdk/releases)
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
   - [rexglue-sdk](https://github.com/rexglue/rexglue-sdk/releases)

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

## Notes

- This project is in an early state and may not build or run correctly in all applications.
- Documentation will be improved as development progresses.
