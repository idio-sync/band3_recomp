# Launcher design

Date: 2026-10-02. Status: phase 1 and phase 2 implemented (phase 2: the input hand-over, device lists, meters and decoders, the Audio, MIDI port and Graphics dropdowns, the Controllers device list and test view, and gamepad navigation).

## Goal

A mouse- and gamepad-driven settings screen that every player can use to set up band3 before the game boots: game files, song folders, display, graphics, microphones, instruments and integrations. Hardware gets real device lists and a live test view, so a player can see that a guitar, kit or mic works before starting.

The launcher is optional. band3.exe boots straight into the game with whatever was last saved; the launcher only appears when it is needed or asked for.

Success: a player never edits `band3_config.ini` or uses F4 for normal setup, and can confirm every instrument works without entering a song.

## Decisions already made

- **In band3.exe, not a separate program.** The launcher is an ImGui screen in the game window, shown from the SDK's `OnFinalizePaths` hook (after the window and ImGui exist, before the Runtime is built). Reasons: one Steam shortcut on the Deck, and the test view must use band3's own input drivers (HID dongles, RtMidi, SDL mics, `player_slots`) to show what the game will actually see. A separate exe would see SDL's generic copy of the devices.
- **`band3.toml` beside the exe is the one settings file.** Priority stays: command line > `REX_*` environment > `band3.toml` > Steam Deck presets > `band3_config.ini` > defaults.
- **The launcher writes only overrides.** A setting is written only when it differs from the default the player sees (the Steam Deck preset on a Deck). Each setting has a reset button that removes its key.
- **Steam Deck:** detected with `steam_deck::IsSteamDeck()`. A banner says so, with the `steam_deck_defaults` toggle. Presets appear as the starting values and can be overridden.
- **Debug settings are not shown** (`autoplay`, `test_port`, `native_view_*`, `virtual_instrument*`, heap sizes, …). They stay in F4.
- **Settings that need engine work first are left out until it lands:** audio output device, pinning a device to a player or instrument, a gameplay fps cap. No greyed-out placeholders.

## Phases

The work is two implementation plans, so the first is fully verifiable without hardware:

- **Phase 1 — launcher and settings.** Startup plumbing, when the launcher shows, the path rule, the game data check, the settings model and `band3.toml` writer, the screen with all five tabs (Controllers without the device list and test view; mic slots as typed names), mouse and keyboard navigation, close handling, Play, docs and harness tests. Its first task settles Open question 1 (FXAA).
- **Phase 2 — devices.** The input system hand-over (Open question 2), device lists for mics and MIDI, mic level meters, the Controllers device list and test view with decoders, gamepad navigation, monitor names (Open question 3).

## What the investigation found

These facts shape the design. Each was checked against code, the SDK's ReXApp source in `.rexglue-sdk/share/rexglue/rex_app.cpp`, or a probe exe linked against the SDK.

1. **Startup order.** `SetupEnvironment` (paths, `OnConfigurePaths`, `LoadConfig(band3.toml)`, `OnPostInitLogging`) → `SetupPresentation` (GPU plugin, `OnPreSetup`, window, ImGui, F3/F4/F7 binds, `OnCreateDialogs`) → `OnFinalizePaths` → `ConstructRuntime` (`Runtime::Setup` builds audio, input and the guest GPU; `AttachWindow`; `OnPostSetup`) → `LaunchModule`. Returning `nullopt` from `OnFinalizePaths` keeps the message loop running until `resume(paths)` is called on the UI thread; `resume` runs `ConstructRuntime` and `LaunchModule` synchronously.
2. **Most settings still apply if changed during the launcher**, because their readers run after `resume`: paths, `content_folders`, `hid_instruments`, `midi_drums*`, `input_backend`, `usb_mics*`, `joypad_lag`, `audio_maxqframes`, render scale (RequiresRestart in the registry, but read in `Runtime::Setup`), and the HotReload cvars (`fullscreen*`, `monitor`, `resolution`, `window_*`, `vsync`, `present_letterbox`, `anisotropic_override`, deadzones, band3's graphics tweaks, `renderer`, `background_fps`).
3. **Some band3 settings are snapshotted too early but read late.** `settings::Init` (in `OnPostInitLogging`) snapshots `StartupSettings`: `controller_type`, `rnd_sync`, `disable_metamusic`, `discord_enabled`, `http_*`, `events_*`, `rb3e_mode`, `native_camera_shake`, heap sizes. `AddSettingArgs` has already appended `-fast`, `-lang` and `-define MHX_PC` to the game's argv. All their consumers run after `resume`, so re-taking the snapshot and rebuilding the args on Play is enough; no relaunch.
4. **Truly too late:** `gpu_plugin`, `d3d12_adapter`, `video_driver` (not shown in the launcher). `swap_post_effect` (FXAA) is RequiresRestart and its read point is unknown — Open question 1.
5. **`SaveConfig` (F4's "Save to config") is unsuitable for the launcher.** It rewrites the file from scratch, writes every cvar that differs from its default whatever set it (command line, ini, Deck presets), drops unknown keys and comments, and does not escape strings: a Windows path with backslashes produces invalid TOML, and the SDK then ignores the whole file. `LoadConfig` accepts only flat `name = value` keys; unknown keys are kept and applied if the cvar registers later (that is how the xenos GPU cvars load).
6. **The SDK's `monitor` (0 default, 1 primary, 2 second…) and `resolution` (presets or `WxH`) cvars exist and apply live.** There is no audio output device cvar. FXAA is `swap_post_effect` (`none`, `fxaa`, `fxaa_extreme`). The SDK's `fullscreen` default is `true` and `audio_maxqframes` default is 8.
7. **band3.exe and rexruntime.dll each have their own SDL.** band3's copy runs the HID driver and mic capture; the SDK's copy runs gamepads. band3 never initialises gamepads in its own copy.
8. **The input drivers work without the Runtime.** The HID and MIDI drivers can be built and `Setup()` standalone. Their process-wide state (`g_active`, `driver()`) means two instances must never overlap. WinMM MIDI ports are single-client. The mic `Capture` singleton cannot be restarted, reads `usb_mic_devices` when first started, and its `Read()` consumes audio.
9. **The SDK's ImGui has no gamepad navigation** (`ConfigFlags = 0`, no gamepad key events).
10. **band3 never checks the game data folder.** The SDK shows a message box for a missing folder or `default.xex`; a folder without `gen/` gets further and fails inside the game.
11. **Closing the window hard-exits.** `ReXApp::OnClosing` calls `std::_Exit(0)`; the only veto is `OnWindowCloseRequested()`.

## When the launcher appears

`Band3App::OnFinalizePaths` decides:

- **Never** when this run is an RB3E relaunch, or when `test_port != 0` (test harness runs) without `--launcher` (a harness that drives the launcher asks for it; band3ctl then waits for the window, and the harness answers once Play starts the game). `WaitForPrevious` resets `relaunch_wait_pid` to 0, so it records the fact for a new `relaunch::WasRelaunched()`. `StartAgain` also drops `--launcher` from the command line it copies.
- **Always** when the game data check fails (below), when `--launcher` is on the command line, or when Shift is held at startup (`GetAsyncKeyState(VK_SHIFT)` on Windows; not on Linux in this design).
- **Otherwise** when the new cvar `show_launcher` is true. Its default is `true`, so the first run shows the launcher — including once for existing users after they update, which is intended. The footer has a "Show this screen at startup" checkbox, ticked when the launcher opens only if `band3.toml` has `show_launcher = true` (its source is `kConfig`: the player ticked it before), unticked otherwise. Save and Play always write `show_launcher`, `true` or `false`: `true` equals the default, and dropping its key under the override rule would untick the box at the next start.

New cvars (category `Band3/Launcher`): `show_launcher` (bool, default true) and `launcher` (bool, default false; meant for the command line). `docs/settings.md` explains the ways back to the launcher: Shift, `--launcher` (Steam launch options on a Deck), or `show_launcher` in F4.

If the launcher is not shown, `OnFinalizePaths` still applies the path rule below and returns the paths synchronously.

## Paths

**One anchor for relative paths.** Today relative paths resolve against `IniAnchor()`: the ini's folder, else the working directory. That fallback changes to the exe's folder, and every relative path band3 reads — from the ini, `band3.toml`, the launcher or `content_folders` — resolves against this anchor. The launcher writes a folder inside the anchor as a relative path (so a whole install still moves as one folder, as `paths.h` promises) and any other folder as an absolute path, always with forward slashes.

**The path rule.** The SDK builds `PathConfig` before `band3.toml` is loaded, so a path saved there never reaches the Runtime today. In `OnFinalizePaths`, start from the `defaults` PathConfig the hook receives (it already holds the ini values, the anchor and the cache rule from `OnConfigurePaths`). For each of `game_data_root`, `user_data_root` and `cache_root` whose value came from `band3.toml` or the launcher (not the command line or environment), replace it with the cvar resolved against the anchor. If `user_data_root` was replaced and `cache_root` was not set anywhere, the cache becomes `user_data_root/cache`, as in `OnConfigurePaths` today. `update_data_root`, `metadata_root` and `config_path` are kept from `defaults`. Call `band3::SetGameDataRoot` with the final game root. The folder log line from `OnPostInitLogging` is printed again with the final paths.

**Path defaults.** In the launcher, the effective default of each path setting is its value in `defaults`, not anything from the ini table.

## Game data check

A pure function `CheckGameData(path) -> {ok, problem}` reports, in order: folder missing; `default.xex` missing; `gen/main_xbox.hdr` missing. The Game tab shows the result next to the folder. The check is redone every second while the launcher is open, and again when Play is pressed; Play with a failing check asks for confirmation. A failing check at startup forces the launcher open (instead of the SDK's message box).

## Settings model

**The setting table.** `src/Launcher/launcher_settings.{h,cpp}` holds a curated, ordered table: cvar name, tab, section, label, widget, and optional value list (value → label), range or visibility condition (e.g. MIDI options only when MIDI drums is on). Descriptions shown in the footer come from the cvar registry, so they stay in step with `settings.cpp`.

**Values.** The launcher reads current values with `GetFlagByName` every frame (so a change made in F4 while the launcher is open shows up) and applies edits immediately with `SetFlagByName`, so HotReload settings (fullscreen, monitor, resolution) take effect on the launcher's own window.

**Effective default** — the value a setting would have with no `band3.toml` key — is the first of these that applies:

1. band3's forced startup value: `input_backend = sdl` on non-Windows, which `OnPostInitLogging` sets whatever else did.
2. The Steam Deck preset, on a Deck with `steam_deck_defaults` on. The preset table moves from `steam_deck.cpp` into a lookup, `steam_deck::Preset(name) -> optional<string>`, used by both. `ApplyDefaults` sets every preset nothing else set, also one equal to the cvar's current value, so its source records it and the ini (which fills only `kDefault` cvars) can't undo it: otherwise a desktop ini's `[window] fullscreen = false` would win over the Deck's `fullscreen = true`, the registry default.
3. The `band3_config.ini` value, read and normalised exactly as `ApplyLegacyIni` does (inih boolean spellings, `zero_is_unset`), through a new `LegacyIniValue(name) -> optional<string>` built on the same `kIniSettings` table. A value the cvar would refuse is skipped, as `ApplyLegacyIni` leaves the default then.
4. band3's own startup default: `audio_maxqframes = 3`, set only when nothing (the ini included) set the cvar, so it sits below the ini (`[audio] max_queued_frames` wins over it).
5. The registry default.

The order is the order startup applies them in: the Deck presets, then the ini, then band3's defaults, each filling in only what is still unset, with the forced value last over everything.

Paths are the exception (see Path defaults).

**Comparison is typed.** Current value and effective default are both parsed through the cvar's type before comparing, so `1.000000` equals `1` and `yes` equals `true`.

**Overrides.** On save, for every setting in the table: if its value differs from its effective default, write it; otherwise remove its key. `show_launcher` is the exception, always written (see above). Keys not in the table are left as they are. The reset button sets the value back to the effective default (and so removes the key on save).

**The Deck toggle.** When `steam_deck_defaults` changes in the launcher, each preset setting whose current value equals the old effective default and that the player has not edited moves to the new effective default, so turning the presets off does not save them as overrides.

**Locked settings.** When the launcher opens, before it sets anything, it records `GetFlagSource` for every setting in the table. A setting whose source is `kCommandLine` or `kEnvironment` is shown read-only with "Set on the command line" or "Set by an environment variable", since a saved value would not win.

**Typed values in the file.** Bools, ints and floats are written bare; strings are quoted.

The model is pure: it takes the table, current values, effective defaults, recorded sources and the existing file contents, and returns the new file contents. It is unit tested.

## The band3.toml writer

`src/Launcher/config_file.{h,cpp}`:

- Everything happens inside Save: read the file as it is now (F4 may have rewritten it while the launcher was open), merge, write.
- Reads with the SDK's toml++. If the file does not parse, the launcher shows a banner ("band3.toml couldn't be read: <error>; saving replaces it, and the old file is kept as band3.toml.bak"), and Save copies the old file to `band3.toml.bak` and starts from an empty table.
- Applies the overrides and removals; keeps every other key.
- **Emits by hand**, not with toml++'s serializer: one flat `name = value` line per key, strings as TOML basic strings with `\\` and `\"` escaped, floats in plain decimal. This keeps the output in the form the SDK's `LoadConfig` is known to accept.
- Writes via a temp file and rename. A failed write (a read-only install) doesn't block Play: it asks "Couldn't save your settings (<error>). Play anyway with these settings for this session?", and playing anyway starts with the settings as they are in the cvars; the footer keeps the error.
- Comments are not preserved. The first line is `# Written by the band3 launcher. F4 > Save to config rewrites this file.` A file with comments of the player's (anything but that line and F4's own header) is copied to `band3.toml.bak` before the rewrite, and the footer says so.

F4 stays as it is. `docs/settings.md` will say the launcher is the recommended way, and that F4's save freezes ini and Deck values into the file.

## While the launcher is open

- **Binds.** The SDK's F3, F4 (settings), F7 and backtick are safe without a Runtime and stay. band3's F6 (Instrument Lab) and F9 (native view) binds are unregistered while the launcher is up and registered again after `resume`. F8 only flips `renderer`, which the launcher shows anyway.
- **Closing the window.** `Band3App` overrides `OnWindowCloseRequested()`: while the launcher has unsaved changes it returns false and the launcher asks "Quit without saving?"; on yes it calls `window()->RequestClose()`. The launcher's Close button goes through the same path. Quitting before `resume` is a hard exit with no teardown, as today.
- **Threading.** Everything runs on the UI thread inside the launcher's `OnDraw`. Anything that changes the dialog list or builds the Runtime is deferred out of the draw (below).

## Play

1. Save (as above).
2. Re-take the startup snapshot: split `settings::Init` into registering change callbacks (once, at startup) and `SnapshotStartupSettings()` (at startup and again on Play).
3. Rebuild the game's argv: remove the `-fast`, `-lang` and `-define MHX_PC` entries `AddSettingArgs` added, then add them again.
4. Release every device the launcher opened (phase 2), then call `ClearPendingRestartFlags()` (it is global; everything the launcher sets is read after `resume`, except possibly FXAA).
5. Build the `PathConfig` (path rule above).
6. Draw one "Starting…" frame, then close the dialog (`ImGuiDialog::Close()`, which defers) and call `resume(paths)` through `app_context().CallInUIThreadDeferred`, never from inside `OnDraw`: `resume` builds the Runtime synchronously and `OnPostSetup` starts native presentation on the same drawer.

If Open question 1 shows FXAA must be set before the presenter exists, a changed `swap_post_effect` makes Play save and relaunch with `relaunch::StartAgain`, extended to take extra arguments (`--launcher=false` for that run).

## Layout

One full-window ImGui screen. Tabs: **Game, Graphics, Audio, Controllers, Online.** A Steam Deck banner under the tabs when on a Deck. A footer with the hovered setting's description, the "Show this screen at startup" checkbox, and Save / Close / **Play**. Each changed setting shows a reset button. A larger font is always added in `OnConfigureFonts` (it runs before the launcher decision) and selected by the launcher. Dropdowns and steppers are preferred over text fields, since a Deck in Game Mode has only the on-screen keyboard.

**Game.** Game data folder (Browse…, check result), song folders (`content_folders`, add/remove list, shown resolved), user data folder (`user_data_root`); language (`lang`), profile name (`username`); fast start, autosave, skip profile prompt, menu music (`disable_metamusic`), forced venue, song speed, track speed, unlock clothing, gold on all difficulties.

**Graphics.** Monitor (`monitor`; numbered in phase 1, named in phase 2), window mode (windowed / borderless / exclusive → `fullscreen`, `fullscreen_exclusive`), resolution (`resolution`, presets in phase 1, the monitor's display modes in phase 2; window size when windowed), renderer (`renderer`), render scale (`resolution_scale`, with the Deck warning above 1), anti-aliasing (`swap_post_effect`), anisotropic filtering (`anisotropic_override`), aspect (`present_letterbox`: letterbox / stretch), VSync (`vsync`), game frame sync (`rnd_sync`), background fps (`background_fps`), and the tweaks (`disable_hair_shader`, `disable_approximate_lights`, `compress_character_textures`).

**Audio.** "Use PC microphones" (`usb_mics`), Mic 1–4 (`usb_mic_devices`: typed names in phase 1; device dropdowns with live level meters in phase 2), audio buffer (`audio_maxqframes`).

**Controllers.** Instrument type (`controller_type`); PS3/Wii/PS4/PS5 dongles (`hid_instruments`); MIDI drums on/off, port (typed in phase 1, a dropdown in phase 2), minimum velocity, cymbal combos; input lag per controller type (`joypad_lag`, one number per type; the editor keeps any `video`/`audio` parts and types it does not edit); stick deadzones (shown as %); input backend (`input_backend`). Phase 2 adds the device list and test view above these.

**Online.** Web song browser (`http_enabled`, `http_port`), RhythmVerse (`http_rhythmverse`), Discord presence (`discord_enabled`), RB3E events (`events_enabled`, `events_target`, `events_port`), RB3E mode (`rb3e_mode`), GoCentral (`gocentral`, `gocentral_address`; warns while `username` is blank or "User", which band3 won't log in as), Liveless online play (`liveless`, `liveless_connect`, `liveless_external_ip`, `liveless_port`; warns when `liveless_connect` isn't an address). GoCentral and Liveless are Windows only, so their rows are too; each one's address and port rows show only while it's on.

Folder pickers use `SDL_ShowOpenFolderDialog` from band3's SDL copy, next to an editable path field (the dialog may not appear under gamescope).

## Devices (phase 2)

**Input system handed over (implemented, Open question 2).** In `OnFinalizePaths`, `input::PrepareInputSystem(window())` builds the real input system and shows it the window; `OnPreSetup`'s `input_factory` (`CreateInputSystem`) hands that system to the Runtime, or builds one if the launcher was not shown. The launcher and the game share one set of drivers. The system is built once and never destroyed or replaced (an SDL one can't be destroyed, see Open question 2, and one kept aside would queue every gamepad event for the rest of the run, since only polling drains its queue): band3's HID and MIDI drivers sit in slots inside it that restart their driver when `hid_instruments`, `midi_drums`, `midi_drums_device` or `midi_drums_notes` changes; `input::ApplyInputSettings()` does this, and the launcher calls it every frame. The MIDI driver restarts only once its settings have stayed the same for 300 ms (`RestartDebounce`, pure in `restart_debounce.h`), so clicking through ports opens the kit once rather than reconnecting it as a new controller each time; Play applies whatever is pending at once. `midi_drums_pulse_ms`, `midi_drums_min_velocity` and `midi_drums_combos` don't restart it: the running driver takes them under its lock (`UpdateMidiDrumsSettings`, `Kit::SetSettings`), since the Minimum velocity slider changes them every frame of a drag. After any restart the player assignment is recomputed from the SDK's last device list (`PlayerAssignment::Reassign`), since which devices are SDL's copies of a HID instrument depends on whether the HID driver runs, and the SDK calls `OnDevicesChanged` only when its device set changes.

**Input backend changes restart band3.** The system isn't switched live. An `input_backend` change made in the launcher applies when the player presses Play: the Input backend row shows "Applies when you press Play (band3 restarts)" while the setting names another backend than the running system's (`input::InputBackendChanged()`, which reads the setting as the SDK does: `BackendFor` in `input_backend.h`, XInput only for exactly `xinput` on Windows, SDL for anything else), the Starting frame reads "Restarting band3...", and `StartFromLauncher(restart)` calls `relaunch::StartAgain()` and quits this process through the launcher's quit path. The new run has this run's command line less `--launcher`, plus `--relaunch_wait_pid`, so it waits for this one to close and skips the launcher (`WasRelaunched`), and reads the band3.toml Play just saved. `RestartsForInput` (pure, `launcher_start.h`) decides: only when the change was saved (a failed save's "Play anyway" says the new backend applies only once saved, and plays on the old one), and never under `test_port`, since the harness follows this process's pid and port; there the game keeps the launcher's backend and `CreateInputSystem` logs that `input_backend` applies at the next start. If `StartAgain` fails the game starts on the old backend. `g_game_input` is set only when the Runtime takes the system. `PlayerDevices()` returns every device with its name, guid, kind (pad, synthetic, virtual, HID instrument, MIDI kit, SDL's copy of a HID instrument) and player, from a locked snapshot `PlayerAssignment` takes when the devices change. `ReadInputCaps(id)` and `ReadInputState(id)` read one device's capabilities or state by handing it alone, for that read, to the last player it doesn't feed (`ProbePlayer`, before the game only); every call makes the SDK enumerate its drivers again, so `DevicePanel` reads capabilities once per device list and only the state each frame: `GetStateForUI` records the device each player last had input from (`ActiveDeviceTracker::Observe`) and `ChooseDeviceForUser` prefers it among that player's devices, so a read on its own player would decide which of player 1's devices (keyboard or pad) the game reads first. The SDK's `last_used_user_` (`InputSystem::GetLastUsedUser`, the player whose device last pressed a button) does end up as the probe player after the launcher's reads; nothing in band3 reads it. On Play, `ReadyInputForGame()` makes every player read as disconnected once and drops queued keystrokes, so the game's first reads announce the players as at a start without the launcher.

**Fallback** (not needed; kept for the record): the launcher would have built standalone HID and MIDI drivers and destroyed them before `resume`, listed Xbox pads with `XInputGetState`, and computed players with the pure `AssignPlayers`.

**Test view.** Selecting a device shows it drawn simply: guitar frets, strum, whammy and tilt bars, solo and pickup; drum pads and cymbals flashing with velocity and both kicks; MIDI kits also list recent hits with note and velocity. Every source reaches the view as `Gamepad360` plus capabilities, so new pure decoders `DecodeGuitar` / `DecodeDrums(const Gamepad360&, caps)` beside the encoders in `instruments.cpp` turn it into `GuitarInputs` / `DrumInputs`. An Xbox instrument read through the SDL backend arrives laid out as a gamepad; the view labels it with `controller_type` and says so.

**Device lists** (`device_lists.h`, pure parts in `device_names.h`). `RecordingDeviceNames()` lists recording devices from band3's SDL directly, never through the `Capture` singleton, which must not be created before the game starts. `MidiInputPorts()` lists MIDI inputs from RtMidi, each with its full RtMidi name and the name to save. Both settings are case-insensitive substring matches, so a dropdown selects the first connected device the saved value matches (`FindSavedDevice`; `FindMidiPort` for MIDI, which also mirrors the driver's empty value: the first port that isn't "Midi Through"), and shows the saved value as "(not connected)" only when nothing matches. Mic slots are saved as full device names (`MicSlotValue`: a name with a comma saves its longest comma-free part) joined by commas (`JoinMicSlots`). WinMM names input port *i* `"<szPname> <i>"` (`MidiInWinMM::getPortName` in the vendored RtMidi.cpp, unless `RTMIDI_DO_NOT_ENSURE_UNIQUE_PORTNAMES`, which band3 doesn't define); `StripMidiPortIndex` removes exactly `" <i>"`, and the stripped name is still a substring of the full one, so the driver's `Matches` finds it. Listing ports doesn't open them, so it works while the driver has one open. `ListMonitors()` lists monitors in the `monitor` setting's order with their names and display modes (Open question 3).

**Mic level meter.** `launcher::MicMeter` (`mic_meter.h`): a separate `SDL_OpenAudioDeviceStream` (mono float, 16 kHz) per selected mic while the Audio tab is visible, with `SDL_InitSubSystem(SDL_INIT_AUDIO)` / `SDL_QuitSubSystem` paired exactly per meter. Levels (`MicLevel`, pure): the peak, held 0.5 s then falling with a 0.3 s time constant, and the RMS over a 0.3 s window, with `ToDecibels` for drawing. `StartFromLauncher` calls `CloseMicMeters()`, which closes every meter still open, before the game starts. The launcher holds `SDL_INIT_AUDIO` itself while the mic slots show, so listing the microphones every 2 s is cheap and sees hotplugs, and lets it go, with its meters, when they stop showing and on the first Starting frame. Device lists are looked for again at most every 2 s, and only while their tab shows (mics on Audio with `usb_mics` on, MIDI ports on Controllers with `midi_drums` on, monitors on Graphics). Mic slot 1 reads "System default" while every slot is empty (the capture then records the default device) and "None" otherwise; the resolution list is the chosen monitor's modes (the window's monitor for `monitor` = 0) by size, then the presets it lacks, matched by size whichever way the value is written.

**Gamepad navigation (implemented).** The launcher sets `NavEnableKeyboard | NavEnableGamepad` and, while a pad is connected, `HasGamepad`, and each frame feeds `ImGuiKey_Gamepad*` events (`GamepadNav`, `gamepad_nav.h`) from every connected device's state, OR'd together, read with `ReadInputState` (the probe player; never `GetStateForUI` on the device's own player, which would record a last-used device in the SDK and leak into the game). The reads stop on the Starting frame, and `Stop()` releases every key it holds and restores the flags (also when the launcher closes). The keyboard (it navigates by itself), the SDK's stand-in and SDL's copies of dongle instruments aren't read. A gamepad gives everything; an instrument gives only its menu buttons (`NavFromReading`, pure in `device_view_model.h`): its whammy, tilt and pickup are sticks and a trigger, a guitar's orange fret and a kit's kick are LB, and an RB2 kit's cymbal hit sets a d-pad marker. On a guitar or kit the frets or pads act as face buttons, so testing one is a mode of its own (`TestMode`, pure in `device_view_model.h`): picking a device's row (a click, or A with the focus on it) or its Test button tests it, and until Back is pressed on it, or Stop (the same button) clicked, it feeds navigation nothing (its Start doesn't play, its bumpers don't switch tabs) while other devices, the mouse and the keyboard navigate as before. Leaving the Controllers tab, the device going and the Starting frame end a test too, and the buttons the device holds as its test ends navigate only once let go and pressed again. The keyboard, which navigates by itself, is only shown, never tested. X isn't given to ImGui (holding it opens ImGui's window switcher); LB and RB aren't either: they switch tabs (`ImGuiTabItemFlags_SetSelected`), and Start plays unless a popup is open, both on the press, so a button held as the launcher opens does nothing until pressed again. The settings and footer children are `NavFlattened`, so focus moves between them as one page, and the footer describes the row the focus is in (`NavCursorWithin`), as it does the hovered one.

**Device list and test view (implemented).** `DevicePanel` (`instrument_view.h`) heads the Controllers tab: `PlayerDevices()` every 250 ms (the player assignment logs only when the devices change), one row per device with its name, kind (`DeviceKindLabel`: Controller or Xbox 360 guitar/drum kit/... by the subtype it last reported, guitar/drum kit "(USB dongle)", MIDI drum kit, keyboard and mouse, the SDK's "None" stand-in, a virtual instrument "(debug)") and player. SDL's copy of a dongle instrument is listed disabled, with a tooltip, rather than hidden. Until the player picks a row, the first device that plays (not the keyboard) is selected and shown live without being tested (it still navigates). The selected device is read every frame while the tab shows (its capabilities once per device list) and drawn by `ViewFor(caps)`: guitar (`DecodeGuitar`: frets, solo frets, strum, whammy and tilt bars, the pickup's five positions, Start/Back), drums (`DecodeDrums`: pads and cymbals lit by `FlashLevel`, 0.35 to 1 by velocity, fading over 0.35 s, with the last velocity on each; RB1 kits at full strength; both kicks), or a pad (face buttons, d-pad, sticks, triggers, bumpers). A gamepad-subtype device says what it plays as (`controller_type`, "Gamepads play as") and that an Xbox instrument through SDL shows as a controller; MIDI kits add their last six notes from `GetMidiDrumsStatus`.

## Code layout

New, under `src/Launcher/`:

- `launcher_dialog.{h,cpp}` — the screen, tabs and footer.
- `launcher_style.{h,cpp}` — colours, fonts and the page's scale.
- `launcher_platform.{h,cpp}` — Shift at startup and the display's refresh rate.
- `launcher_start.{h,cpp}`, `launcher_cvars.{h,cpp}` — when it shows (pure), and the model's view of the real cvars.
- `launcher_settings.{h,cpp}` — the setting table and the override model (pure).
- `config_file.{h,cpp}` — band3.toml read/merge/write (pure apart from file IO).
- `game_data_check.{h,cpp}` — the folder check (pure).
- `device_lists.{h,cpp}`, `device_names.{h,cpp}` — mic and MIDI port lists, monitor list, and their pure parts (phase 2).
- `mic_meter.{h,cpp}`, `mic_level.{h,cpp}` — the mic level meter and its level computation (phase 2).
- `instrument_view.{h,cpp}` — the Controllers device list and test-view drawing (phase 2).
- `gamepad_nav.{h,cpp}` — feeding ImGui gamepad events (phase 2).
- `device_view_model.{h,cpp}` — the pure parts of both: kind labels, view choice, hit flashes, devices' states as navigation (phase 2; `Input/device_kind.h` holds `DeviceKind` for it).

Changed: `band3_app.h` (`OnFinalizePaths`, `OnWindowCloseRequested`, the path rule, bind handling, input hand-over in `OnPreSetup`), `settings.{h,cpp}` (new cvars; split `Init`), `config.cpp` (rebuildable `AddSettingArgs`, `LegacyIniValue`, the anchor fallback), `paths.{h,cpp}`, `steam_deck.{h,cpp}` (shared preset lookup), `relaunch.{h,cpp}` (`WasRelaunched`, dropping `--launcher`, extra arguments if needed), `Input/instruments.{h,cpp}` (decoders), `Input/input_system.cpp` (prebuilt system, `PlayerDevices()`), `Input/midi_drums_driver`, `Audio/usb_mic_capture` (device lists), `docs/settings.md`, `README.md`.

## Testing

- **Unit tests (`band3_tests`, doctest):** the override model (each effective-default layer, typed comparison, reset, keys left alone, locked sources, the Deck toggle); the writer (merge, escaping of backslashes and quotes, typed values, invalid input → backup); the game data check (temp folders); the path rule (anchor, cache-follows-user, sources); `joypad_lag` round trip; phase 2: decoders (encode → decode round trip for every guitar and drum input, RB1 kits without velocity), MIDI name stripping, mic list joining and substring selection.
- **SDK round trip:** a file produced by the writer loads through `rex::cvar::LoadConfig` with every value applied — in `band3_tests` if it can link rexruntime, otherwise in the harness test below.
- **Harness:** existing `tests/game` scripts must still pass; they start with `test_port`, so the launcher is skipped. A new script writes a `band3.toml` (paths with backslash-free absolute and relative forms, `controller_type`, `lang`, `content_folders`) through the writer, launches with band3ctl, and checks through the test server that the values reached the game, adding a cvar query command to the server if it lacks one.
- **Launcher screenshots:** launch with `--launcher` via band3ctl and capture each tab with `band3ctl window shot`, for review.
- **Manual, with real hardware (the user, phase 2):** an Xbox guitar and kit, a PS3 dongle, a MIDI kit and two mics show up, test correctly, and play in the game after Play; the Steam Deck banner and presets on a Deck.

## Open questions, settled by the first task of each phase

1. **(Phase 1) When is `swap_post_effect` read?** Settled: once, when the guest GPU is set up. Disassembling `rexgpu-xenos.dll` shows its only reader is `GraphicsSystem::SetupGuestGpu`, through `CommandProcessor::SetDesiredSwapPostEffect` (that function's only call site), and `Runtime::Setup` runs it inside `resume`, after the launcher. So FXAA set in the launcher applies without a relaunch; a change made in F4 in game applies at the next start.
2. **(Phase 2) Can the input system be built before the Runtime and handed over?** Settled: yes, with one change to the plan: an SDL-backed system is never destroyed. From disassembling `rexruntime.dll`:
   - `CreateDefaultInputSystem` builds the SDL (or XInput) driver, MnK and NOP drivers with no window and sets them up; `SDLInputDriver::Setup` only checks SDL's version. `Runtime::Setup` calls the factory and then `IInputSystem::Setup`, which for `InputSystem` is `xor eax, eax; ret`, so the HID driver is never set up twice.
   - The SDL driver starts SDL's events and gamepad subsystems and adds an SDL event watch only in `OnWindowAvailable` (from `AttachWindow`), so before the Runtime pads appear only once the launcher calls `AttachWindow` itself. The watch is fed by the SDK window's own event loop, which runs while the launcher is up. `OnWindowAvailable` returns early when a window is already attached, and `Window::AddListener`/`AddInputListener` ignore a listener already added (MnK's `OnWindowAvailable` has no guard of its own), so the Runtime's second `AttachWindow` is harmless. `CallInUIThreadSynchronous` runs inline on the UI thread.
   - `~SDLInputDriver` doesn't remove its window listener or the SDL event watch (`SDL_AddEventWatch(..., this)` has no matching removal anywhere in the DLL; only `OnClosing` removes the listener and quits the subsystems), so destroying an SDL driver that has seen the window leaves dangling pointers. Hence the slots for band3's drivers, and a system that is never destroyed. A kept-aside SDL system (an earlier version parked one to switch backends live) isn't free either: its driver queues every joystick and gamepad event until polled, so backend changes restart band3 instead (Devices, above). XInput systems (XInput driver, MnK, whose destructor detaches it, NOP) are destroyed and rebuilt freely.
   - `InputSystem::GetStateForUI` marks a player connected and tells the kernel (`XN_SYS_INPUTDEVICESCHANGED`) only when `kernel_state()` exists, so the launcher's reads would hide the first connection from the game; `ReadyInputForGame` clears them first. `GetCapabilities` doesn't touch connection state.
   - Checked with band3ctl (a temporary build that showed the launcher under the harness and pressed Play by itself): the virtual instrument enumerated and its presses read through `ReadInputDevice` in the launcher; twelve setting changes (`hid_instruments`, `midi_drums`, `midi_drums_device`, `input_backend` sdl and xinput back and forth) restarted the drivers and switched systems without a crash (backend switching has since been replaced by a restart, Devices above); after Play the log shows "Controller disconnected from slot 0" and then, on the game's thread, "New controller connected to slot 0", and `boot.b3t` passed on a fresh profile with both the SDL and the XInput backend. A USB mic meter opened in the launcher and closed at Play, and the game's `usb_mics` capture then opened the same mic and fed it.
3. **(Phase 2) Does the SDK's `monitor` index follow `SDL_GetDisplays` order** in band3's SDL copy, so the monitor dropdown can show names? Settled: it follows the order of the SDK's own SDL copy, and band3 reproduces that order with Win32 rather than its SDL copy. The SDK's window is SDL's (`WindowSDL`); `WindowSDL::CenterOnConfiguredDisplay` takes `SDL_GetDisplays()`, and for `monitor` = N >= 1 moves the window to `SDL_WINDOWPOS_CENTERED_DISPLAY(displays[N - 1])` (warning "monitor cvar is {} but only {} display(s) present; using default" past the end); 0 leaves it where it is. The SDK's SDL is 3.4.14, whose `WIN_AddDisplays` (`SDL_windowsmodes.c`) calls `EnumDisplayMonitors` twice, adding the primary monitor first and then the others in enumeration order, skipping any whose current mode can't be read; its names come from `QueryDisplayConfig`'s `monitorFriendlyDeviceName`, else `EnumDisplayDevices`' `DeviceString`. `ListMonitors()` does the same through Win32 (`SdlDisplayOrder` is the pure ordering), with the modes from `EnumDisplaySettings`. band3's own SDL copy isn't used: its video subsystem isn't started, and starting it beside the SDK's (both register SDL's window class for the exe's instance on Windows) isn't known to be safe. When monitors change, SDL enumerates again the same way (moving known displays to their new places), so the two orders agree once both have looked since the change. Off Windows `ListMonitors()` returns nothing and the launcher keeps numbering monitors.

## Out of scope

Audio output device selection, pinning devices to players or instruments, a gameplay fps cap (each is engine work that adds a row to the launcher when it lands); opening the launcher from inside the game; Shift-to-open on Linux; changing F4's save behaviour.

Found while investigating, to fix separately: `menu_shortcut_dialog.cpp` checks `SDL_WasInit(SDL_INIT_GAMEPAD)` in band3's SDL copy, which never initialises gamepads, so the controller menu shortcut may never fire; `band3_app.h`'s comment says the SDK's audio queue default is 64, but it is 8.
