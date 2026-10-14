# Native renderer internals

How the [native renderer](../native-renderer.md) works and the measurements behind its
defaults, moved out of the user docs. The numbers are from when each change landed; the
commits that made them have the rest.

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
window gets one new frame a refresh (`native_present_pacing`). That's only while the game
runs under 1.5 times the display's refresh rate and `frame_cap` isn't `auto`: faster, each
paint shows the newest of two or more frames anyway, and a variable refresh display shows
each frame as it comes, so the delay would only add lag; there each frame is handed over as
soon as it's drawn (the log says which: `native present: pacing frames` or `not pacing
frames`). If the GPU hasn't finished a frame in 2 s, the window keeps its last frame until
it does (`native renderer: the GPU hasn't finished a frame ...`). After F8 back to native,
the window shows only frames the game presented since. With RB3's even/odd rendering (on, as the game ships), a frame that draws
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

## Lens flares and occlusion queries

The lens flares aren't occluded. Their visibility tests (occlusion queries) report the
pixels each test covers on screen, with either GPU (`src/Render/query_answers.h`), so a
flare in the open draws at a 360's strength (the GPUs' old constant 1000 left a 128x128
one at 6%), but one behind a character or the set still draws, where a 360 dims or hides
it. The native renderer draws a frame after the game has moved on, and may skip one, so
its depth can't answer by the next frame as a 360's GPU does. `native_query_sample_count`
0 or more brings the constant back; `native_query_log` logs the answers.

## Advanced settings

What each does, how it was measured and why it defaults as it does. The
[settings reference](../settings-reference.md#native-renderer-1) has the short version.

- `emulated_gpu`: retired, still read at startup into `renderer` (`off` is `native`; `on`
  is `both`, or `emulated` if `renderer` said so), then cleared at the next save.
- `native_present_zero_copy`, `native_present_pacing`, `native_present_request_paint`:
  turn off the presenting above, to compare.
- `native_present_pipeline`: records the next frame while the GPU draws the one before.
  Keep it off: it hangs AMD GPUs and grows video memory.
- `native_world_ahead` (experimental, off): with even/odd rendering, draws each world frame's
  venue right after that frame without waiting for it, so the post frame after it only
  post-processes it and draws its overlay; the world's GPU time moves into the world frame's
  half of the pair. On a 60 Hz laptop in a paused song post frames went from 14 to 8 ms and
  latency fell 4 ms, the picture the same, but the frames shown barely changed there (the
  missing ones were pacing's, not the renderer's); it's meant for 120 Hz, where a post frame
  over 8.3 ms costs the next. Where post frames drawn this way still run about a frame's
  time it backfires (one too slow skips the next world frame, so the world drawn ahead is
  wasted and the post frame after draws the whole world, too slow again), so it measures the
  frames skipped with it and without in 2 s windows, keeps the one that skips fewer, and
  tries the other every eighth window (`present_model.h`'s `AheadChooser`): in that case
  it then stays within noise of off, where drawing ahead regardless skipped 18% more frames.
  `native_view stats`' `by_kind` has `ahead` (the world frames' milliseconds drawing it),
  `ahead_gated` (world frames it chose not to), `ahead_used` (post frames that used it) and
  `ahead_fallback_passes` (world texture passes a post frame drew itself all the same).
- `native_query_sample_count`, `native_query_log`: the occlusion queries' answers (with
  either GPU). `native_sync_short_wait_us`, `native_vblank_free_running`: the sync-only
  GPU's pacing.
- `native_view_target_scale`: the haze and smoke passes in proportion to the picture (on by
  default; every other texture pass is drawn at the game's size). `native_view_shadow_scale`:
  sharper self-shadows.
- `native_slow_frame_ms`, `game_stall_log_ms`: log native frames slower than 12 ms and game
  frames slower than 100 ms, with where the time went: for a native frame its planning's
  parts too (the render targets and texture arrays it made, the textures and meshes drawn for
  the first time), its capture's cost to the game's thread and the camera
  (`src/Render/camera_cut.h`: whether the frame is at a cut). At first sight of a song's or a
  shot's textures and meshes the game's thread only copies their bytes (a few MB, 2 to 10 ms
  of a frame's capture), and the worker decodes them before the frame (`decode` in the line,
  `src/Render/deferred_decode.h`; with helper threads, `native_deferred_decode_threads` below),
  as the same decoder would have from the same bytes:
  decoding them where they were drawn cost the game's thread 30 to 70 ms on those frames.
  `record` says how much of it went on what SDL_gpu makes the first time a frame needs more
  than any before: getting command buffers (one for each part the frame is submitted in; a
  command list and allocator made when its pool has none), their first draws (where it takes
  their descriptor heaps), later draws over 0.05 ms (another pair once 2048 samplers are
  written, 16 a draw that rebinds) and pipeline binds and uniform pushes over 0.05 ms (a 32
  KB uniform buffer made; a draw pushes 2 KB), each with the slowest; `submit` its slowest
  submission. So that frames don't make those, the worker grows SDL_gpu's pools once, the
  first time it waits for a capture (`gpu_view.h`'s `Idle`, logged "SDL's pools
  warmed"): 16 command buffers and their fences, 128 uniform buffers (8 MB) and 24
  descriptor heap pairs (about 2 MB each), in 103 to 116 ms, about 30 MB more video memory
  than a song grew them to by itself. At 20th Century Boy's start (four runs each) the world
  passes before its first frame went from 37.6 to 38.9 ms to 25.7 to 27.3 (their recording
  from 16 to 4.5: 24 uniform buffers), its first 1402-draw frame from 56 to 65 to 37 to 60
  (14 ms of command buffers and 8 to 11 of uniform buffers gone; what's left varies with
  single submissions of 6 to 31 ms, SDL_gpu's own), and the four first-sight frames from 168
  to 180 ms in all to 135 to 159. A frame that drew its world first (`pre`: a refracting
  venue without a kept world to read) has those passes' own parts after the line's counts
  ("pre: plan ... (walk ...: the render targets and texture arrays made), upload, record,
  submit, waiting for the GPU, evict").
- `native_gpu_draw_profile` (off): with `native_gpu_timestamps`, a timestamp before each
  back-buffer world draw too (`gpu_timing_model.h`'s `MarkKeyed`), summed by mesh and material
  (`draw_profile.h`) and logged every 10 s: the groups by shader type and blend, then the 40 that
  took the most, named from guest memory. A draw's span runs to the next timestamp, so with the
  GPU overlapping draws it's roughly the time the draw added; the timestamps themselves cost
  about 2 ms of a 1080p post frame on the Steam Deck.
- `native_gpu_timestamps` (off): times each frame's parts on the GPU with timestamps written
  between them (`src/Render/gpu_timing_model.h`): its upload, the world, the texture passes
  (`pass_shadow`, `pass_spot` for the spotlights' targets, `pass_other` for the rest: outfits,
  the crowd...), the blurs in them, their mips, post-processing (`velocity`, `dof`, `bloom`,
  `composite`), the overlay, its multisampled start (`overlay_start`: the picture copied into
  every sample) and resolves, the gamma pass, and a world drawn
  ahead or before the frame. `idle` is the GPU waiting for the CPU to send the next command
  buffer (a frame goes to the GPU in parts, submitted after each texture pass with mips and,
  with `native_view_submit_points`, at the resolve) and
  isn't in the frame's total, its busy time. The slow-frame log ends with the split ("GPU 1.5 ms busy: world 0.5, ...").
  Direct3D 12 only: the timestamps go into SDL_gpu's own command list, reached through SDL
  3.4.14's private layout and checked first; where the checks fail, or on Vulkan, there are
  no times (one log line) and nothing else changes. About 60 timestamps a frame in a song,
  which cost about 0.1 to 0.2 ms of the worker's frame, in its wait for the GPU (at the edge
  of run-to-run noise; the frames shown and skipped don't change), so it's off unless
  measuring. At 1600x900 in arena_04 the GPU is busy about 0.7 ms on a world frame
  (mostly `pass_other`) and 1.5 ms on a post frame (`pass_other` and `world` half a
  millisecond each), with 0.5 to 0.8 ms idle around the mips (before `native_view_inline_mips`).
- `native_view_inline_mips` (on): makes the mips of the textures RB3 draws into (in a song the
  crowd's 256x512 target, four levels, several times a frame) at the end of the command buffer
  the frame submits there, by `src/Render/shaders/mips.hlsl`, a pass a level drawn as SDL_gpu's
  `SDL_GenerateMipmapsForGPUTexture` draws it on Direct3D 12 (the same triangle, sampler and
  calls). SDL's own mips take a sampler each, which would put the command buffer's sampler heap
  off the step `gpu_view.cpp`'s `kSamplerBatch` keeps it on (the AMD hangs), so off they go in
  a command buffer of their own between the frame's work so far and the rest: two command
  buffers each time where on takes one. The levels come out byte for byte as SDL's (random
  texels at even and odd sizes, and the crowd's target in render_song's captures); a level
  whose side halves to 0 is drawn by neither. Direct3D 12 only (elsewhere SDL's, as before).
  The frame is still submitted there, mips or not: the GPU draws the frame's first parts while
  the worker records the rest, and recording the frame whole and submitting it once (tried)
  had post frames wait 2.2 ms for the GPU rather than 1.2. In 20th Century Boy's 60 s slice at
  60 Hz, on against off: the GPU's `idle` 0.2 rather than 0.5 ms a world frame and 0.4 to 0.5
  rather than 0.7 to 0.8 a post frame; the worker's wait for the GPU on world frames 1.03 to
  1.09 ms p50 and 2.2 to 2.3 p95 rather than 1.2 and 2.6, its whole world frame 0.2 ms
  shorter and its post frame 0.2 to 0.3; post frames' wait the same.
- `native_view_submit_points` (1): where else a frame goes to the GPU in parts. With 0 only at
  the texture passes with mips (above); with 1 also at the resolve, once the world and its
  texture passes are recorded (after the last world draw's pass ends, before the resolve's
  copies), so the GPU draws them while the worker records post-processing, the overlay and the
  gamma pass, rather than waiting for all of it. Not for a world pass before the frame or the
  world drawn ahead (they end there and are submitted anyway), nor with nothing drawn since the
  last submission (a world frame whose last texture pass made mips). A submission costs the
  worker about 0.03 ms and the GPU a short wait for the next command buffer; the new command
  buffer starts with nothing bound and its own descriptor heaps, which `BeginPass` starts on
  `kSamplerBatch`'s step as at every pass. The picture is the same to the byte: 11 captures
  (render_song_evenodd's and render_screens_menus') drawn with 0 and with 1 had no pixel
  different. native_view stats' `by_kind` counts the command buffers (`per_frame`'s
  `submits`) and their submissions' time (`parts_ms_per_frame`'s `submit`).
  In 20th Century Boy's 60 s slice at 60 Hz (1280x720, arena_04), 1 against 0, three runs
  each: post frames' wait for the GPU 0.82 to 0.90 ms p50 rather than 1.10 to 1.22 (0.29 less
  on average) and 2.81 p95 rather than 3.06, their whole frame 3.27 ms p50 rather than 3.44;
  world frames' wait 0.96 rather than 1.04 ms p50; the GPU's busy time the same (1.71 against
  1.72 ms a post frame, p95 3.06 against 3.10) and its `idle` on post frames 0.35 rather than
  0.44 ms; 7.2 command buffers a post frame rather than 6.3, their submissions 0.25 ms in all
  rather than 0.23. The soak (`soak_native_only.b3t`) passed with 1 and with 0.
- `native_bc_textures` (on): keeps RB3's block-compressed textures (DXT1, DXT2/3, DXT4/5 and
  DXN, most of what it loads) compressed on the GPU, as BC1, BC2, BC3 and BC5, in texture
  arrays of their own. The worker's decode of a texture seen first (`decode` in the slow-frame
  line) then only untiles and byte-swaps its blocks (`src/Render/guest_formats.h`'s
  `UntileLevelBlocks`), and the GPU gets a quarter (DXT1: an eighth) of the RGBA bytes. At
  20th Century Boy's start the worker's first-sight frames went from 37, 47, 67 and 53 ms of
  decoding to 10, 14, 19 and 23 (what's left is the meshes and the movie planes, k_8), the
  textures they send from 21, 22, 85 and 42 MB to 4, 5, 17 and 8, and in its 60 s slice the
  texture arrays from 381 to 181 MB and the process's video memory from 783 to 531 MB. A
  texture's RGBA is decoded from its blocks only when something asks for it (the CPU
  rasterizer, a capture file: `deferred_decode.h`'s `EnsureRgba`), by the same decoder, to the
  same texels. The GPU's BC decoders round the 5- and 6-bit colours and the colours between
  them a step or two differently from that decoder (as the emulated GPU's do, which draws them
  as BC where it can), so the GPU's picture moves from the CPU's by a mean of 0.1 to 0.3 (`parity.py`'s
  `gpu-cpu`, 0.04 to 0.07 with it off). BC5 has no z and w, which RB3's fetch fills with y
  (a head's normal map is drawn into another texture as a diffuse texture, its alpha read):
  `mesh.hlsl`'s `MapTexel` puts them back. The device's support for the four formats is
  checked once (logged if missing: then every texture is RGBA, as with it off), and a
  texture whose fetch swizzles its components stays RGBA. Read as each texture is placed: off
  (to compare) places new textures as RGBA, decoding the blocks of any decoded while it was
  on. `native_view stats`' `capture` counts `deferred_bc_blocks` (textures kept as blocks),
  `deferred_bc_swizzled` (kept RGBA for their swizzle) and `deferred_bc_rgba` (kept as blocks,
  then decoded to RGBA).
- `native_r8_textures` (on): keeps RB3's one-channel textures (k_8) a byte a texel on the GPU,
  as R8, in texture arrays of their own, rather than RGBA8. They're the planes of its movies
  (Bink's Y and two chroma planes, 1280x720 and 640x360 for the intro and the music-video
  venues, 512x512 and 256x256 behind the music library and other menus; linear, no mips), and
  each of a movie's frames is new textures, decoded, sent and placed again
  (`deferred_decode.h`). Their fetch's swizzle puts the byte, 0 or 1 in each of RGBA
  (`guest_formats.h`'s `DecodeLevel8`; every plane seen has xxx1, 0xA00); R8 loads as (byte,
  0, 0, 1), so the pixel shader rearranges what the game's sampler filtered from it by the
  swizzle (`mesh.hlsl`'s `MapTexel`, as for DXN, coded by `R8ExpandCode` in
  `PixelUniforms`' `pixel_flags.z` and `.w`): each component filtered on its own, and a
  border one colour, that's the decoded texels filtered, bit for bit (the unit tests check
  every swizzle through the CPU's copy of the sampler), and every swizzle has such a code, so
  no k_8 texture stays RGBA. The same frames drawn with their planes kept as R8 and as RGBA
  copies are byte-identical (the music library, the part choice, a music-video venue, its
  controller dialog and the results after it). In 20th Century Boy in a music-video venue the texture
  arrays went from 1205 to 357 MB and the process's video memory from 1585 to 732-754 MB; a
  world frame's decode on the worker from 1.04 to 0.16 ms, its upload from 0.27 to 0.07 ms and
  0.44 to 0.13 ms of the GPU's time, and the textures sent from 2.6 to 0.7 MB a frame (the
  music library's 0.75 to 0.19, its arrays 232 to 137 MB). Growing the arrays for the intro
  movie took frames 12.8 and 23.2 ms (512 MB and 1 GB of RGBA) and for a venue's movie 7.9 and
  8.4 ms (341 MB); none of it now reaches the slow-frame log. The device's support for R8
  arrays is checked once (logged if missing: RGBA, as with it off). Read as each texture is
  placed, as `native_bc_textures` is; the CPU rasterizer and capture files decode RGBA from
  the bytes when they need it (`EnsureRgba`). `native_view stats`' `capture` counts
  `deferred_r8` (k_8 textures kept as bytes) and `deferred_r8_rgba` (those then decoded to
  RGBA).
- `native_deferred_decode_threads` (2): how many helper threads decode a frame's first-sight
  textures and meshes with the worker (`deferred_decode.h`'s `GatherPending`, `DecodeHelpers`):
  where those not yet decoded come to 4 MB or more of the game thread's copies (a song's first
  frames and its camera cuts have 5 to 15 MB; a movie's planes, decoded again each of its
  frames, 1.4 MB), each thread takes the biggest left until none are, the worker included,
  and the frame waits for all of them. Each is still decoded once, by the same decoder (its
  `call_once`), so the picture is the same; `deferred_decode_us` sums the threads' time. The
  helpers start when first wanted and sleep on a condition variable between such frames (none
  of their CPU in a 60 s slice of a song); perf_sample names them `band3 decode <n>`. 0 decodes
  on the worker alone, as before. At 20th Century Boy's start (`songstart.b3t`), its four
  first-sight frames' decode with 0, 2 and 3: 9.8, 14.1 to 14.7, 16.1 to 18.3 and 21.8 to 22.1
  ms; 3.8, 5.0 to 5.6, 5.8 to 7.2 and 7.8 to 8.6; 2.9 to 3.0, 3.8 to 4.0, 5.2 to 5.9 and 6.1 to
  6.3; the four frames' worker time in all 182 to 196 ms with 0, 150 to 165 with 2 and 156 to
  158 with 3 (two runs each). 2 is the default: 3 decodes a millisecond or two less but leaves
  the frames no shorter, and the Steam Deck's eight threads have the game's, the worker's, the
  command processor's and the audio's busy. What's left of those frames is the planning's
  render targets made (25 in 23 to 24 ms on the third), the world passes before the frame (35
  to 37 ms on the first) and recording (17 to 35 ms on the fourth).
- `native_view_premake_targets` (on): makes the textures RB3 draws into (outfits, faces, the
  crowd, the spotlights' targets, shadow maps) as it loads them, rather than in the frame
  that first draws into them, where each cost about a millisecond (a colour and a depth
  texture, each a committed resource): a song's first camera cut to the band made 25 in one
  frame, 23 to 26 ms of it. RB3 gives a rendered texture its surfaces in `DxTex::SyncBitmap`,
  seconds before its first pass (the song's 25 four seconds before that cut); the capture's
  hook announces it there and the worker makes it while it waits for a capture, up to four
  at a time and 4 ms (`src/Render/target_premake.h`), at most 64 made ahead and not yet used
  (unused ones go after 30 s as any target). One made ahead is undrawn, so it draws and
  samples as one not yet made, and only targets a pass doesn't have are made, so the picture
  is the same: 11 frames (render_song_evenodd's on a refracting venue and
  render_screens_menus') drawn the same moment with targets made ahead and shared depths and
  without had no pixel different. A soft-particle surface is made at the game's size and its
  first pass remakes it, as before. Targets without depth (RndTex's kRenderedNoZ: most of
  them, outfits and the spotlights' included) clear their depth at every pass and keep none,
  so those of a size share one depth texture; one that loads depth never gets it, and one
  whose type changes is made again. The slow-frame line says how many the frame found made
  ahead ("N made ahead" in its walk) and how many were made since the frame before, unused
  and shared depths; `by_kind` has `targets_premade`, `premade_used`, `premade_unused`,
  `shared_depths` and `premake` (their milliseconds, between frames, not in `ms`). At 20th
  Century Boy's start (four runs) the cut to the band went from 45 to 49 ms to 22 to 23 (24 of
  its 25 targets made ahead; the 25th, announced 10 ms before, 0.4 ms), the first 1402-draw
  frame's 8 targets (8 ms) to 2 or 3 (0.8 to 1.2 ms, the soft-particle surfaces), and in
  its 60 s slice the process's video memory from 552 to 557 MB to 533 to 536 (the shared
  depths; targets 35 MB at most); a song announces about 70.
- `native_view_premake_arrays` (on): makes the texture arrays the textures RB3 loads will go
  in as it loads them. Textures share an array per size class and format
  (`gpu_view.cpp`'s `PlaceTexture`), the first one 4 MB, made when the first texture of its
  class is drawn: a song's first frame made 19 (11.5 to 12.3 ms of its world passes before
  it), a cut a few more. `DxTex::SyncBitmap` sees each texture as it loads, with its size and
  format, 0.1 to 0.2 s before the frame that first draws it (a song's 240 in the last 0.2 s
  of its loading screen); the hook announces each kind once and the worker makes the class's
  first array if it has none, as `PlaceTexture` would, in the format the texture will be
  kept in (`native_bc_textures`, `native_r8_textures`), sharing `native_view_premake_targets`'
  budget between frames and coming back after 2 ms rather than 100 while some are left. That
  predicted 20 of the song's first frame's 21 arrays and 2 it didn't use, and 15 to 30 of
  each render check frame's, missing one at most (a texture its fetch keeps RGBA). One made
  ahead and unused goes after 10 s, then as an empty array does. The arrays are made and
  given out the same either way, so the picture is: the 11 frames above drawn the same moment
  from no arrays, with them made ahead and without, had no pixel different. At 20th Century
  Boy's start (four runs) the song's first frame went from 35 to 39 ms to 23 to 27 (its world
  passes from 24 to 25 to 13 to 15), and its four first-sight frames from 115 to 127 ms in
  all to 92 to 106 (168 to 180 before the pools were warmed and targets made ahead); the
  process's video memory in its 60 s slice stayed 536 to 539 MB.

## native_view stats: by_kind and residency

`native_view stats`' `by_kind` splits the live view's frames by what they drew under
even/odd rendering (`frame_compose.h`'s `FrameKind`): `world` (the game drew the world; the
native renderer shows the kept post buffer and draws the world's texture passes and the
overlay), `post` (the world frame's world composed in, drawn and post-processed), `between`
(neither, at a background rate below half the game's) and `full` (everything, or a frame
that doesn't say). Each kind has `rendered`, `skipped_busy` (captures skipped while one of
its frames was drawn), `ms` and `wait_ms`, and per frame drawn the worker's parts
(`parts_ms_per_frame`), what it drew, sent, made and let go of and what the GPU kept after
(`per_frame`), what capturing it cost the game's thread (`capture`), and the most the GPU
kept after one (`peak`); with `native_gpu_timestamps`, the frames timed (`gpu_timed`), the
GPU's milliseconds by part per frame timed (`gpu_ms`) and each frame's busy total
(`gpu_total_ms`). Since only post frames draw the world, the native renderer keeps
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

It changes what the game's cameras see, not the picture after: RB3 builds each camera's
projection from its vertical field of view and `Rnd::YRatio` (height over width, 9/16), so
band3 gives `YRatio` the window's height over width, and for a window taller than 16:9 widens
each perspective camera's vertical field of view while the projection is built
(`src/Hooks/aspect.cpp`, the numbers in `src/Hooks/aspect_model.h`). The game culls with the
same frustum, so nothing is missing at the new edges. A camera built for another shape is
rebuilt the next time it draws, so resizing the window, the setting and F8 apply at once (the
log says `fill window: the game's cameras at <w>x<h>`, or `at 16:9`).

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

## The sync-only GPU

With `renderer` native band3 doesn't load the emulated GPU at all. On Windows it presents on
the SDK's Direct3D 12 device, which the native renderer draws on, zero-copy; on Linux it
presents through the SDK's Vulkan presenter, each frame read back and uploaded: SDL_gpu and
the SDK each make their own Vulkan device, so the presenter can't sample the native
renderer's frames in place. With `native_upload_pipeline` (on) the next frame is recorded
while the GPU draws and reads back the one before, and the presenter waits for the vertical
blank (FIFO), as Direct3D 12's does. The
game still sends its GPU commands and waits on what the GPU does with them, so band3's
sync-only GPU (`src/Render/sync_gpu/`) reads them and does only what the game waits on: the
fences, the swap's interrupt, the vertical blanks, the occlusion queries' results (which
band3 then replaces with its own counts, `native_query_sample_count`), the read pointer and
the display gamma ramp. It draws nothing; the native renderer is the only picture, on the
SDK's presenter with its overlays (F3, band3's menus and the rest).

The log's `sync gpu:` lines show the ring the game set up, a summary every 10 s (packets
by opcode, waits, interrupts, swaps, vblanks, fences, anything unknown) and a warning when
the game has waited on the GPU for 2 s. The summary sorts the waits that stalled by their
packet's wait interval: `yield` (under 0x100: polls again at once, unless
`native_sync_short_wait_us` is set), `sleep` (0x100 to 0xFFF: sleeps the interval / 0x100
ms between polls) and `long_sleep` (0x1000 and up).

## native_view stats fields in full

The harness's [`native_view stats`](../test-harness.md#native_view-stats) `capture` and `by_kind`, every field.

| Field | |
|---|---|
| `capture` | what capturing cost the game's thread per game frame: `ms_per_frame` by hook (`mesh`, `multimesh`, `particles`, `rect`, `pass`, `present`, `other`), `us_per_draw`, counts `per_frame`, the caches' `sizes`, and with `native_view_capture_profile` on `steps_ms_per_frame`. Of the counts, `geom_copy_bytes` and `tex_copy_bytes` are the meshes' buffers and the textures' levels the game's thread copied out of guest memory where it saw them first or changed, to be decoded off it (`src/Render/deferred_decode.h`), `tex_decode_bytes` the RGBA it decoded itself (a render target's guest pixels, the noise map), and `deferred_decodes`, `deferred_decode_us` and `deferred_decode_bytes` (decoded Vertex, index and RGBA bytes) what the copies cost whoever took a capture with them first, the native renderer's worker or a `capture`: not the game's thread; of those, `deferred_bc_blocks` the block-compressed textures kept as blocks for the GPU (`native_bc_textures`; their bytes the blocks'), `deferred_bc_swizzled` those decoded to RGBA for a swizzle, and `deferred_bc_rgba` those kept as blocks whose RGBA something asked for after; `deferred_r8` the k_8 textures (movie planes) kept as their bytes for the GPU's R8 (`native_r8_textures`) and `deferred_r8_rgba` those whose RGBA something asked for after. `by_kind`'s `capture` has the game thread's three by kind of frame |
| `by_kind` | the same frames by what they drew under even/odd rendering ([native renderer](../native-renderer.md)); `{}` while it's off. Each kind's `parts_ms_per_frame` has `decode` (the worker decoding what capture left it to, at first sight of a song's or a shot's content; in `ms` too) and planning's parts too (`plan_setup`, `plan_walk`, `plan_targets`, `premake` (render targets and texture arrays made ahead between frames, `native_view_premake_targets` and `native_view_premake_arrays`: not in the total), `plan_arrays`, `plan_arena`, `plan_reserve`, and record's `post_plan`, `acquire` (getting its command buffers from SDL_gpu), `first_draw` (their first draws, where SDL takes their descriptor heaps), `draw_slow` (later draws over 0.05 ms: another heap pair) and `uniform_slow` (pipeline binds and uniform pushes over 0.05 ms: SDL making a uniform buffer)), which don't add up to the total, the world passes' before the frame (in `pre`: `pre_plan`, `pre_walk`, `pre_targets`, `pre_arrays`, `pre_upload`, `pre_record`, `pre_acquire`, `pre_first_draw`, `pre_draw_slow`, `pre_uniform_slow`, `pre_submit`, `pre_wait`), and `submit`, every submission of the frame's command buffers (those before its end are in `record` too); `per_frame` has the command buffers submitted (`submits`: one, and one more at each texture pass with mips and, with `native_view_submit_points`, at the resolve), `acquires`, `uniform_slow` and `draw_slow` (those slow calls), the world passes' `pre_submits`, `pre_targets_made` and `pre_arrays_grown`, the render targets made (`targets_made`, `targets_returning`), found made ahead (`premade_used`), made ahead between frames (`targets_premade`), made ahead and not yet used (`premade_unused`) and the depth textures targets without depth share (`shared_depths`), texture arrays made ahead between frames (`arrays_premade`, `native_view_premake_arrays`) and those given their first texture (`arrays_premade_used`), forgotten (`evicted_rts`) and released (`rts_released`), the textures and meshes drawn for the first time, the arrays grown, an arena rebuild's meshes copied on the GPU (`arena_copied`), what was let go early for room (`textures_pressured`, `meshes_pressured`) and kept by the 30 s alone (`meshes_by_time`, `textures_by_time`, in a song), and `rts_mb`; `plan_max_ms` the most each part took; `plan_spikes` the frames whose planning took over 8 ms, every one, and `plan_spikes_at_cut` those at a camera cut or up to 2 game frames after; with `native_gpu_timestamps` (Direct3D 12) also `gpu_timed` (the frames with GPU timings), `gpu_ms` (the GPU's milliseconds by part, per frame timed; `idle` is the GPU waiting for the CPU between command buffers), `gpu_total_ms` (each frame's busy total, all but `idle`) and `gpu_marks_dropped`/`gpu_bad_spans` (0 when all went well) |
