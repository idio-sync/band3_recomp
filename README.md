# slopband3: an AI-assisted band3_recomp experiment

A static recompilation of Rock Band 3 (Xbox 360, Title Update 5, or Rock Band 3 Deluxe)
into a native PC game, built on the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).
Playable, still a work in progress.

![A full band playing 20th Century Boy under band3: guitar, drums and bass highways below the vocal track](docs/images/gameplay.jpg)

This is a fork of [ihatecompvir/band3_recomp](https://github.com/ihatecompvir/band3_recomp).
Everything it adds was written with the help of AI, which did the vast majority of the
work. I wanted something I could keep on my Steam Deck/laptop so I didn't have to lug my
360 to parties anymore. There are other recomp projects that have not utilized AI as much
as this one has, if you'd rather go with one of those. This one was put together by me with
features I wanted to see and use, for my personal use. If you like it, awesome. If not, I
get it and don't blame you at all. Either way, rock on.

You need your own copy of the game; no game files are included. RB3 Deluxe is highly recommended.

## Features

**Playing**
- Local multiplayer: up to four players, each controller its own player
- DLC and custom songs (`CON`/`LIVE`/`PIRS`) read straight from folders, nothing to install
- Rock Band 3 Deluxe support
- A Quit button on the main menu
- The mouse in menus: point at a button or a song to highlight it, click to pick it, right click to go back
- Type on the song list to search it, with a Rock Band 3 Deluxe that has keyboard search
- Runs at the display's refresh rate (`frame_cap`)
- Forced venue, song speed and highway speed
- Steam Deck defaults: fullscreen, letterboxed, vsync

**Instruments**
- Xbox 360 instruments and gamepads, through SDL or XInput
- PS3, Wii, PS4 and PS5 Rock Band guitars and drums through their USB dongles (experimental)
- Electronic drum kits over MIDI, played as pro drums without a MIDI Pro Adapter
- MIDI keyboards, played as the keytar for Keys and Pro Keys, menus, pausing and overdrive included
- USB microphones, including harmonies (experimental)
- Pro Keys and Pro Guitar data
- Per-type controller lag
- The **Instrument Lab** (F6): a virtual instrument and a view of what the game reads from each one

**Integrations** (RB3Enhanced-compatible)
- Network events over UDP: Stage Kit lighting, song, band and venue info
- Stage Kits lit directly: Santroller and Xbox 360 Stage Kits plugged in by USB, and the RB3E Dashboard's Pico W wireless kits found and tested from the Lights tab
- A web page for browsing the song library from a phone and picking the next song, plus the RB3E web API
- Searching [RhythmVerse](https://rhythmverse.co) for custom songs from that page, and downloading them into the game without a restart
- Script functions, modifiers and unlock options, so Deluxe's RB3E features work
- Song source icons in the song list, from Deluxe's icons
- Rock Central's online features (leaderboards, Battles) through [GoCentral](https://github.com/ihatecompvir/GoCentral), using RB3Enhanced's server (Windows)
- Online play without Xbox Live (Liveless, Windows): by address, or by room code with Liveless Rooms (F10)
- Discord Rich Presence

<img src="docs/images/web-song-browser.png" alt="band3's web song browser: the library with album art and part difficulties, and the song now playing" width="520">

**Settings**
- A launcher: a setup screen for folders, graphics, audio, controllers and online features, shown on the first run (Shift at startup brings it back on Windows)
- A pause menu (Escape, or both stick clicks) that pauses a song, with Resume, Settings, the
  Instrument Lab and Quit game
- In-game settings (the pause menu's Settings): the launcher's tabs over the running game, plus
  band3's technical settings, saved to `band3.toml`; All settings... opens the SDK's own menu
- Configurable save, cache and song folders, including a portable install

**Graphics**
- A native renderer, the default on Windows: band3 draws each frame itself with the game's
  own shaders and no emulated Xbox 360 GPU, checked pixel by pixel against the emulated GPU
  - Faster: uncapped in a song, 442 fps against 234 on the emulated GPU; at 60 Hz the GPU
    is 13% busy against 28%
  - Sharper: it draws at the window's size (1080p, 4K), not the console's 720p
  - Fills ultrawide and 16:10 windows: the game draws at the window's shape, more of the
    venue to the sides or above and below, with the HUD and highways at their size in the
    middle and no black bars ([Filling the window](docs/native-renderer.md#filling-the-window))
  - `renderer` picks `native`, `emulated` (the Linux default) or `both` (F8 switches
    pictures); F9 opens its debug view
  - Tested only on Windows (Direct3D 12) so far; the Vulkan path for Linux is untested
  - [Native renderer](docs/native-renderer.md) has the settings and what still differs

**Development**
- A scriptable test harness, render checks against the game's own picture, unit tests and CI
- Tracy profiling zones on RB3's engine systems

## Quick start

1. Get the [ReXGlue SDK nightly-20260925-5cf287f4](https://github.com/rexglue/rexglue-sdk/releases/tag/nightly-20260925-5cf287f4)
   and unpack its platform folder to `.rexglue-sdk` in the repository root.
2. Put the game's `default.xex` and `gen` folder (with the `main` and `patch` ARKs) in `assets/`.
3. Build (Windows, from a Visual Studio developer prompt):
   ```
   rexglue codegen band3_manifest.toml
   cmake --preset win-amd64-release
   cmake --build --preset win-amd64-release
   ```
4. Run `out\build\win-amd64-release\band3.exe` from the repository root. The launcher opens
   on the first run.

See [Building](docs/building.md) for the full requirements, the Linux steps, the checks
CI runs and profiling.

## Documentation

| Page | Covers |
|---|---|
| [Building](docs/building.md) | requirements, Windows and Linux builds, unit tests, compile check, profiling |
| [Settings, folders and songs](docs/settings.md) | the launcher, the pause menu and in-game settings, config files, where band3 keeps things, DLC and custom songs, loose-file mods, Steam Deck |
| [Settings reference](docs/settings-reference.md) | every setting: its default, the values it takes and what it does, generated from the build |
| [Instruments and microphones](docs/instruments.md) | Instrument Lab, PlayStation/Wii dongles, MIDI drums and keyboards, USB mics, pro instruments, controller lag |
| [Integrations](docs/integrations.md) | network events, Stage Kit lights, Discord, the web server and its API, GoCentral, Liveless online play, RB3Enhanced and Deluxe compatibility |
| [Test harness](docs/test-harness.md) | `band3ctl`: driving the game from scripts, and the game tests |
| [Native renderer](docs/native-renderer.md) | the native renderer, render checks, capture replay and parity measurement |

## band3 and milo-native-engine

[milo-native-engine](https://github.com/freeqaz/milo-native-engine) takes the other route
to a native Rock Band 3: a port. Decompiled source (rb3-xenon's, for the Xbox 360 version)
is rebuilt as native 64-bit C++ and linked against a shared engine that renders with WebGPU
and provides audio, input and file access. band3 instead runs the game's own executable,
statically recompiled.

| | band3 | milo-native-engine |
|---|---|---|
| Game code | the Xbox 360 executable, recompiled from PowerPC; it runs in an emulated 32-bit address space, on ReXGlue's implementation of the Xbox kernel | the decompiled source, rebuilt as native 64-bit C++ |
| What it needs | only the executable: every function runs, decompiled or not | a complete, working decompilation |
| Rendering | a native renderer, which reproduces the game's own shaders and is checked against the game's picture pixel by pixel | WebGPU, with shaders of its own |
| Platforms | wherever ReXGlue runs: Windows and Linux | Windows, Linux, Mac and the web |
| Pros | playable now; the whole game runs as it shipped, so gameplay, timing, DLC and Deluxe behave as on a 360; the picture aims to match the 360's exactly | runs anywhere, the web included; the game can be changed at the source; no emulation layer, and ordinary C++ to debug |
| Cons | the emulated address space and kernel stay; changing the game means hooking recompiled functions; limited to ReXGlue's platforms | playable only once the decompilation is complete; the game behaves as the original only where the decompilation matches it; its own shaders don't reproduce the 360's picture |

## Credits

band3 stands on the work of these projects:

**Recompilation**
- [band3_recomp](https://github.com/ihatecompvir/band3_recomp) by ihatecompvir and its
  contributors: the original Rock Band 3 recompilation this fork builds on.
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk): the Xbox 360 recompiler and
  runtime band3 is built with.

**Decompilations**
- [rb3-xenon](https://github.com/freeqaz/rb3-xenon) by freeqaz: a decompilation of the
  same Xbox 360 TU5 executable. band3's function names are imported from its symbol map
  (`tools/import_rb3xenon_names.py`), and its source is the reference for band3's hooks,
  instrument handling and renderer.
- [dc3-decomp](https://github.com/rjkiv/dc3-decomp), the MiloHax decompilation of Dance
  Central 3: the source of rb3-xenon's Milo engine code.
- [rb3](https://github.com/DarkRTA/rb3), the decompilation of the Wii version of Rock
  Band 3: the source of rb3-xenon's game code.
- [milo-native-engine](https://github.com/freeqaz/milo-native-engine) by freeqaz: a
  reference for the native renderer.

**Rock Band community**
- [RB3Enhanced](https://github.com/RBEnhanced/RB3Enhanced): band3 follows its network
  event format, web API, script functions, modifiers and custom song IDs.
- [Rock Band 3 Deluxe](https://github.com/hmxmilohax/rock-band-3-deluxe) by MiloHax:
  supported as a mod, and the source of the song source icons.
- [PlasticBand](https://github.com/TheNathannator/PlasticBand) and
  [PlasticBand-Unity](https://github.com/TheNathannator/PlasticBand-Unity) by
  TheNathannator: the documentation behind band3's Xbox 360, PlayStation and Wii
  instrument support.

**Emulators**
- [RPCS3](https://github.com/RPCS3/rpcs3): band3's MIDI drum support is adapted from its
  emulated MIDI Pro Adapter.
- [Xenia](https://github.com/xenia-canary/xenia-canary): the native renderer reads
  textures, mip tails and the gamma ramp as Xenia does, and the shader research tools
  read its ucode dumps.

**Libraries**
- [RtMidi](https://github.com/thestk/rtmidi) (MIDI input),
  [inih](https://github.com/benhoyt/inih) (INI parsing),
  [miniupnpc](https://github.com/miniupnp/miniupnp) (UPnP port mapping),
  [stb_image_write](https://github.com/nothings/stb) (album art),
  [doctest](https://github.com/doctest/doctest) (unit tests),
  [Tracy](https://github.com/wolfpld/tracy) (profiling), and SDL3 through the ReXGlue SDK.

Rock Band 3 is a trademark of Harmonix Music Systems. This project is not affiliated with
or endorsed by Harmonix, MTV Games or Microsoft.

## License

[GPL-2.0](LICENSE.md).
