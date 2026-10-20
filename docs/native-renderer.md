# Native renderer and render checks

band3's own renderer draws the game's picture, by default on Windows with no emulated Xbox
360 GPU at all. This page covers its settings, what still differs from the game's picture,
and the tools that check it. [Native renderer internals](dev/native-renderer-internals.md)
has how it works and the measurements behind its defaults.

## Choosing the renderer

`renderer` (Settings > Graphics, on the launcher or in game) picks what draws the game's
picture:

| `renderer` | In Settings | |
|---|---|---|
| `native` | Native | band3's own renderer alone, at the window's size, under the SDK's overlays. There's no emulated GPU: band3's sync-only GPU answers what the game waits on ([below](#renderer-native-no-emulated-gpu)). The default on Windows, and on a Steam Deck (its [presets](settings.md#steam-deck)) |
| `emulated` | Emulated | the emulated Xbox 360 GPU (the SDK's xenos plugin) alone. The default elsewhere: on Linux the native renderer has run on a Steam Deck (Vulkan, RADV), not yet on other drivers |
| `both` | Both (debug) | the two side by side, the native picture shown; **F8** (`bind_renderer`) switches to the emulated GPU's picture and back. For comparing them (the checks below); it costs the GPU both |

- A change to or from `native` applies at the next start (the launcher's Play restarts band3
  for it); between `emulated` and `both` it applies at once.
- F8 only switches the picture under `both`; under the others it logs why not.
- A build that can present with neither Direct3D 12 nor Vulkan runs `native` as `emulated`.
- The log names each run's renderer (`renderer native: ...`, `renderer emulated: ...`,
  `renderer both (debug): ...`).

### Which settings apply

| Group (Settings > Display and Graphics, on the launcher and in game) | `native` | `emulated` | `both` |
|---|---|---|---|
| Display: monitor, window mode, resolution, aspect (`present_letterbox`), frame rate cap (`frame_cap`) | yes | yes | yes |
| Native renderer (Band3 → Graphics → Native): `native_fill_window`, `native_view_msaa`, `native_anisotropic`, `native_max_height`, `native_post_effects` | yes | no | yes |
| Emulated GPU, the plugin's own (Every SDK setting: GPU): `resolution_scale`, `swap_post_effect` (FXAA), `anisotropic_override`, `vsync` (shown with the frame cap off, which turns it off otherwise) | no | yes | yes (the emulated picture) |
| Emulated GPU, band3's (Band3 → Graphics → Emulated): `emulated_gpu_while_native` | no | no | yes |
| Game (Band3 → Graphics): `rnd_sync`, `background_fps`, `disable_hair_shader`, `disable_approximate_lights` | yes | yes | yes |
| `compress_character_textures` | no: ignored ([below](#renderer-native-no-emulated-gpu)) | yes | yes |

The launcher and the in-game settings show only the rows for the chosen renderer. The emulated
GPU's own settings belong to its plugin, which a `native` run doesn't load, so after switching
to `emulated` or `both` they appear once band3 restarts.

## Presenting

- **Zero-copy.** On Windows frames reach the window without leaving the GPU. Elsewhere, or with
  `native_view_backend` cpu, each frame is read back and uploaded. The log says which
  (`native present: zero-copy` or `uploading each frame`).
- **Phase lock** (`native_phase_lock`). With `frame_cap` at the display's rate (`display`, or a
  number matching it), the cap's beat is moved so each frame reaches the window a set time
  (`native_phase_lock_slack`: 65% of a refresh at 100 Hz or more, 50% below) before the refresh
  that shows it, rather than wherever the cap's clock happens to fall against the display's.
- **Pacing** (`native_present_pacing`). Each frame is handed to the window a steady delay after
  it's drawn, never more than a frame, so motion steps evenly. It's skipped while the phase lock
  steers on a display of 100 Hz or more (frames already arrive ahead of their refresh, and
  holding them only adds lag), when the game runs at 1.5 times the display's refresh rate or
  more, with `frame_cap` `auto`, or on a variable refresh display, where it would only add lag.
  So it applies at 60 Hz, to a cap the lock can't steer, such as 100 on a 120 Hz display, and
  with `native_phase_lock` off. At 60 Hz on an integrated GPU (Radeon 680M, 1920x1200) the lock
  without it showed 83% of frames distinct, against 97% with it, for 7 ms less latency.
- **Stalls.** If the GPU hasn't finished a frame in 2 s, the window keeps its last frame until
  it does.
- **Sound effects** (`audio_xapo_nonblocking`). Not the renderer's, but it sets the slowest game
  frames: RB3 asks every sound effect's voice each frame whether it's still playing, and for
  one that's starting or fading out, XAudio2's code has the game wait for the audio thread's
  next pass (up to 5 ms on a console, about 10 ms here, where audio frames are asked for two at
  a time). band3 answers with the newest result the audio thread set, without the wait.
- **Measuring.** The test harness's `present_stats` measures the window's pacing under either
  renderer.

Under `both`, `emulated_gpu_while_native` sets what the emulated GPU still does while the
native picture shows:

| Mode | |
|---|---|
| `skip_draws` (default) | skips the draws nobody sees; after F8 back to emulated its picture is the game's within a few frames |
| `full` | draws everything; without a frame cap its frames stall now and then |
| `swap_only` | test and performance mode: only what the game waits on. After F8 back, outfits and portraits RB3 drew once may be black until it draws them again |

RB3 composes a band's outfits once, in the main menu, so switching to the native picture after
that (`emulated` to `both` in game) shows them wrong until RB3 composes them again.
`tests/game/soak_native.b3t` soaks `both`: three songs, menu round trips and F8 both ways.

### What still differs

Every kind of screen the render checks go through matches the game's picture. The exceptions:

- **Lens flares aren't occluded:** one behind a character or the set still draws, where a 360
  dims or hides it ([why](dev/native-renderer-internals.md#lens-flares-and-occlusion-queries)).
- **Edges above 720p:** the world is drawn single-sampled at the window's size, as RB3 draws it
  at 720p, so its edges shimmer a little where the emulated GPU's stretched picture blurs them.
- **120 Hz:** at `video_mode_refresh_rate` 120 it shows about two in three of the game's frames
  in a song, where at 60 Hz it shows nearly all.
- **F8 back to emulated:** a texture pass RB3 stops drawing while the native renderer is on
  shows what the emulated GPU last drew in it.
- **Not checked:** the store and other online screens, RB3's error screens drawn over
  everything (`ModalDraw`), screens no script reaches, and Linux drivers other than the Steam
  Deck's.

### Settings

| Setting | |
|---|---|
| `renderer` (Band3 → Graphics) | `native` (the default on Windows), `emulated` (the default elsewhere) or `both` ([above](#choosing-the-renderer)) |
| `emulated_gpu_while_native` (Band3 → Graphics → Emulated) | with `renderer` both and the native picture shown: `skip_draws` (the default), `full` or `swap_only`, as above |
| `native_fill_window` (Band3 → Graphics → Native) | the game drawn at the window's shape rather than 16:9 with black bars ([below](#filling-the-window)); on by default |
| `native_max_height` (Band3 → Graphics → Native) | the most lines the native renderer draws: a taller window's picture is drawn this tall and scaled up, for 4K on a GPU that can't keep up. 0 (the default) draws at the window's size |
| `native_anisotropic` (Band3 → Graphics → Native) | anisotropic filtering, counted as `anisotropic_override` counts it: 0 off, 1 to 5 for 1x to 16x. -1 (the default) follows `anisotropic_override` where the emulated GPU runs, so the two pictures match, and keeps the game's own samplers under `native` |
| `native_view_msaa` (Band3 → Graphics → Native) | the samples a pixel of the overlay (the track, the HUD, menus drawn after the world), the only part RB3 multisamples: 2 (the default) as RB3 does, 4 smoother, 1 none |
| `native_post_effects` (Band3 → Graphics → Native; the launcher's Graphics → Performance) | the camera's post-processing effects: bloom (and glare), depth of field and camera motion blur. On by default; off leaves them out for a slower GPU, from the next frame. The composite still runs for the rest of RB3's picture (the venue's colour matrix, the spotlights' haze, soft particles, film grain, trails), and with none of those on a frame is resolved as it was drawn |

The rest are in Settings > Advanced > Native renderer, described in the
[settings reference](settings-reference.md#native-renderer). Most are there to compare or
measure; leave them at their defaults. Three to know about:

- `native_present_pipeline`: keep it off. It hangs AMD GPUs and grows video memory.
- `native_world_ahead`: on by itself on an integrated GPU (one that shares the system's
  memory, as a laptop's does) and on a Steam Deck (`native_world_ahead_auto`, on by default;
  on Linux only with `native_upload_pipeline` off). It draws each world frame's venue right
  after that frame, so the frame after it only post-processes it and draws the track and HUD.
  On a Radeon 680M at 1920x1200, 60 Hz, a song's post frames went from 11.1 to 6.2 ms
  (median), latency from 22.5 to 17.5 ms, and repeated frames fell by a sixth, the picture
  the same. Other GPUs leave it off: on a Radeon AI PRO R9700 at 120 Hz it made things worse.
  `native_world_ahead = true` in `band3.toml` (or in Settings > Advanced >
  Native renderer) turns it on for any GPU; `native_world_ahead_auto = false` leaves it to
  that alone. The log says which applied, for example
  `native_world_ahead: on by itself (integrated GPU: AMD Radeon(TM) Graphics)`. On Linux it
  does nothing unless `native_upload_pipeline` is off too, which the window's frames
  otherwise go through: on the Steam Deck at 1080p the two together cut latency from 25 to
  21 ms, but repeated frames went from 23 to 32 per 28 s.
- `native_slow_frame_ms`, `game_stall_log_ms`: log native frames slower than 12 ms and game
  frames slower than 100 ms, with where the time went.

## Filling the window

RB3 draws 16:9. With `native_fill_window` on (the default, Fill the window in Settings >
Display), the native renderer has it draw at the window's shape instead, with no black bars:

- A wider window (21:9, 32:9) shows more of the venue to the sides.
- A taller one (16:10, 4:3) shows more above and below, at 16:9's width.
- The HUD, the highways and the menus keep their size and shape in the middle 16:9; with
  more players the highways stay there too.
- Menu art drawn a little past 16:9's edges (the player bar, header banners, the results
  banner) is stretched out to the window's edge. The song list is the exception: it looks as
  it does at 16:9, with the background at the sides.

It changes what the game's cameras see, so nothing is missing at the new edges, and resizing
the window applies at once. Only the native renderer's picture fills the window: the emulated
GPU's stays 16:9, letterboxed or stretched by `present_letterbox`. The test harness's 1280x720
windows keep the render checks at 16:9.

All 32 venues were checked at 32:9. Not right yet: one orthographic camera draws to the
screen; it isn't widened, and what it draws hasn't been identified.

## Renderer native: no emulated GPU

With `renderer` native band3 doesn't load the emulated GPU at all. The native renderer is the
only picture, on the SDK's presenter with its overlays (F3, band3's menus and the rest).
band3's sync-only GPU (`src/Render/sync_gpu/`) answers what the game waits on, such as fences
and vertical blanks, and draws nothing.

- `emulated_gpu_while_native` doesn't apply, and F8 only logs that it needs `both`.
- `compress_character_textures` is ignored: with nothing drawing the outfits in guest memory, a
  created character's outfit and skin came out black.
- Art saved in the Art Library is drawn again on the CPU from the art maker's recorded passes,
  as nothing puts the art maker's picture in guest memory for the game to copy (it saved noise).
  The log says `Target readback:` with the time it took.
- A GPU hang ends band3, with the crash trace: there's no device recovery to fall back on.
- While the window is minimized nothing is drawn; restored, the next whole frame shows a frame
  or two later. Under `both` it draws on while minimized.
- With the frame cap on (the default) the cap paces the game; off, the vertical blank paces it
  at `video_mode_refresh_rate`, unless `native_vblank_free_running` is on.
- In the test harness, `screenshot` is the native renderer's picture and `screenshot emulated`
  an error; `capture` holds a frame at once (`<name>.png` at the window's size,
  `<name>.gpu.png` at 1280x720, `emulated` `none`). `native_view stats` has
  `emulated_gpu.present` false, the sync-only GPU's numbers (`sync`) and the minimized pauses
  (`paused`, `paused_ms`, `paused_captures`).
- The log's `sync gpu:` lines show the game's GPU traffic, with a warning when the game has
  waited on the GPU for 2 s.
- `python tools/perf_sample.py --pid <pid> --seconds 20` samples a run's CPU, GPU use and video
  memory once a second, per thread; its docstring has the options.

## Render checks

The native view (F9, experimental) draws the game's frames from a capture of what RB3 drew,
on the GPU or on a reference CPU rasterizer: its shading, textures, texture passes (outfits,
the crowd, shadow maps), post-processing and the display's gamma ramp. Render checks set its
picture against the game's. Its settings are in Settings > Advanced > Native
renderer, described in the [settings reference](settings-reference.md#native-renderer).

Launch render checks with:

```
python tools/band3ctl.py launch --fresh -- --renderer=emulated --native_view_record_targets=true --test_random_seed=21 --async_shader_compilation=false
python tools/band3ctl.py run tests/game/boot.b3t
python tools/band3ctl.py run tests/game/render_song.b3t
```

| Flag | Why |
|---|---|
| `--renderer=emulated` | the emulated GPU's picture is the reference. `both` works too, but guest memory's copies of the texture passes are stale there (no `--rt-guest`); for `native`, see [below](#render-checks-without-the-emulated-gpu) |
| `--native_view_record_targets=true` | RB3 composes a band's outfits once, in the main menu, so they must be recorded from launch |
| `--test_random_seed=21` | the same band and shot categories on every launch and machine (0, the default, is off). Poses and camera angles still vary a little. With 21, Futurama's Fry plays guitar in the render songs' 25 s capture |
| `--async_shader_compilation=false` | the emulated GPU skips draws whose pipeline isn't compiled yet, which can leave black outfits for the session |
| `--readback_resolve=full` (optional) | guest memory holds right copies of the texture passes, for replay's `--rt-guest` and `--dump-tex`. Leave it off for `render_song_evenodd.b3t` |

The scripts:

| Script | Captures |
|---|---|
| `render_song.b3t` | four points through a song, even/odd rendering off (every frame draws everything) |
| `render_song_evenodd.b3t` | the same with even/odd rendering on, as the game ships; `capture <name> composed` fails unless the capture is a post frame with the world frame's world in front of its overlay |
| `render_song_live.b3t` | the live view's cost with `native_view on` (native renderer off) |
| `render_screens_boot.b3t`, `_menus`, `_song` | run in that order from a fresh launch (instead of `boot.b3t`): `screen-<kind>` on each kind of screen RB3 shows, from the boot logos to the results |
| `render_screens_more.b3t` | after `boot.b3t`: the rest that work offline (character creator, goals, Play a Show, calibration, drum trainers and lessons) |
| `render_multiplayer.b3t` | a song with guitar and drums (`screen-mp2-<kind>`) |
| `render_multiplayer4.b3t` | four parts, the mics' test tone singing through Rock Band 3 Deluxe's All Instruments Mode (`screen-mp4-<kind>`; its header has the launch) |

`capture`'s reply:

| Field | |
|---|---|
| `draws` | the draws the capture kept |
| `skipped_pass` | draws left out for their draw mode: the velocity buffer's (kept apart, as the motion blur's objects) and other passes the native view doesn't draw |
| `skipped_shadow` | shadow-map or shadow-caster draws outside their own pass; 0 expected |
| `passes`, `passes_carried` | the texture passes in the capture, and those carried in from earlier frames (a band's outfits: there only with `native_view_record_targets` on from launch) |
| `rt_sampled` | the render target versions the draws sample |
| `rt_filtered` | of those, the ones whose pass drew nothing the capture keeps (the velocity buffer, or draws with no material or geometry) |
| `rt_missing` | the others no pass in the capture made; 0 means none it could have had is missing |
| `rt_fallback` | `native_view_rt_fallback` |
| `proc_cmds` | what the frame drew: 7 everything; with even/odd rendering 1 the world, 2 post-processing; -1 unknown |
| `composed`, `world_frame`, `game_frame` | the capture has the world of `world_frame` in front of the overlay of its own `game_frame` (a post frame, which shows the world frame before it) |
| `held_fallback` | no such post frame came in 30 frames, so the capture took the last |
| `emulated` | `full`: the emulated GPU drew the screenshot's frame (and the one before it) whole, as it does for `capture` under `skip_draws` and `swap_only` too; `stale` if it couldn't; `none` with `renderer` native |
| `emulated_passes_dropped` | passes RB3 draws once whose draws `swap_only` skipped since the game started: over 0, the screenshot may show black outfits or portraits |
| `gpu`, `gpu_ms`, `gpu_passes`, `gpu_rt_missing` | the GPU's `<name>.gpu.png` at the screenshot's size, its time, the texture passes it drew, and its draws that sampled a render target nothing had drawn (drawn transparent black). `gpu_error` instead when there's no GPU device or `native_view_backend` is `cpu` |
| `gpu_presented` | with the native picture shown at another size than the screenshot's, the GPU's `<name>.gpu.presented.png` at the size it draws the window at (replay's `--scale` checks it) |

With `native_view_texture_filtering` on, `<name>.gpu.nearest.png` is the same capture drawn
with it off, and with `native_view_instancing` on, `<name>.gpu.uninstanced.png` is it drawn
a draw at a time, and with `native_view_hoist_passes` on, `<name>.gpu.unhoisted.png` is it
with its texture passes where the game draws them; both should be the same picture.
`<name>.gpu.alpha.png` and `<name>.gpu.depth.png` are the GPU's scene target
before the overlay: its alpha (the bloom weight RB3's shaders write) and its depth, as grey.

### Replay and grading

`out/native_view_replay.exe <name>.cap out.png [options]` draws a capture on the CPU. Its
header (`tools/native_view_replay/replay.cpp`) has the build command and every option. It
prints `post:`, `check:` and `gamma:` lines, and with `--compare` or `--diff` a `metrics:` line.
F9's window has the same switches for the live view.

| Replay option | |
|---|---|
| `--compare <name>.png [--image <name>.gpu.png]` | the game's screenshot and the CPU's drawing side by side, at the screenshot's size; with `--image`, that PNG (the GPU's) instead of the CPU's |
| `--diff <name>.gpu.png` | the CPU's drawing against that PNG: the GPU checked against the CPU |
| `--scale <f>` | draws as the native renderer draws a window f times 720 lines tall: `--scale 1.5 --diff <name>.gpu.presented.png` for a 1080-line window |
| `--list` | the passes and every draw: mesh, material, where it lands, what its shader was given |

- `tools/parity.py` grades captures against the game. Copy `<name>.cap`, `<name>.png` and
  `<name>.gpu.png` from `screenshots/` into `out/parity` and run it. Each capture gets `cpu`,
  `gpu` and `gpu-cpu` rows, graded Tier A (parity) or Tier B (acceptable); `--baseline <file>`
  exits 1 when a mean got worse.
- `tools/pairs.py` measures F8 pairs: the native picture and, after F8, the emulated one. With
  the game launched `--renderer=both`, `window offscreen` at `window size 1280x720`, on a still
  moment, run `python tools/pairs.py out/pairs --take title --port <port>`. A pair passes at a
  mean of 3 or less with no 4x4 cell over 20.

Each tool's docstring has its tiers, thresholds and options.

### Render checks without the emulated GPU

With `renderer` native there's no emulated picture to check against, so each render script
runs twice with the same launch line apart from the renderer: R, the reference, on the
emulated GPU, and N without it (the four-part script adds `--usb_mics=true
--usb_mic_test_tone=220`):

```
python tools/band3ctl.py launch --fresh -- --renderer=emulated --native_view_record_targets=true --test_random_seed=21 --async_shader_compilation=false
python tools/band3ctl.py run tests/game/render_screens_boot.b3t | tee out/n7/ab/menus/R/run.log
...
python tools/band3ctl.py launch --fresh -- --renderer=native --native_view_record_targets=true --test_random_seed=21 --async_shader_compilation=false
python tools/band3ctl.py run tests/game/render_screens_boot.b3t | tee out/n7/ab/menus/N/run.log
```

Copy each run's `screenshots/` into its directory (`out/n7/ab/<set>/R` and `.../N`).

- In R, `<name>.png` is the emulated GPU's picture and `<name>.gpu.png` the native renderer's
  drawing of the capture.
- In N, `<name>.png` is the native renderer's own picture at the window's size and
  `<name>.gpu.png` its drawing at 1280x720: the one to set against R's `<name>.png`.
- `<name>-again.png`, taken a command after each capture, shows in R what moved by itself.

The two runs are two moments of the same game, so they're checked four ways:

- `tools/capdiff.py N R`: each capture holds what R's does (passes, draws, render targets,
  post-processing, replies); exits 1 on a difference that matters.
- `tools/parity.py --set N`: N against itself, `gpu-cpu` at 0.5 or less and `cpu` at a mean
  of 1 or less.
- `tools/pairs.py --cross N R`: N's `<name>.gpu.png` against R's `<name>.png`, leaving out what
  moved by itself; still screens pass at Tier A, moving ones at Tier B or better. Exits 1 if
  any doesn't.
- `tools/parity.py --set R`: R as every render check.
