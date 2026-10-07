# Native renderer and render checks

band3's own renderer for the game's frames, which draws the game's picture by default on
Windows with no emulated Xbox 360 GPU at all, and the tools that check its picture against
the game's.

## Choosing the renderer

`renderer` (the Graphics tab, on the launcher or in the in-game settings) picks what draws the game's
picture:

| `renderer` | Launcher | |
|---|---|---|
| `native` | Native | band3's own renderer alone, at the window's size, under the SDK's overlays. There's no emulated GPU: band3's sync-only GPU answers what the game waits on ([below](#renderer-native-no-emulated-gpu)). The default on Windows |
| `emulated` | Emulated | the emulated Xbox 360 GPU (the SDK's xenos plugin) alone. The default elsewhere: the native renderer builds on Linux, but hasn't been run there yet |
| `both` | Native + emulated (debug) | the two side by side, the native picture shown; **F8** (`bind_renderer`) switches to the emulated GPU's picture and back. For comparing them (the checks below); it costs the GPU both |

Whether the emulated GPU runs is decided as band3 starts, so a change to or from `native`
applies at the next start (the launcher's Play restarts band3 for it, and the in-game settings' row
says **Applies at the next start**); between `emulated`
and `both` it applies at once. F8 only switches the picture under `both`, logs why not
under the others, and doesn't change the setting. A build that can present with neither
Direct3D 12 nor Vulkan runs `native` as `emulated`. The log says which renderer each run
has (`renderer native: ...`, `renderer emulated: ...`, `renderer both (debug): ...`).

### Which settings apply

| Group (the Graphics tab, on the launcher and in the in-game settings) | `native` | `emulated` | `both` |
|---|---|---|---|
| Display: monitor, window mode, resolution, aspect (`present_letterbox`), frame rate cap (`frame_cap`) | yes | yes | yes |
| Native renderer (Band3 → Graphics → Native): `native_fill_window`, `native_view_msaa`, `native_anisotropic`, `native_max_height` | yes | no | yes |
| Emulated GPU, the plugin's own (All settings: GPU): `resolution_scale`, `swap_post_effect` (FXAA), `anisotropic_override`, `vsync` (shown with the frame cap off, which turns it off otherwise) | no | yes | yes (the emulated picture) |
| Emulated GPU, band3's (Band3 → Graphics → Emulated): `emulated_gpu_while_native` | no | no | yes |
| Game (Band3 → Graphics): `rnd_sync`, `background_fps`, `disable_hair_shader`, `disable_approximate_lights` | yes | yes | yes |
| `compress_character_textures` | no: ignored ([below](#renderer-native-no-emulated-gpu)) | yes | yes |

The launcher and the in-game settings (Escape > Settings) show only the rows for the chosen renderer. The
emulated GPU's own settings belong to its plugin, which a `native` run doesn't load, so
choosing `emulated` or `both` there shows a note in their place until band3 restarts (the
launcher's Play does it). The SDK's own menu (the in-game settings' **All settings...**) lists every setting
by category, each description saying which renderers it applies to; the plugin's appear
only in a run with the emulated GPU.

## Presenting

With `native_fill_window` on (the default), the game draws at the window's shape
([below](#filling-the-window)); off, the picture keeps the game's 16:9 with black bars, or
stretches with `present_letterbox` off. On Windows frames reach the window without leaving
the GPU; elsewhere, with `native_view_backend` cpu, or with `native_present_zero_copy` off,
each is read back and uploaded (the log says which: `native present: zero-copy` or
`native present: uploading each frame (<why>)`). On Microsoft's software rasterizer (WARP)
it draws on the CPU, since sharing the emulated GPU's Direct3D 12 device there crashes.

Its GPU pipelines are made as the native renderer starts (about 100 ms, once a session), so
no frame waits for one; the log names any made later (`pipeline made after warm-up`). Each
frame is drawn as soon as the game presents it and handed to the window a steady delay
later, about the slowest recent frame's drawing time and never more than a frame, so the
window gets one new frame a refresh (`native_present_pacing`). If the GPU hasn't finished a
frame in 2 s, the window keeps its last frame until it does (`native renderer: the GPU
hasn't finished a frame ...`). After F8 back to native, the window shows only frames the game
presented since. With RB3's even/odd rendering (on, as the game ships), a frame that draws
the world without post-processing it shows the last post-processed picture under its own
track and HUD, as the game's does. The test harness's `present_stats` measures the window's
pacing under either renderer.

While the native picture shows, the passes RB3 draws into textures (outfits and the like)
are recorded all the time, as `native_view_record_targets` does: about 170 ms of game-thread
time from boot to a song. Once the native picture has shown they stay recorded for the
session. RB3 composes a band's outfits once, in the main menu, so switching to the native
picture after that (`emulated` to `both` in game) shows them wrong until RB3 composes them
again.

Under `both`, while the native picture shows, the emulated GPU skips the draws nobody sees
(`emulated_gpu_while_native` `skip_draws`, the default). It still clears, resolves and
swaps, and draws the texture passes RB3 draws once or now and then and the lens flares'
occlusion tests, so after F8 back to emulated its picture is the game's within a few frames;
the native renderer keeps the window until then (`native present: off, ... (after <ms>)`).
The title screen's clouds take 5 to 20 frames to come back. `capture` has the emulated GPU
draw 30 whole frames before it holds one, so its screenshot is whole. `full` draws
everything. `swap_only`, a test and performance mode, leaves the emulated GPU only what the
game waits on (fences, occlusion query results, vertical blanks and swaps), so after F8 back
outfits, portraits and other pictures RB3 drew once may be black until it draws them again
(`native_view stats`' `passes_dropped`, `capture`'s `emulated_passes_dropped`); with
`compress_character_textures` on, outfits composed under `swap_only` may be black in both
pictures. `tests/game/soak_native.b3t` soaks `both`: three songs, menu round trips and F8
both ways in each.

Every kind of screen the render checks below go through matches the game's picture, as
they measure it. Where the native renderer still differs, or hasn't been checked:

- The lens flares are drawn without occlusion, as the emulated GPU draws them, so the
  picture matches the emulated one, not a 360's.
- The world is drawn single-sampled at the window's size, as RB3 draws it at 720p, so above
  720p its edges shimmer a little where the emulated GPU's stretched 720p picture blurs them.
- With `emulated_gpu_while_native` `full` and no frame cap, its frames stall now and then
  (both GPUs' work on one 3D engine); `skip_draws` doesn't.
- Not checked: the store and other online screens, RB3's error screens drawn over
  everything (`ModalDraw`), and screens no script reaches.
- A texture pass RB3 draws every frame but stops drawing while the native renderer is on
  shows, after F8 back to emulated, what the emulated GPU last drew in it.
- At 120 Hz (`video_mode_refresh_rate`) it shows about two in three of the game's frames in
  a song, where at 60 Hz it shows nearly all.
- It has only run on Windows, on Direct3D 12. The Vulkan path for Linux is written but
  untested, so Linux defaults to `emulated` and the launcher offers Native on Windows only.

| Setting | |
|---|---|
| `renderer` (Band3 → Graphics) | `native` (the default on Windows), `emulated` (the default elsewhere) or `both` ([above](#choosing-the-renderer)) |
| `emulated_gpu_while_native` (Band3 → Graphics → Emulated) | with `renderer` both and the native picture shown: `skip_draws` (the default), `full` or `swap_only`, as above |
| `native_fill_window` (Band3 → Graphics → Native) | the game drawn at the window's shape rather than 16:9 with black bars ([below](#filling-the-window)); on by default |
| `native_max_height` (Band3 → Graphics → Native) | the most lines the native renderer draws: a taller window's picture is drawn this tall and scaled up, for 4K on a GPU that can't keep up. 0 (the default) draws at the window's size |
| `native_anisotropic` (Band3 → Graphics → Native) | anisotropic filtering, counted as `anisotropic_override` counts it: 0 off, 1 to 5 for 1x to 16x. -1 (the default) follows `anisotropic_override` where the emulated GPU runs, so the two pictures match, and keeps the game's own samplers under `native` |
| `native_view_msaa` (Band3 → Graphics → Native) | the samples a pixel of the overlay (the track, the HUD, menus drawn after the world), the only part RB3 multisamples: 2 (the default) as RB3 does, 4 smoother, 1 none |

The rest are on the in-game settings' Advanced tab (`emulated_gpu` under Band3 → Advanced → Retired in All
settings), each described there (`src/settings.cpp`):

- `emulated_gpu`: retired, still read at startup into `renderer` (`off` is `native`; `on`
  is `both`, or `emulated` if `renderer` said so), then cleared at the next save.
- `native_present_zero_copy`, `native_present_pacing`, `native_present_request_paint`:
  turn off the presenting above, to compare.
- `native_present_pipeline`: records the next frame while the GPU draws the one before.
  Keep it off: it hangs AMD GPUs and grows video memory.
- `native_query_sample_count`, `native_query_log`, `native_sync_short_wait_us`,
  `native_vblank_free_running`: the sync-only GPU's query answers and pacing.
- `native_view_target_scale`: the haze and smoke passes in proportion to the picture (on by
  default; every other texture pass is drawn at the game's size). `native_view_shadow_scale`:
  sharper self-shadows.
- `native_slow_frame_ms`, `game_stall_log_ms`: log native frames slower than 12 ms and game
  frames slower than 100 ms, with where the time went: for a native frame its planning's
  parts too (the render targets and texture arrays it made, the textures and meshes drawn for
  the first time) and the camera (`src/Render/camera_cut.h`: whether the frame is at a cut).

`native_view stats`' `by_kind` splits the live view's frames by what they drew under
even/odd rendering (`frame_compose.h`'s `FrameKind`): `world` (the game drew the world; the
native renderer shows the kept post buffer and draws the world's texture passes and the
overlay), `post` (the world frame's world composed in, drawn and post-processed), `between`
(neither, at a background rate below half the game's) and `full` (everything, or a frame
that doesn't say). Each kind has `rendered`, `skipped_busy` (captures skipped while one of
its frames was drawn), `ms` and `wait_ms`, and per frame drawn the worker's parts
(`parts_ms_per_frame`), what it drew, sent, made and let go of and what the GPU kept after
(`per_frame`), what capturing it cost the game's thread (`capture`), and the most the GPU
kept after one (`peak`). Since only post frames draw the world, the native renderer keeps
the geometry and textures a frame drew for a world period and a little more (`gpu_view.h`'s
residency) before letting them go. RB3 stops drawing a character while it's out of the shot,
so a camera cut back to it would make its render targets and send its textures and meshes
again (12 to 25 ms at 120 Hz). Render targets keep their textures until they've gone 30 s
unused, and in a song (from its loading screen until its results are left) so do the meshes
and textures drawn in more than one world; outside a song, what the menus draw goes as
before, so their texture arrays empty while a song loads (arrays never shrink). `by_kind`
also counts the frames whose planning took over 8 ms (`plan_spikes`, `plan_spikes_at_cut`),
and `camera` the cuts seen.

## Filling the window

RB3 draws 16:9. With `native_fill_window` on (the default, Fill the window on the Graphics
tab), the native renderer has it draw at the window's shape instead, with no black bars:

- A wider window (21:9, 32:9) shows more of the venue to the sides.
- A taller one (16:10, 4:3) shows more above and below, at 16:9's width.
- The HUD, the highways and the menus keep their size and shape in the middle 16:9; with
  more players the highways stay there too.

It changes what the game's cameras see, not the picture after: RB3 builds each camera's
projection from its vertical field of view and `Rnd::YRatio` (height over width, 9/16), so
band3 gives `YRatio` the window's height over width, and for a window taller than 16:9 widens
each perspective camera's vertical field of view while the projection is built
(`src/Hooks/aspect.cpp`, the numbers in `src/Hooks/aspect_model.h`). The game culls with the
same frustum, so nothing is missing at the new edges. A camera built for another shape is
rebuilt the next time it draws, so resizing the window, the setting and F8 apply at once (the
log says `fill window: the game's cameras at <w>x<h>`, or `at 16:9`).

Only the native renderer's picture fills the window. The emulated GPU draws the game's
1280x720, so while its picture shows (`renderer` emulated, or F8 under `both`) the game's
cameras stay 16:9, letterboxed or stretched by `present_letterbox` as before. The test
harness's windows are 1280x720, so the render checks see the game's own 16:9.

Much of RB3's menu art was drawn a little past 16:9's edges, for TVs that cut the picture's
edges off: the player bar along the bottom, the screens' header banners, the bars behind
submenu titles, the song list's rows, the results banner. In a wider or taller window that
art would stop short of the window's edge, so the native renderer moves the vertices of the
overlay's draws (menus and HUD, drawn after the world) that lie past the game's 16:9 out to
the window's edge: the part past 16:9 stretches, what's inside it stays as it is
(`RasterOptions::overlay_edge`, `mesh.hlsl`'s `StretchEdges`). It leaves the world's draws,
a camera's with a screen rect of its own (the tracks with more players) and the game's
full-screen quads alone, and in a song it works across only: the HUD under the track (the
player's name) runs past 16:9's bottom by design, and a taller window shows it whole.

The song list (`song_select_screen`, Play a Show's too) is the exception: its rows run off a
16:9 screen's left edge, and stretched out to a wider window's edge the selected row's tab
would become a long slab. There the draws lying between the "viewing all ... songs" bar and
the hint bar are cut where a 16:9 screen cuts them instead (`RasterOptions::overlay_cut`,
`aspect_model.h`'s `kSongListCut`), so the list looks as it does at 16:9 and the side shows
the background; the bars above and below it, and the player bar, still span the window.

All 32 venues were checked at 32:9 (`forced_venue`, intros and play): each fills the
window, with nothing missing at its edges. Not right yet: one orthographic camera draws to
the screen (the rest are perspective); it isn't widened, and what it draws hasn't been
identified.

## Renderer native: no emulated GPU

With `renderer` native band3 doesn't load the emulated GPU at all. On Windows it presents on
the SDK's Direct3D 12 device, which the native renderer draws on, zero-copy; on Linux it
presents through the SDK's Vulkan presenter, each frame uploaded (written, not yet run). The
game still sends its GPU commands and waits on what the GPU does with them, so band3's
sync-only GPU (`src/Render/sync_gpu/`) reads them and does only what the game waits on: the
fences, the swap's interrupt, the vertical blanks, the occlusion queries' results
(`native_query_sample_count`), the read pointer and the display gamma ramp. It draws
nothing; the native renderer is the only picture, on the SDK's presenter with its overlays
(F3, band3's menus and the rest).

- `emulated_gpu_while_native` doesn't apply, and F8 only logs that it needs `both`.
- `compress_character_textures` is ignored (logged once). Compressing reads the outfits
  back from guest memory, where with no emulated GPU nothing draws them: a created
  character's outfit and skin came out black.
- In the test harness, `screenshot` is the native renderer's picture and `screenshot
  emulated` an error; `capture` holds a frame at once, with `<name>.png` at the window's
  size, `<name>.gpu.png` at 1280x720, and `emulated` `none`. `native_view stats` has
  `emulated_gpu.present` false and the sync-only GPU's numbers (`sync`).
- The log's `sync gpu:` lines show the ring the game set up, a summary every 10 s (packets
  by opcode, waits, interrupts, swaps, vblanks, fences, anything unknown) and a warning when
  the game has waited on the GPU for 2 s. The summary sorts the waits that stalled by their
  packet's wait interval: `yield` (under 0x100: polls again at once, unless
  `native_sync_short_wait_us` is set), `sleep` (0x100 to 0xFFF: sleeps the interval / 0x100
  ms between polls) and `long_sleep` (0x1000 and up).
- A GPU hang ends band3 (with the crash trace): there's no emulated GPU's device recovery to
  fall back on.
- While the window is minimized the native renderer draws nothing (a harness `screenshot`
  still gets a frame). Restored, it shows the last frame drawn until the next whole one, a
  frame or two later. The log says when it pauses and resumes; `native_view stats` has
  `paused`, `paused_ms` and `paused_captures`. Under `both` it draws on while minimized.
- With the frame cap on (the default) the vertical blank runs every millisecond and the cap
  paces the game; with it off, the vertical blank paces the game at
  `video_mode_refresh_rate`, unless `native_vblank_free_running` is on.
- `python tools/perf_sample.py --pid <pid> --seconds 20` samples a run's CPU, GPU use and
  video memory from outside once a second, which sees either GPU's work, and each thread's
  CPU; its docstring has the options.

## Render checks

The native view (F9, experimental) draws the game's frames itself, from a capture of
what RB3 drew: on the GPU, or on a reference CPU rasterizer. It draws RB3's shading
(point, box and projected lights, characters' self-shadows, normal and detail maps),
its textures as the game's samplers read them, the passes RB3 draws into textures (outfit
composites, the crowd's impostors, shadow maps, NgLight's projected shadow, heads' normal
maps, blurs), its post-processing (motion blur, depth of field, bloom or glare, the
spotlights' beams and haze, soft particles such as stage smoke, the colour matrix) and,
last, the display's gamma ramp. Render checks set its picture against the game's.

Its settings are on the in-game settings' Advanced tab (Native renderer), each described there: `native_view_backend` (`gpu`,
the default, or `cpu`, the reference rasterizer, which the GPU falls back to when it can't
start); `native_view_record_targets`, which records the passes RB3 draws into textures even
while the native view is off (render checks need it from launch, as RB3 composes a band's
outfits once, in the main menu; always on once `renderer` has been native in the session);
and `native_view_rt_fallback`, `native_view_normal_maps`, `native_view_texture_filtering`
and `native_view_capture_profile`.

Launch render checks with:

```
python tools/band3ctl.py launch --fresh -- --renderer=emulated --native_view_record_targets=true --test_random_seed=21 --async_shader_compilation=false
python tools/band3ctl.py run tests/game/boot.b3t
python tools/band3ctl.py run tests/game/render_song.b3t
```

`--renderer=emulated` has the emulated GPU draw the window, as its picture is the reference.
The checks hold under `both` too (`capture` has the emulated GPU draw whole frames first),
but guest memory's copies of the texture passes are stale there (no `--rt-guest`). Under
`native` there's no emulated picture to check against: see
[below](#render-checks-without-the-emulated-gpu).

`--readback_resolve=full` isn't needed, but with it guest memory holds right copies of what
RB3 draws into textures, to compare against (replay's `--rt-guest` and `--dump-tex`).

`--async_shader_compilation=false` matters because the emulated GPU skips a draw whose
pipeline isn't compiled yet, and RB3 composes each band member's outfits only once: the
game's own picture can keep black outfits for the session, differently on each run.

`--test_random_seed=<n>` (0, the default, is off) seeds RB3's random numbers, so the same
seed gives the same band and shot categories on every launch and machine. Poses and camera
angles still vary a little, as animations and particles draw random numbers as the frame
timing gives. With seed 21, Futurama's Fry (a cel-shaded Deluxe character) plays guitar in
the render songs' 25 s capture.

`tests/game/render_song.b3t` plays a song with even/odd rendering off (every frame draws
everything) and captures four points through it. `render_song_evenodd.b3t` does the same
with it on, as the game ships: each capture is a post frame with the world frame's world in
front of its overlay, and `capture <name> composed` fails if it isn't; launch it without
`--readback_resolve=full`. `render_song_live.b3t` measures the live view's cost with
`native_view on` (with the native renderer off).

`render_screens_boot.b3t`, `render_screens_menus.b3t` and `render_screens_song.b3t`, run
in that order from a fresh launch (instead of `boot.b3t`), capture `screen-<kind>` on each
kind of screen RB3 shows: the boot logos, the intro movie, the title, the first-run
prompts, the band, closet and main menus, practice, the music library, a song's loading
vignette, the song, its pause menu, a music-video venue, a controller's disconnect
dialog and the results. `render_screens_more.b3t` (after `boot.b3t`) captures the rest
that work offline: the character creator and its face maker, the career's goals, Play a
Show and its setlists, the calibration screens, and on drums the trainers and a drum
lesson. `render_multiplayer.b3t` plays a song with two players (guitar and drums,
`screen-mp2-<kind>`), and `render_multiplayer4.b3t` with four parts (guitar, drums, keys,
and the USB mics' test tone singing through Rock Band 3 Deluxe's All Instruments Mode,
`screen-mp4-<kind>`; its header has the launch).

Besides the frame's draws and texture passes, a capture keeps the characters' shadow-map
passes, NgLight's shadow casters, texture passes cleared but drawn into by nothing, and the
display's gamma ramp. `capture`'s reply:

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
with it off. `<name>.gpu.alpha.png` and `<name>.gpu.depth.png` are the GPU's scene target
before the overlay: its alpha (the bloom weight RB3's shaders write) and its depth, as grey.

`out/native_view_replay.exe <name>.cap out.png [options]` draws a capture on the CPU
(`tools/native_view_replay/replay.cpp`; its header has the build command and every option,
for dumping textures and targets, finding the draw behind a pixel, or leaving out
post-processing, shadows, lighting and the like). It prints `post:`, `check:` and `gamma:`
lines (what post-processing was set to do, whether RB3's composite constants agree, the
ramp), and `--compare` and `--diff` a `metrics:` line. The main options:

| Replay option | |
|---|---|
| `--compare <name>.png [--image <name>.gpu.png]` | the game's screenshot and the CPU's drawing side by side, at the screenshot's size; with `--image`, that PNG (the GPU's) instead of the CPU's |
| `--diff <name>.gpu.png` | the CPU's drawing against that PNG: the GPU checked against the CPU |
| `--scale <f>` | draws as the native renderer draws a window f times 720 lines tall: `--scale 1.5 --diff <name>.gpu.presented.png` for a 1080-line window |
| `--list` | the passes and every draw: mesh, material, where it lands, what its shader was given |

F9's window has switches like replay's for the live view.

`tools/parity.py` grades a set of captures against the game: copy `<name>.cap`, `<name>.png`
and `<name>.gpu.png` from `screenshots/` into `out/parity` and run it. Each capture gets a
`cpu` row (the CPU's drawing against the game), a `gpu` row (the GPU's) and a `gpu-cpu` row
(the two against each other), graded Tier A (parity) or Tier B (acceptable);
`--baseline <file>` exits 1 when a mean got worse. Its docstring has the tiers and options.

`tools/pairs.py` measures F8 pairs: the native renderer's picture and, after F8, the
emulated GPU's, as a player switching sees them. With the game launched `--renderer=both`,
its window `window offscreen` at `window size 1280x720`, on a still moment (a menu, a
paused song), `python tools/pairs.py out/pairs --take title --port <port>` takes a pair
and measures it; a pair passes at a mean of 3 or less with no 4x4 cell over 20. Its
docstring has the rest.

### Render checks without the emulated GPU

With `renderer` native there's no emulated picture in the run, so the render scripts run
twice on the same launch line but for the renderer: R, the reference, on the emulated GPU,
and N without it (the four-part script's launches add `--usb_mics=true
--usb_mic_test_tone=220`):

```
python tools/band3ctl.py launch --fresh -- --renderer=emulated --native_view_record_targets=true --test_random_seed=21 --async_shader_compilation=false
python tools/band3ctl.py run tests/game/render_screens_boot.b3t | tee out/n7/ab/menus/R/run.log
...
python tools/band3ctl.py launch --fresh -- --renderer=native --native_view_record_targets=true --test_random_seed=21 --async_shader_compilation=false
python tools/band3ctl.py run tests/game/render_screens_boot.b3t | tee out/n7/ab/menus/N/run.log
```

then each run's `screenshots/` copied into its directory (`out/n7/ab/<set>/R` and
`.../N`); the run.log keeps each capture's reply. In R, `<name>.png` is the emulated GPU's
picture and `<name>.gpu.png` the native renderer's drawing of the capture. In N,
`<name>.png` is the native renderer's own picture at the window's size and
`<name>.gpu.png` its drawing of the capture at 1280x720: the one to set against R's
`<name>.png`. After each capture the scripts take `screenshot <name>-again` a command
later, which in R shows what moved by itself in that moment. The two runs are two moments
of the same game (the seed fixes the band and the shots, not every frame), so they're
checked four ways (each tool's docstring has its checks and thresholds):

- `tools/capdiff.py N R`: each capture holds what R's does (the texture passes, the draws,
  within 10% on a screen that moved by itself, the render targets, the post-processing and
  gamma ramp, the capture replies); exits 1 on a difference that matters.
- `tools/parity.py --set N`: N against itself, `gpu-cpu` at 0.5 or less and `cpu` (the
  replay of N's capture against N's own picture) at a mean of 1 or less.
- `tools/pairs.py --cross N R`: N's `<name>.gpu.png` against R's `<name>.png`, leaving out
  what moved by itself between R's `<name>.png` and `<name>-again.png`; a still screen
  passes at Tier A, a moving one at Tier B or better within 1 of what R's own drawing
  reaches. Exits 1 if any doesn't.
- `tools/parity.py --set R`: R as every render check.
