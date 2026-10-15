# Building band3

## Requirements

- [rexglue-sdk nightly 0.10.0.15-dev.g5cf287f](https://github.com/rexglue/rexglue-sdk/releases/tag/nightly-20260925-5cf287f4)
  (nightly-20260925-5cf287f4). The plain v0.10.0 release isn't enough: band3 needs this
  nightly's XInput changes. Unpack the zip for your platform and put the folder inside it
  (`win-amd64`, `linux-amd64`, ...) at `.rexglue-sdk` in the repository root, where the
  CMake presets look for it, so that `.rexglue-sdk/include/rex/version.h` exists.
- A copy of Rock Band 3 (Xbox 360) with the Title Update 5 (TU5) `default.xex`, or Rock
  Band 3 Deluxe's patched XEX.
- cmake, ninja and clang, plus:
  - **Windows:** Visual Studio with "Desktop development with C++". FFmpeg for [music
    videos](settings.md#music-videos) comes with the source in `deps/`, a trimmed LGPL build
    (rebuilt by `tools/build_ffmpeg.sh` in MSYS2's MinGW64 shell). `-DBAND3_FFMPEG_ROOT=<folder>`
    uses another; `-DBAND3_FFMPEG=OFF` leaves it out.
  - **Linux:** `sudo apt install build-essential git cmake ninja-build clang libasound2-dev
    libcurl4-openssl-dev`. ALSA is for MIDI drum kits and libcurl for
    [RhythmVerse](integrations.md#rhythmverse); band3 builds without them, minus those
    features. FFmpeg for music videos comes with the source in `deps/`, as on Windows
    (rebuilt by `tools/build_ffmpeg_linux.sh`), since distributions ship different FFmpeg
    versions and band3 runs only with the one it was built with; without the zip, it builds
    against the system's (`pkg-config libavformat-dev libavcodec-dev libavutil-dev
    libswscale-dev`). It needs GCC 13's libstdc++ or later (Ubuntu 24.04
    has it), for `std::format` and `std::byteswap`.

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

The first configure downloads Microsoft's GameInput 3 package from NuGet (its header and
loader, for [Xbox One instruments](instruments.md#xbox-one-instruments-experimental)) into
the build folder. Without a connection the build goes on without Xbox One instruments,
saying so.

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

## Last.fm API key

Last.fm scrobbling ([Integrations](integrations.md#scrobbling-lastfm-listenbrainz))
signs its calls with an API account's key and shared secret. They aren't in the
repository: pass them to the configure as `-DBAND3_LASTFM_API_KEY=<key>
-DBAND3_LASTFM_API_SECRET=<secret>`. Without them, Last.fm scrobbling logs that it isn't
available. ListenBrainz needs nothing.

## Packaging

To give a build to someone else, build it, then:

```
python tools/package.py
python tools/package.py --build-dir out/build/linux-amd64-release
```

It writes `out/package/slopband3-<commit>-<build folder>.zip`: band3 and the libraries beside
it, `settings-reference.md`, the licenses (band3's and those of the libraries built into it)
and a `README.txt` with what to install, where the game files go and a link to the
commit's source. No game files. `band3.map` and `band3.pdb` go beside the zip, not in
it: keep them to read crash reports from that build. With uncommitted changes to tracked
files it stops unless `--allow-dirty`.

`python tools/publish_nightly.py <zip> --dispatch-linux <commit>` puts a zip on the
repository's rolling `nightly` release, replacing that platform's older one, and starts
the Linux nightly workflow (`.github/workflows/nightly-linux.yml`). That workflow builds
a pushed commit's Linux zip on GitHub, with Rock Band 3 Deluxe's published `default.xex`,
and publishes it the same way.

`docs/settings-reference.md` is generated: band3 writes it from its settings registry
(`band3 --settings_reference=<file>` quits without starting the game). After changing a
setting (its description in `src/settings.cpp`, or its row in
`src/Launcher/launcher_settings.cpp`), build and run `python tools/settings_reference.py`
to update it, from the Windows build, whose defaults it lists. `--check` only compares,
and `tools/package.py` stops on a Windows build whose reference differs.

## Crash reports

When band3 crashes (`abort()`, `std::terminate`, or an unhandled exception or fatal signal),
it writes the stack to the log and to `logs/crash-<start>-<pid>.txt` beside the executable.

- The report's header names the build and binary (link time stamp, or Linux build id).
- The next start tells the player where it is, and after a native renderer GPU hang suggests
  `renderer = emulated`.
- On Windows a minidump goes beside it (`crash-<start>-<pid>.dmp`, newest five kept), for
  WinDbg with the build's `band3.pdb`.

Frames are module+offset. `tools/symbolize.py` names band3's (from the matching `band3.map` on
Windows, `addr2line` on Linux) and guest addresses (from `band3_functions.toml`):

```
python tools/symbolize.py out/build/win-amd64-release/logs/crash-20261006-142122-29936.txt
python3 tools/symbolize.py --elf band3 logs/crash-20261006-142122-29936.txt
```

- Frames in the SDK's libraries stay unnamed: the SDK ships no symbols.
- `BAND3_CRASH_TEST=abort`, `terminate`, `access-violation` or `stack-overflow` in the
  environment crashes band3 that way, to check the reporting.
- `BAND3_FULL_DUMP=1` makes a Windows minidump hold all of band3's memory, the guest's
  included (gigabytes), for finding what overwrote it.

## Checks

These run on every push (`.github/workflows/ci.yml`) and don't need the game.

Unit tests (no SDK needed; the first configure downloads toml++ 3.4.0, for the launcher's
config writer):

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
(`cmake --preset win-amd64-relwithdebinfo`, then `cmake --build --preset
win-amd64-relwithdebinfo`) and connect the [Tracy](https://github.com/wolfpld/tracy) 0.13.1
profiler to the running game. band3 adds zones for RB3's engine systems (`RB3 Game::Poll`,
`RB3 WorldCrowd::DrawShowing`, ...) next to the SDK's.

For comparable captures, turn on `autoplay` (Advanced → Test harness), play the same song in
the same venue each time, and turn `autosave` off (the Game tab) to keep the runs out of your
profile.
