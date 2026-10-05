# Native renderer and render checks

band3's own renderer for the game's frames, which draws the game's picture by default on
Windows (the emulated Xbox 360 GPU is a switch away), and the tools that check its picture
against the game's.

## Switching renderers

`renderer` (Band3 → Graphics, or the launcher's Graphics tab) picks what draws the game's
picture: `native`, band3's own renderer (the native view's, below) drawing each frame the
game sends at the window's size, under the SDK's overlays, or `emulated`, the emulated Xbox
360 GPU. `native` is the default on Windows. Elsewhere `emulated` stays the default: the
native renderer builds on Linux, but hasn't been run there yet. **F8** (`bind_renderer`)
switches between them at once, without a restart; the emulated GPU keeps running either
way, so its picture is always one key (or one setting) away. The picture keeps the game's
16:9 with black bars, or stretches when the SDK's `present_letterbox` is off. On Windows the frames go to the window without
leaving the GPU; where that can't be done (other platforms, `native_view_backend` cpu, or
`native_present_zero_copy` off) each frame is read back and uploaded instead. The log says
which (`native present: zero-copy`, or `native present: uploading each frame (<why>)`). On
Microsoft's software rasterizer (WARP, the Basic Render Driver Windows uses without a GPU
driver) the native renderer draws on the CPU instead: band3's GPU drawing would share the
emulated GPU's Direct3D 12 device there, which crashes (`native view gpu: not started`).

Turning native on makes the native renderer's GPU pipelines first, on the UI thread (about
100 ms, once a session), so no frame waits for one; the log says `native view gpu: <n>
pipelines ... in <ms>`, and names any pipeline a frame still had to wait for after that
(`pipeline made after warm-up: <key> (<ms>)`). Each frame is drawn as soon as the game
presents it, and reaches the window a steady delay after the game presented it, about as
long as the slowest of the last few frames took to draw (never more than a frame), so the
window gets one new frame a refresh: frames take as long to draw as what they have, and
with even/odd rendering one published at once came 8 ms after the one before and the next
25 ms after it, so a paint could find two new frames and never show the first
(`native_present_pacing`, below). With `native_present_pipeline` on (off by default until
it has been checked in game), the native renderer on the zero-copy path doesn't wait for
the GPU after sending it a frame: it waits only to hand the frame to the window, so it
records the next frame while the GPU draws the one before (at most that one frame in flight
besides the one being recorded), and a frame costs the longer of its CPU and GPU time
rather than both, to keep up at 120 Hz; off, it waits for each frame once sent, as before.
Either way, if the GPU hasn't finished a frame in 2 s, it
sends no more and the window keeps its last frame until the GPU does (`native renderer:
the GPU hasn't finished a frame in <n> ms; holding the last frame`, then `... drawing
again`). After F8 back to native the window shows only frames the
game presented since, from the first whose capture is the whole picture, never one left
from before the switch. With RB3's even/odd rendering (on, as
the game ships), a frame that draws the world but doesn't post-process it shows what the
game's does: the last post-processed picture (the world frame before it, with its
spotlights' beams, smoke and bloom) under its own track and HUD. The test harness's `present_stats`
measures the window's pacing under either renderer.

While `renderer` is native, the passes RB3 draws into textures are recorded all the time,
as `native_view_record_targets` does (below), since the native renderer draws outfits and
the like from them: a little game-thread time while characters load (about 170 ms from boot
to a song). Once `renderer` has been native, they stay recorded for the rest of the session,
under the emulated GPU too, so F8 back to native still has the outfits. RB3 composes a
band's outfits once, in the main menu, so switching to native later (F8) after they were
composed without recording shows them wrong until RB3 composes them again. On Windows the
native renderer is on from launch by default; elsewhere, to play on it, set it at launch
(`--renderer=native`, or in the settings before the main menu).

While the native renderer draws the window, the emulated GPU skips the game's draws nobody
sees (`emulated_gpu_while_native`, `skip_draws` by default): the meshes, instanced meshes and
quads of every frame the native renderer has whole. It still clears, resolves and swaps, and
still draws the passes RB3 draws into textures once or now and then (outfits, portraits;
the first two of any pass), and the lens flares' occlusion tests, so after F8 back to emulated
its picture is the game's within a few frames (two, and with even/odd rendering a world
frame drawn whole and the post frame after it, a frame later on screen): the native
renderer keeps drawing the window until it is (the log says `native present: off, the emulated GPU's picture shows (after <ms>)`).
The title screen's clouds are the exception found so far: they take 5 to 20 whole frames to
come back, so the emulated GPU's picture of the title lacks them for a moment after F8 back.
`capture` under `skip_draws` (or `swap_only`) has the emulated GPU draw 30 whole frames
before it holds one, so its game screenshot has them. With nothing seen to draw, the
emulated GPU's command processor does about a third of the work per frame, and the game runs
faster uncapped than under `emulated`. `full` draws everything, as before. `swap_only`, a test and performance mode, leaves the emulated
GPU only what the game waits on (fences, the occlusion queries' results, vertical blanks and
swaps): it also skips the clears, every resolve, the flares' occlusion-test quads (their
queries still give the same result, the emulated GPU's fixed sample count) and the passes
RB3 draws into textures once. After F8 back to emulated its picture may show black outfits
and portraits, and other pictures RB3 drew once, until RB3 draws them again; the log says
how many such passes were skipped, `native_view stats` counts them (`passes_dropped`) and
`capture` reports them (`emulated_passes_dropped`). With `compress_character_textures` on,
outfits composed under `swap_only` are read back before anything was drawn into them, so
both renderers may show them black (the log warns once). `tests/game/soak_native.b3t` soaks
the native renderer: three songs, menu round trips and F8 both ways in each song.

Every kind of screen the render checks below go through matches the game's picture, as
they measure it. Where the native renderer still differs from the emulated GPU, or hasn't
been checked:

- The lens flares are drawn without occlusion, as the emulated GPU draws them (it never
  occludes RB3's flare queries), so the picture matches the emulated one, not a 360's.
- The world is drawn single-sampled at the window's size, as RB3 draws it at 720p, so above
  720p its edges shimmer a little where the emulated GPU's 720p picture, stretched to the
  window, blurs them.
- With `emulated_gpu_while_native` `full` and no frame cap, the native renderer's frames
  stall now and then (both GPUs' work on one 3D engine); `skip_draws`, the default, doesn't.
- Not checked: the store and other online screens, RB3's error screens drawn over
  everything (`ModalDraw`), and screens no script reaches.
- A texture pass RB3 draws every frame but stops drawing while the native renderer is on
  shows, after F8 back to emulated, what the emulated GPU last drew in it, until RB3 draws
  it again.
- At a 120 Hz refresh rate (`video_mode_refresh_rate`) it doesn't show every one of the
  game's frames yet: about two in three in a song, where at 60 Hz it shows nearly all.
- It has only run on Windows. Linux keeps `emulated` as the default until it has been run
  there (Vulkan, and each frame uploaded rather than shown in place).

| Setting | |
|---|---|
| `renderer` (Band3 → Graphics) | `native` (the default on Windows) or `emulated` (the default elsewhere) |
| `emulated_gpu_while_native` (Band3 → Graphics) | with `renderer` native, `skip_draws` (the default) leaves the game's draws out of the emulated GPU's work as above; `full` has it draw everything; `swap_only` leaves it only what the game waits on, so F8 back may show black outfits and portraits for a while (above) |
| `emulated_gpu` (Band3 → Graphics) | experimental: `on` (the default) runs the emulated GPU beside the native renderer, as above; `off` runs without it ([below](#running-without-the-emulated-gpu-emulated_gpu-off)). Applies at the next start (the launcher restarts band3 for it) |
| `native_present_request_paint` (Band3 → Debug) | with `emulated_gpu` off, on (the default) asks the window to paint each time the native renderer has a frame for it; off leaves the window to paint when something else asks, to compare |
| `native_query_sample_count` (Band3 → Debug) | with `emulated_gpu` off, the samples every occlusion query reports drawn (the lens flares' visibility tests): 1000 (the default), what the emulated GPU's `query_occlusion_fake_sample_count` gives; -1 leaves them unanswered |
| `native_max_height` (Band3 → Graphics) | the most lines the native renderer draws: a taller window's picture is drawn this tall and scaled up to fill it, for 4K on a GPU that can't keep up at full size. 0 (the default) draws at the window's size |
| `native_view_msaa` (Band3 → Graphics) | the samples a pixel the native renderer and the native view draw the overlay with (the track, the HUD, menus drawn after the world), averaged at its edges: 2 (the default) as RB3 does, 4 smoother than the game, 1 none. RB3 multisamples only those: the world, its post-processing and every texture pass are single-sampled, in the game and here. Where the GPU can't draw 2 samples it draws 4 (or 4 → 2, else 1; the log says so) |
| `native_present_zero_copy` (Band3 → Debug) | on (the default) shows the GPU's frames where they are; off reads each back and uploads it, to compare |
| `native_present_pacing` (Band3 → Debug) | on (the default) publishes each frame to the window a steady delay after the game presented it, as above; off publishes each as soon as it's drawn, sooner on average but unevenly, to compare |
| `native_present_pipeline` (Band3 → Debug) | on records the next frame while the GPU draws the one before, on the zero-copy path, as above; off (the default, until it has been checked in game) waits for the GPU after each frame. `set` changes it at once. Its frames are submitted without waiting, and that alone (with the worker waiting for each at once) had the Direct3D 12 debug layer report the SDK's command lists going wrong (a barrier out of step, a list executed still open) and AMD GPUs hang, so leave it off until that's understood |
| `native_view_target_scale` (Band3 → Debug) | on (the default) draws the passes that are pictures of the screen (the spotlights' haze and the soft particles' smoke, made at 640x360 and 320x180 for the game's 1280x720) in proportion to the picture: 1.5 times at 1080p, 3 times at 4K. Off keeps the game's sizes, to compare |
| `native_view_shadow_scale` (Band3 → Debug) | the characters' self-shadow maps at this many times the game's 512x512 (1, the default, to 4): sharper shadow edges, and less of the game's own shadow acne, so further from the game's picture |
| `native_slow_frame_ms` (Band3 → Debug) | logs a line (`native renderer: slow frame ...`) for each frame the native renderer takes longer than this many milliseconds to draw, its GPU wait included (12, the default; 0 off), two a second at most: what kind of frame it was (as `by_kind` below), where its time went (the world passes before it, planning, filling the upload buffer, recording, submitting, waiting for the GPU, letting go), what it drew and sent (meshes into the pool and the arena, textures, bones, in MB), the pipelines, buffers and textures it made, what it let go of after, what capturing it cost the game's thread, and the captures skipped before it |
| `game_stall_log_ms` (Band3 → Debug) | logs a warning (`game stall: ...`) for each of the game's frames longer than this many milliseconds (100, the default; 0 off), one every two seconds at most: the frame split at DxRnd::Present's hook (the game's own part, its Present, capture, the frame cap's wait), and over the part of it a watcher saw (from half the threshold on) the game thread's and the emulated GPU's command processor's CPU time, the process's file I/O and page faults, what the native renderer's worker was doing, and samples of those two threads' and the worker's stacks every 50 ms (module+RVA, as the crash trace's; `src/stall_watch.h`) |

The test harness's `native_view stats` has `by_kind` too: the live view's frames by what
they drew under even/odd rendering (`frame_compose.h`'s `FrameKind`): `world` (the game drew
the world; the native renderer shows the kept post buffer and draws the world's texture
passes and the overlay), `post` (the world frame's world composed in, drawn and
post-processed), `between` (neither, at a background rate below half the game's) and `full`
(everything, or a frame that doesn't say). Each has its frames drawn (`rendered`), the
captures skipped while one of them was being drawn (`skipped_busy`, so the slow kind is the
one charged), `ms` and `wait_ms` as the totals', and per frame drawn: the worker's parts
(`parts_ms_per_frame`: `pre`, `plan`, `upload`, `record`, `submit`, `wait`, `evict`), what it
drew, sent, made and let go of (`per_frame`: `draws`, `world_draws`, `passes`, `pool_meshes`,
`mesh_bytes`, `textures_sent`, `texture_bytes`, `evicted_meshes` and the rest, and what the
GPU kept after it: `resident_meshes`, `resident_textures`, `resident_rts`,
`texture_array_mb`, `arena_mb`), what capturing it cost the game's thread (`capture`:
`ms_per_frame` by hook, and `per_frame` counts, `game_ms` the game's frame), and the most
the GPU kept after one of them (`peak`).

Under even/odd rendering the world's draws are drawn by the post frames alone, one in every
world period, so the native renderer keeps geometry and textures a frame drew for the
period and a little more (`gpu_view.h`'s residency) before letting them go; with a shorter
keep each post frame sent the whole world again (30 to 50 MB in arena_04), which took it
past a frame at 120 Hz. What's drawn once, a movie's frames say, goes as many frames
later. Without native_present_pipeline, `wait` (and `wait_ms`)
start when the frame is submitted, so they include `submit`.

Every other pass RB3 draws into a texture (outfits, the crowd's impostors, NgLight's
projected shadow, heads' normal maps) is drawn at the game's size at any window size.

## Running without the emulated GPU (emulated_gpu off)

Experimental, and Windows (Direct3D 12) only for now. With `emulated_gpu` off (Band3 →
Graphics, the launcher's Graphics tab, or `--emulated_gpu=off`; it applies at the next
start) band3 doesn't load the emulated GPU at all. The game still sends its GPU commands
and waits on what the GPU does with them, so band3's sync-only GPU
(`src/Render/sync_gpu/`) reads them as the emulated GPU would and does only what the
game waits on: the fences, the swap's interrupt, the vertical blanks, the occlusion
queries' results (`native_query_sample_count`), the read pointer and the display gamma
ramp. It draws nothing; the native renderer is the only picture, on the SDK's own
presenter with its overlays (F3, F4 and the rest) as before.

- `renderer` is native for the run whatever it says (the log says so if it wasn't), and
  `emulated_gpu_while_native` is ignored: the game's draws never reach a GPU but the
  native renderer's.
- F8 (`bind_renderer`) does nothing but log `the emulated GPU is off this run
  (emulated_gpu off); restart with it on to switch`; so does changing `renderer`.
- The test harness's `screenshot` is the native renderer's picture and `screenshot
  emulated` an error; `capture` holds a frame at once (no whole frames first), its
  `<name>.png` is the native renderer's picture at the window's size, `<name>.gpu.png` is
  drawn at 1280x720, and its reply's `emulated` is `none`. `native_view stats` has
  `emulated_gpu.present` false and the sync-only GPU's numbers (`sync`).
- The log has `sync gpu:` lines: the ring, the read pointer write-back and the interrupt
  callback the game set up, a summary of what it sent every 10 s (packets by opcode,
  waits, interrupts, swaps, vblanks, fences, and anything unknown), and a watchdog's
  warning when the game has waited on the GPU for 2 s.
- A GPU hang ends band3 (with the crash trace), as there's no emulated GPU's device
  recovery to fall back on.
- With the frame cap on (the default) the sync-only GPU's vertical blank runs every
  millisecond and the cap paces the game, as the emulated GPU's vsync off did; with it
  off, the vertical blank paces the game at `video_mode_refresh_rate`.

## Render checks

The native view (F9, experimental) draws the game's frames itself, from a capture of
what RB3 drew: on the GPU, or on a reference CPU rasterizer. It draws RB3's shading
(point, box and projected lights, characters' self-shadows, normal and detail maps),
its textures as the game's samplers read them (filtered, between mip levels, clamped
or wrapped), the passes RB3 draws into textures (outfit composites, the crowd's
impostors, shadow maps, NgLight's projected shadow, heads' normal maps, blurs), its
post-processing (the camera's motion blur, depth of field, bloom or glare, the spotlights'
beams and haze, soft particles such as stage smoke, the colour matrix) and, last, the
display's gamma ramp. The motion blur is the camera's, and the characters' own where
RB3 draws them into its velocity buffer with their motion, as it does in songs.
Render checks set its picture against the game's.

| Setting (Band3 → Debug) | |
|---|---|
| `native_view_backend` | `gpu` (the default) or `cpu`, the reference rasterizer. The GPU falls back to the CPU when it can't start |
| `native_view_record_targets` | records the passes RB3 draws into textures all the time, even while the native view is off. Off by default, as it costs a little game-thread time while characters load; on regardless once `renderer` has been native in the session. Render checks need it from launch: RB3 composes a band's outfits once, in the main menu |
| `native_view_rt_fallback` | `guest` (the default) also keeps what guest memory holds of a texture RB3 draws, sampled where no recorded pass made it (right only with `--readback_resolve=full`); `none` keeps only which texture and version it is. While `renderer` is native it's always `none`: what guest memory holds is stale then, as the emulated GPU skips the draws that make it |
| `native_view_normal_maps` | on (the default) shades normal and detail maps, live and in `capture`'s `.gpu.png`; off shades those materials with the vertex normal, to compare. Captures from before the capture kept the meshes' tangents have none either way |
| `native_view_capture_profile` | off by default. The test harness's `native_view stats` has `capture`: what capturing cost the game's thread per game frame since `on` or `off`, in all and by kind of hook (`ms_per_frame`: `mesh`, `multimesh`, `particles`, `rect`, `pass`, `present`, `other`), per draw (`us_per_draw`), with counts per frame (`per_frame`) and the caches' `sizes`. On, it also times each step inside the hooks (`steps_ms_per_frame`: `bones`, `shade_read`, `tex_decode`, `publish` and the rest, `rest` what none names), at a clock read per step |
| `native_view_texture_filtering` | on (the default) samples textures as each draw's fetch constants say: bilinear or point, the mip level (or two, blended) by how far and at what angle the surface is, anisotropy, and wrapping, mirroring or clamping per axis, from the mip chains in guest memory (and a render target's own, made after its pass). As the game's own picture under band3 is drawn, the SDK's `anisotropic_override` (4:1 by default) applies to the samplers it would apply to there. Off reads every texture's nearest texel at full size, as before, to compare; captures from before the capture kept the samplers and mips are drawn so either way |

Launch render checks with:

```
python tools/band3ctl.py launch --fresh -- --renderer=emulated --native_view_record_targets=true --test_random_seed=21 --async_shader_compilation=false
python tools/band3ctl.py run tests/game/boot.b3t
python tools/band3ctl.py run tests/game/render_song.b3t
```

`--renderer=emulated` keeps the emulated GPU drawing the window, as the reference picture
is its. The checks hold under the native renderer too (the default on Windows), since
`capture` has the emulated GPU draw whole frames before it holds one (`emulated`: `full`
in its reply), but guest memory's copies of the texture passes are stale there (no
`--rt-guest`), and `render_song_live.b3t` measures the live view with the native renderer
off.

The native view doesn't need `--readback_resolve=full`. With it, guest memory holds
right copies of what RB3 draws into textures (garbage otherwise), to compare against:
replay's `--rt-guest` and `--dump-tex`.

Also launch with `--async_shader_compilation=false`. With the emulated GPU's
default asynchronous shader compilation, a draw whose pipeline isn't compiled yet is
skipped, and RB3 composes each band member's outfits only once, in the main menu: the
first composites it makes can come out empty and stay black for the session in the
game's own picture (not the native view's), differently on each run.

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
`--readback_resolve=full`. `render_song_live.b3t` measures the live view's cost with
`native_view on`.

`render_screens_boot.b3t`, `render_screens_menus.b3t` and `render_screens_song.b3t`, run
in that order from a fresh launch (instead of `boot.b3t`), capture `screen-<kind>` on each
kind of screen RB3 shows: the boot logos, the intro movie, the title, the first-run
prompts, the band, closet and main menus, practice, the music library, a song's loading
vignette, the song, its pause menu, a music-video venue, a controller's disconnect
dialog and the results. `render_screens_more.b3t` (after `boot.b3t`) captures the ones
that work offline and those don't reach: the character creator and its face maker, the
career's goals, Play a Show and its setlists, the calibration screens, and on drums the
trainers and a drum lesson. `render_multiplayer.b3t` plays a song with two players
(guitar and drums, `screen-mp2-<kind>`), and `render_multiplayer4.b3t` with four parts
(guitar, drums, keys, and the USB mics' test tone singing through Rock Band 3 Deluxe's
All Instruments Mode, `screen-mp4-<kind>`; its header has the launch).

Besides the back buffer's draws and the texture passes, a capture keeps the
characters' shadow-map passes and NgLight's shadow casters, a texture pass whose camera
cleared it but that drew nothing (the spotlights' depth volume with no cone in view), and
the display's gamma ramp the game was shown through (the screenshot has it). `capture`'s
reply:

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
| `emulated` | `full`: the emulated GPU drew the screenshot's frame (and the one before it) whole. With `renderer` native and `emulated_gpu_while_native` `skip_draws` or `swap_only`, `capture` has it draw whole frames for a moment first and holds one of those, so this is `full` too; `stale` if it couldn't; `none` with `emulated_gpu` off, when the screenshot is the native renderer's |
| `emulated_passes_dropped` | the passes RB3 draws into textures once whose draws the emulated GPU skipped under `swap_only` since the game started: more than 0, the screenshot may show black outfits or portraits RB3 hasn't drawn again since |
| `gpu`, `gpu_ms`, `gpu_passes`, `gpu_rt_missing` | the GPU's `<name>.gpu.png` at the screenshot's size, its time, the texture passes it drew, and its draws that sampled a render target nothing had drawn (drawn transparent black). `gpu_error` instead when there's no GPU device or `native_view_backend` is `cpu` |
| `gpu_presented` | with `renderer` native at another size than the screenshot's, the GPU's `<name>.gpu.presented.png` at the size it draws the window at, as it draws it there (replay's `--scale` checks it) |

With `native_view_texture_filtering` on, `<name>.gpu.nearest.png` is the GPU's drawing
of the same capture with it off: the same frame without the game's samplers.

Beside `<name>.gpu.png`, `<name>.gpu.alpha.png` and `<name>.gpu.depth.png` are the
GPU's scene target where the world's draws left it, before the overlay: its alpha (the
bloom weight RB3's shaders write) and its depth, as grey.

`out/native_view_replay.exe <name>.cap out.png [options]` draws a capture on the CPU
(`tools/native_view_replay/replay.cpp`; its header has the build command and every
option). It prints a `post:` line (what post-processing was set to do), a `check:` line
(whether the constants RB3's composite drew with agree) and a `gamma:` line (the ramp).
`--compare` and `--diff` follow the mean difference with a `metrics:` line.

| Replay option | |
|---|---|
| `--compare <name>.png [--image <name>.gpu.png]` | the game's screenshot and the CPU's drawing side by side, at the screenshot's size; with `--image`, that PNG (the GPU's) instead of the CPU's |
| `--diff <name>.gpu.png` | the CPU's drawing against that PNG: the GPU checked against the CPU |
| `--crop x,y,w,h` | measures that rectangle alone (a HUD element, say), in the compared PNG's pixels |
| `--size WxH` | the size to draw at (640x360) when nothing sets it |
| `--scale <f>` | draws at f times 1280x720 (or at `--size`, or the PNG's size with `--compare` and `--diff`) with the haze's and smoke's passes f times theirs, as the native renderer draws a window f times 720 lines tall: `--scale 1.5 --diff <name>.gpu.presented.png` for a 1080-line window |
| `--shadow-scale <f>` | the characters' shadow maps at f times their size (`native_view_shadow_scale`) |
| `--msaa 1\|2\|4` | the overlay's samples a pixel (`native_view_msaa`): 2 by default, as the game's; 1 as the renderers drew before |
| `--list` | the passes and every draw: mesh, material, where it lands, what its shader was given, its `cull` (2 `D3DCULL_CW`, 6 `D3DCULL_CCW`); and `shadow:` lines checking each self-shadowed draw's shadow map |
| `--shade <draw>` | everything one draw's shader was given |
| `--pick X,Y` | the draw that last wrote that pixel (at `--size`), its colour and shade |
| `--dump-rt <DxTex hex>[:<version>]` | what that render target holds after its pass (`out.alpha.png` its alpha) |
| `--dump-tex <draw>[:<map>][@<level>]` | a draw's diffuse texture as captured, or one of its shade's maps (`--shade`'s names: `normal`, `projected`...); with `@<level>`, that mip level of it. `--shade` prints each one's sampler and how many levels the capture kept |
| `--dump-alpha <png>`, `--dump-depth <png>` | the scene target's alpha or depth, as the GPU's `.gpu.alpha.png` and `.gpu.depth.png` |
| `--view alpha\|depth` | draws that view instead of the picture (with `--diff` against those PNGs) |
| `--dump-bloom <png>` | bloom's first level as the composite read it, after glare's pass |
| `--rt-none`, `--rt-guest` | never use guest memory's pixels for a render target; or draw no texture passes and use guest memory's alone |
| `--no-post`, `--post-only xfm\|dof\|bloom\|spot\|soft\|noise\|velocity` | no post-processing; or only the colour matrix, depth of field, bloom (and glare), the spotlights' beams, the soft particles, the film grain or the camera's motion blur |
| `--no-grain`, `--no-velocity`, `--no-velocity-objects` | without the film grain; without the camera's motion blur; with the camera's alone, not the characters' own motion |
| `--no-gamma`, `--gamma-from <other.cap>` | no gamma ramp; or another capture's |
| `--no-shadow` | characters without their self-shadows |
| `--no-normal` | every material with its vertex normal, no normal or detail map |
| `--no-default-mat` | without the passes RB3 draws with no material of their own, which it draws with its default one (white, prelit) |
| `--nearest` | every texture's nearest texel at full size, not the game's samplers (`native_view_texture_filtering` off) |
| `--no-cull` | both sides of every triangle |
| `--legacy-light`, `--no-light` | the placeholder lighting from before RB3's shading; or every material unlit |

F9's window has switches like these for the live view (lighting, culling,
post-processing, the gamma ramp and others).

`tools/parity.py` measures a set of captures against the game: copy `<name>.cap`,
`<name>.png` and `<name>.gpu.png` from `screenshots/` into `out/parity`, then

```
python tools/parity.py                                  # the table for out/parity
python tools/parity.py --set out/parity_other           # another set
python tools/parity.py --baseline out/parity/baseline.json
python tools/parity.py --replay-args=--no-gamma         # every replay run with these options too
```

Each capture gets a `cpu` row (the CPU's drawing against the game), a `gpu` row (the
GPU's `.gpu.png` against the game) and a `gpu-cpu` row (the two against each other,
ok at a mean of 0.5 or less), with both HUD crops' means (the score box, the track).
The `cpu` and `gpu` rows are graded: Tier A (parity) is a mean of 8 or less, p50 4 or
less, at most 5% of pixels off by more than 32, no 4x4 grid cell's mean over 20, both
HUD crops 5 or less and a signed mean within 3; Tier B (acceptable) a mean of 12 or
less and at most 12% off by more than 32. `--baseline` writes the numbers if the file
is missing, and otherwise exits 1 if any capture's `cpu` or `gpu` mean got worse by
more than 0.5 (`--write-baseline` saves over it). The `gpu` rows change only when the
capture is taken again, not when replay is rebuilt, and numbers from different runs of
the game aren't comparable: a baseline compares re-renders of the same captures.

`tools/pairs.py` measures F8 pairs: the native renderer's picture and, after F8, the
emulated GPU's, a moment apart, without `capture`'s whole frames first, so what the
window shows under each renderer as a player switches. With the game launched
`--renderer=native` and its window `window offscreen` at `window size 1280x720` (the
emulated GPU's size), on a still moment (a menu, a paused song):

```
python tools/pairs.py out/pairs --take title --port <port>   # take one and measure it
python tools/pairs.py out/pairs --still 2                    # every pair in the set
```

Each pair gets parity.py's columns and tier, and passes at a mean of 3 or less with no
4x4 cell over 20. `--take` takes the native picture again after F8 back, and the pair is
graded by the closer of the two; `--still <mean>` leaves out what moved between them by
itself, and `<set>/exclude.txt` rectangles by hand. `pairs_sheet.png` shows each pair
and its difference.
