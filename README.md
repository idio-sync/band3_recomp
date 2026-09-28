# band3 recompiled

Early recompilation of Rock Band 3 (title update 5). Playable, but just barely.

## Prerequisites

Before building, ensure you have the following:

- [rexglue-sdk](https://github.com/rexglue/rexglue-sdk/releases)
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
   git clone https://github.com/ihatecompvir/band3_recomp
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
   git clone https://github.com/ihatecompvir/band3_recomp
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

## Notes

- This project is in an early state and may not build or run correctly in all applications.
- Documentation will be improved as development progresses.
