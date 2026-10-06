# Building band3

## Requirements

- [rexglue-sdk nightly 0.10.0.15-dev.g5cf287f](https://github.com/rexglue/rexglue-sdk/releases/tag/nightly-20260925-5cf287f4)
  (nightly-20260925-5cf287f4). The plain v0.10.0 release isn't enough: band3 needs this
  nightly's XInput changes. Unpack the zip for your platform and put the folder inside it
  (`win-amd64`, `linux-amd64`, ...) at `.rexglue-sdk` in the repository root, where the
  CMake presets look for it, so that `.rexglue-sdk/include/rex/version.h` exists.
- A copy of Rock Band 3 (Xbox 360) with the Title Update 5 (TU5) `default.xex`, or Rock
  Band 3 Deluxe's patched XEX.
- cmake, ninja and clang. On Windows, also Visual Studio with "Desktop development with
  C++"; on Linux, `sudo apt install build-essential git cmake ninja-build clang
  libasound2-dev libcurl4-openssl-dev` (ALSA for MIDI drum kits, libcurl for
  [RhythmVerse](integrations.md#rhythmverse); band3 builds without them, less those).
  band3 uses `std::format` and `std::byteswap`, so Linux needs GCC 13's libstdc++ or
  later, which clang builds against too (Ubuntu 24.04 has it).

## Game files

1. Clone the repository:
   ```
   git clone https://github.com/idio-sync/band3_recomp
   cd band3_recomp
   ```
2. Create an `assets` folder in the repository root and put in it:
   - the TU5 (or Deluxe) `default.xex`
   - the Xbox `gen` folder, with both the `main` and `patch` ARK files

## Windows

From the repository root, in a Visual Studio developer command prompt:

```
rexglue codegen band3_manifest.toml
cmake --preset win-amd64-release
cmake --build --preset win-amd64-release
```

## Linux

```
rexglue codegen band3_manifest.toml
cmake --preset=linux-amd64-release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
ninja -C out/build/linux-amd64-release
```

PlayStation and Wii instrument dongles need a udev rule to be readable by your user; see
[Instruments](instruments.md#playstation-and-wii-instruments-experimental).

## Build tag

Each build names itself by `git describe` of the checkout (the log's `band3 build`
line, and the version a Liveless Rooms server sees). A build from source without
`.git` is `unknown` unless the configure passes `-DBAND3_BUILD_TAG=<tag>`, as the Nix
flake does with its revision.

## Packaging

To give a build to someone else, build it, then:

```
python tools/package.py
python tools/package.py --build-dir out/build/linux-amd64-release
```

It writes `out/package/band3-<commit>-<build folder>.zip`: band3 and the libraries beside
it, `band3_config.ini`, the licenses (band3's and those of the libraries built into it)
and a `README.txt` with what to install, where the game files go and a link to the
commit's source. No game files. `band3.map` goes beside the zip, not in it: keep it to
resolve crash traces from that build. With uncommitted changes to tracked files it stops
unless `--allow-dirty`.

## Checks

These run on every push (`.github/workflows/ci.yml`) and don't need the game.

Unit tests (no SDK needed):

The first configure downloads toml++ 3.4.0, the header-only dependency used by
the launcher's config writer. It requires internet access and checks the archive's
SHA-256 hash; subsequent builds reuse the download in the build directory.

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

For checks that run the game itself, see the [test harness](test-harness.md).

## Updating the SDK

CI downloads its own copy of the SDK, so when you move `.rexglue-sdk` to a newer nightly,
move CI and the docs along with it. Otherwise the compile check keeps building against the
old SDK and fails as soon as band3 uses something only the new one has.

1. In `.github/workflows/ci.yml`, set `REXGLUE_SDK_TAG` to the release tag (e.g.
   `nightly-20260925-5cf287f4`) and `REXGLUE_SDK_VERSION` to the version in its zip names
   (e.g. `0.10.0.15-dev.g5cf287f` from `rexglue-sdk-0.10.0.15-dev.g5cf287f-win-amd64.zip`).
   The SDK cache is keyed by that version, so CI downloads the new one on its next run.
2. Update the SDK link and version under [Requirements](#requirements) above and in the
   README's quick start.
3. Run the compile check above against the new SDK before pushing.

## Profiling

Profiling is compiled into every build except Release. Build the `relwithdebinfo` preset
(e.g. `cmake --preset win-amd64-relwithdebinfo`, then
`cmake --build --preset win-amd64-relwithdebinfo`) and connect the
[Tracy](https://github.com/wolfpld/tracy) 0.13.1 profiler to the running game. Next to the
SDK's own zones, band3 marks RB3's engine systems (`RB3 Game::Poll`,
`RB3 WorldCrowd::DrawShowing`, `RB3 DxRnd::DoPostProcess` and so on), so a capture shows
where each frame goes. For captures that can be compared, turn on `autoplay`
(F4's Advanced tab, Test harness) and play the same song in the same venue each time. Turning `autosave`
off (the Game tab) keeps those runs out of your profile; it then only saves from the
options menu.
