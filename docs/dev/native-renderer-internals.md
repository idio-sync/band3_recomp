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
[settings reference](../settings-reference.md#native-renderer) has the short version.

- `emulated_gpu`: retired, still read at startup into `renderer` (`off` is `native`; `on`
  is `both`, or `emulated` if `renderer` said so), then cleared at the next save.
- `native_present_zero_copy`, `native_present_pacing`, `native_present_request_paint`:
  turn off the presenting above, to compare.
- `native_present_pipeline`: records the next frame while the GPU draws the one before.
  Keep it off: it hangs AMD GPUs and grows video memory.
- `native_world_ahead` (experimental; off, but on by itself on an integrated GPU or a Steam
  Deck with `native_world_ahead_auto`): with even/odd rendering, draws each world frame's
  venue right after that frame without waiting for it, so the post frame after it only
  post-processes it and draws its overlay; the world's GPU time moves into the world frame's
  half of the pair. On a 60 Hz laptop in a paused song post frames went from 14 to 8 ms and
  latency fell 4 ms, the picture the same, but the frames shown barely changed there (the
  missing ones were pacing's, not the renderer's). In a playing song on that laptop (3 runs
  each, 2026-10-14) it was used on 85-89% of post frames: post frames' worker time 11.1 to
  6.2 ms at the median and 19.9 to 13.8 ms at p95, latency 22.5 to 17.5 ms, repeats 70 to 58
  a minute; on the R9700 at 120 Hz it hurt, so it's off by default and
  `native_world_ahead_auto` (on) turns it on by itself where the GPU is integrated and on a
  Steam Deck (below). Where post frames drawn this way still run about a frame's
  time it backfires (one too slow skips the next world frame, so the world drawn ahead is
  wasted and the post frame after draws the whole world, too slow again), so it measures the
  frames skipped with it and without in 2 s windows, keeps the one that skips fewer, and
  tries the other every eighth window (`present_model.h`'s `AheadChooser`): in that case
  it then stays within noise of off, where drawing ahead regardless skipped 18% more frames.
  `native_view stats`' `by_kind` has `ahead` (the world frames' milliseconds drawing it),
  `ahead_gated` (world frames it chose not to), `ahead_used` (post frames that used it) and
  `ahead_fallback_passes` (world texture passes a post frame drew itself all the same).
  `present_model.h`'s `WorldAheadWanted` has the rule; either pipeline still keeps it off.
  Integrated, on Direct3D 12: the SDK's device has a unified memory architecture
  (`D3D12_FEATURE_ARCHITECTURE`'s `UMA`; `native_view.cpp`'s `SdkGpuKind`, which names it as
  DXGI names its adapter). On Vulkan: SDL's device's `deviceType` is
  `VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU`, read once as the device is made, from SDL
  3.4.14's renderer behind a command buffer then cancelled, under the GPU timings' layout
  checks (`gpu_view.cpp`'s `ReadVulkanKind`); where those fail, only `steam_deck.h`'s
  `IsSteamDeck` is left. The worker logs what applied once the GPU draws and as it changes
  (`native_world_ahead: on by itself (integrated GPU: AMD Radeon(TM) Graphics)`).
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
- `native_view_shader_variants` (on): draws the common venue
  materials with variants of `mesh.hlsl`'s `PSMain` compiled with some of its `kShade*` flags
  fixed and its own texture filtering left out (`build_shaders.py`'s `MESH_VARIANTS`,
  `gpu_view.cpp`'s `MeshVariantFor`): lit per vertex, unlit, lit per pixel without and with
  the skin and hair families, all without projected light, refraction, movies, box maps and
  fades. A single-sampled mesh draw takes the first that fits its flags as planned and its
  maps' samplers (every filtered one the device's), else `PSMain`; `Prewarm` makes their
  pipelines too. On the laptop's 680M (RGA, the installed driver) `PSMain` needs 72 VGPRs, 7
  of 16 waves a SIMD; the variants 24, 26, 48 and 64, and they draw 89.5% of the venue
  pass's GPU time (four venues). The same captured frames drawn with them off, on, each alone
  and off again had no pixel different in 140 frames over four venues on Direct3D 12. Fixing
  the shadow off as well (40 VGPRs) wasn't exact: fxc reordered `Light()`'s colour products,
  a level or two off in a few pixels a frame, so the shadow stays a run-time branch. In
  20th Century Boy at 60 Hz (three runs each) the venue pass went from 5.0 to 4.0 ms and,
  with `native_world_ahead`, post frames' GPU p95 from 19.8 to 14.9 ms and repeated frames
  from 83 to 52 a minute. On the Steam Deck (Vulkan, RADV), paused songs in the same four
  venues captured off, on, off, on had no venue pixel different among those that held still;
  at 1080p repeated frames went from 54 to 23 per 28 s and post frames' GPU wait p50 from
  10.9 to 9.2 ms.
- `native_view_instancing` (on): a run of consecutive draws that differ only in world (a
  multimesh's instances, which `CaptureMultiMesh` unrolls into a draw each: the crowd's
  cards) is one instanced draw. The plan walk folds each into the run's head
  (`gpu_view.h`'s `SameInstanceState` and `InstanceRuns`: the very next index, the same
  geometry, texture, shade state, target, camera, blend, depth, cull, draw mode, view and
  colour, not skinned, not a DrawRect quad, cone, soft particle or blur, never past
  `post_boundary` or across a pass run) and puts the worlds in the bone buffer;
  `mesh.hlsl`'s `World()` reads instance `SV_InstanceID >> instancing.z` from
  `instancing.y`, where `z`, 0 now, leaves room for a stereo eye in the low bit. The record
  skips the folded draws; `native_view stats` counts `instanced_draws` and
  `instances_merged`. The same captured frames drawn with it on and off (`capture`'s
  `<name>.gpu.uninstanced.png`) had no pixel different, with `vr_show_all` and without. In
  20th Century Boy at arena_04 on the R9700 (two runs, stretches of 600 frames, means):
  with `vr_show_all` draws went from 5,290-5,440 to 750-780 a frame, record 6.5-6.9 to
  4.5-4.6 ms, submit 1.6-1.8 to 1.4, plan 0.84-0.86 to 0.55-0.56, frames 8.0-8.3 to 6.1-6.4
  ms, and the GPU's world 1.6-1.7 to 1.0-1.2 ms; without it 570-890 draws to 370-420 and
  frames 1.9-2.1 to 1.7-1.8 ms. What `vr_show_all` still costs is mostly its ~95 texture
  passes a frame (and 86 command buffers, against 8 without it), not its draws.
- `native_view_hoist_passes` (on): RB3's crowd (`WorldCrowd::DrawShowing`) draws each
  character type's picture into one 256x512 target, draws that type's cards, and reuses the
  target for the next: a version a type, 5 to 8 a frame in a song and 84 with `vr_show_all`,
  each splitting the world's pass and (`native_view_mip_submits`) a submission of its own.
  `gpu_view.h`'s `PlanHoist` gives a target several of a frame's passes draw a layer per
  version (the newest in layer 0, so whatever reads it after this frame, or as a normal or
  projected map, reads layer 0 as before; the rest in 1, 2..., the array made in steps of 8
  layers and never shrunk while the target lives), and the cards read their version's
  layer. It's only for targets every pass of which clears colour and depth, starts before
  `post_boundary` and isn't carried, read only as a diffuse map by world draws after the
  version is made, and with the inline mips (`DrawMips` draws one layer's). Passes then move
  ahead of the world's first back-buffer run, in order, where nothing between reads what they
  change (`HoistablePass`; Plan's stand-ins and anything before the world stay first), and the
  moved ones submit once, after the last, so the GPU draws them while the world is recorded.
  Not for a post frame drawing over the world drawn ahead. `native_view stats` counts
  `passes_hoisted`, `rt_layers` and `rt_layers_mb`; DRED labels give the layer a pass draws
  into. The same captured frames with it on and off (`capture`'s
  `<name>.gpu.unhoisted.png`) had no pixel different, two in a song (8 versions) and one
  with `vr_show_all` (84). In 20th Century Boy at arena_04 on the R9700 (600-frame stretches,
  full frames, 1280x720): with `vr_show_all` record went from 4.6 to 2.2 ms, submit from 1.45
  to 0.1 (86 submissions to 3), plan from 0.57 to 0.78 (planning the move), frames from
  6.2-6.3 to 5.0 ms mean and 7.6 to 5.6 p95, and the GPU's world from 1.0-1.2 to 0.3-0.4 ms
  (its total 2.9-3.1 to 2.2-2.3), for 58 MB of layers (96 of them); without it record 1.0 to
  0.84, frames 1.72 to 1.64. At 4073x2291 with `vr_show_all` (the display went to 60 Hz, so
  even/odd frames) post frames went from 8.3 to 7.0 ms and world frames from 6.4 to 5.3.
- `vr` (debug, off, restart; Windows): the native renderer's picture in a headset through
  OpenXR (`xr_present.h`). `openxr_loader.dll` is loaded at run time (the usual search path,
  else SteamVR's `bin\win64`), never linked, and only with `vr` on; the session is bound
  (`XR_KHR_D3D12_enable`) to the SDK's device and direct queue, two swapchains of the
  runtime's recommended size, drawn by `present.hlsl` on band3's own command list on that
  queue (typeless images get a UNORM view, bytes verbatim; typed sRGB ones
  `PSPresentLinear`). Once the session runs, `xrWaitFrame` paces the native renderer's worker
  (`VrFrame`): each frame re-poses the newest capture for each eye (`vr_pose.h`'s `RePose`:
  the world camera's draws get the eye's view and `EyeProjection` of the captured
  projection, their shades cloned with the eye's position and billboard axes), draws both
  into the eye output slots (`GpuRenderer::EyeSlot`; the left into a window slot when one is
  free, published as the window's picture) without the overlay (`BackBufferPart::kEyes`), the world
  drawn ahead, kept buffers, motion blur, DOF or the spotlights' haze, then presents them and
  ends the frame with a projection layer. The eyes are drawn at `vr_eye_scale` (0.8) of the
  swapchain's size from a fixed viewpoint and `vr_window_eye_scale` (1) through the window
  (`vr_pose.h`'s `EyeScaleOf`, `EyeDrawSize`), and `present.hlsl` stretches them into the
  images, bilinear; the HUD panel stays at the swapchain's size. At 0.8 (2115x2115 an eye on
  the Steam Frame) the twelve fixed-viewpoint captures' eyes took 0.2-0.4 ms less GPU each
  (the world and post-processing, about two thirds of the pixels; the texture passes are
  their own size), the CPU the same; at 1 they're the pictures as before, pixel for pixel.
  The eyes and the panel at two sizes, drawn by turns every frame, each need the frame's
  targets (`scene`, `color`, `depth`, post-processing's levels, the overlay's MSAA pair,
  the blur scratches) at their own size: `EnsureTargets` sets the frame's aside
  (`gpu_view.cpp`'s `FrameTargets`, `SwapTargets`) rather than letting them go, and takes
  the set aside back when the next picture is its size, so the eyes and the panel each
  keep a set (one more at the eyes' size, ~120 MB at 2115x2115 with its readback buffer); a set left aside 2 s
  (`kSpareTargetsKeep`: a size the window had) is let go. Before (2026-10-21) every eye
  pair and panel made all of them again: the user's first live run from a fixed viewpoint
  went to ~21 fps on the Steam Frame at 144 Hz, eyes 20.7 ms of CPU and the panel 20.2
  (0.25 through the window, where both are 2424x2424), the slow-frame log's `plan: setup`
  10.5-11 ms a picture and submits 4.6 ms. `eye_cost`'s `with_hud`, below, draws them by
  turns as `VrFrame` does: at arena_04's three fixed viewpoints, eyes 2115x2115 and panel
  2644x2644, eyes 22.9-23.8 ms (setup 10.3-11.3) and panel 20.0-20.5 (setup 12.4-12.7)
  before, eyes 6.6-6.8 ms (setup 0.01) and panel 0.8-0.9 after, both waited for the GPU;
  pictures the same, pixel for pixel.
  With `vr_shared_eyes` (on) the right eye keeps what the left drew of each texture pass
  that neither sees itself (`RasterOptions::keep_eye_passes`, `gpu_view.h`'s
  `KeepsEyePass`): none of its draws has the eye's world `view_proj` (`RePoseStats::
  view_proj`, so `RePose` moved none of them: the crowd's impostors, outfits, heads, shadow
  maps and NgLight's shadow have cameras of their own), and its target's layer still holds
  the version the pass makes (`Rt::layer_version`: a renamed target's passes each draw a
  layer of their own), drawn with the same options; it's then counted
  (`GpuStats::passes_shared`; `passes_eye` those with a draw the eye sees) and not drawn.
  NgLight's shadow is three passes into one layer (its casters, then two blurs in place,
  each a version), so only its last could be kept and the eye draws all three. On the
  twelve fixed-viewpoint captures at 2644x2644 the right eye kept 84-110 of 88-117 passes
  (small_club_01: 1 of 4-6), its recording 1.8-2.3 ms down to 0.55-0.9 and its GPU
  2.2-3.6 ms to 0.8-1.7; through the window 9-10 of 12-13 passes, 0.1-0.2 ms less of
  each. The right eye's pictures are the same as without it, pixel for pixel, on all
  seventeen captures.
  Also with `vr_shared_eyes`, an eye without its own dark (a fixed viewpoint, not
  fading) is drawn from the capture itself rather than `RePose`'s copy of it
  (`RasterOptions::eye_override`): `vr_pose.h`'s `EyeOverrideOf` sets out what the copy
  would change (`ViewSwap`s: the world camera's matrix and the eye's pose, each track
  camera's, with its inverse for the folded ones; the crowds' meshes for
  `vr_crowd_clear`), `EyeDrawOf` what that is for one draw (its matrix: its own, a
  swap's or folded; the swap whose pose its shade takes; squashed; a track's draw cut
  below the picture), and `Render`'s walk applies it as it plans each draw kept
  (`eye_drawn`, the cut geometry in `eye_geom`): the instances' worlds and the bones
  squashed, the matrix at recording, the shade's eye point and billboard axes patched
  into the draw's copy of its `ShadeParams` (which no longer changes `shades[d]`), the
  back buffer's layout and placement through the eye's matrix and the tracks' whole
  picture (`LayoutBackBuffer`'s and `PlaceBackBufferDraw`'s overloads). `RePose` is now
  the copy with `EyeDrawOf` applied, so the two can't drift; the CPU (`Rasterize`,
  `native_view_replay --track-at`) draws its copies as before (replay's eyes the same,
  pixel for pixel). The eyes' dark (the window, a fade) adds draws, which the override
  can't: those eyes take `RePose`'s copy. On the twelve captures the override took
  0.02-0.22 ms to make an eye against `RePose`'s 0.7-1.45, the walk 0.1-0.3 ms more
  (`EyeDrawOf`, the crowd's distances), and the eyes are the same pictures, pixel for
  pixel; the two eyes' CPU at 2644x2644 went from a mean 5.2 to 4.2 ms a frame
  (6.4 before the passes were shared), the GPU 3.5 (4.7).
  Where both eyes have an override and `GpuRenderer::CanRenderEyes` (no kept buffers, world
  ahead or trails, a world without REFRACT_WORLD draws, whose world passes before the frame
  each eye would draw of its own) `VrFrame` draws them in one go
  (`RenderEyesToOutputs`): `Render` takes the second eye (`Impl::SecondEye`), plans both in
  one walk (`EyeDrawOf` per eye into `eye_drawn[2]`) and one upload, then records each
  view in turn, each in a command buffer of its own with its own timings, output and
  fence, the first's submitted before the second is recorded. The second view's own
  instance worlds and bones go after the walk's where the crowd near its eye is squashed
  otherwise; it records again only the texture passes it must (`redraw1`): those with a
  draw it sees itself, every pass into a target several of the frame's passes draw into
  one layer (a character's shadow map, NgLight's shadow: by the second view it holds only
  its last version, which the earlier reads would see), and from those on every pass into
  a target one of them draws or sampling one. Its stats are its recording alone
  (`stats[1]`); the first's have the plan and the upload. On the twelve captures the eyes
  are the same pictures, pixel for pixel (the first try, without the shadow maps' rule,
  gave four right eyes with a band member's self-shadows of the last member's map), and
  the two eyes' CPU went from a mean 4.2 to 3.7 ms a frame at 2644x2644 (the GPU 3.5).
  Two things tried there and left out (2026-10-18): leaving out each eye's crowd cards
  wholly outside its picture (a conservative test of their corners, each eye its own
  instance worlds) left the pictures the same, pixel for pixel, and culled 4,500 of 4,600
  cards from front_row at arena_04 (all 8,344 at festival_01, 800-2,600 from on_stage and
  venue), but those cards cost the GPU next to nothing off the picture (the world's GPU
  time didn't move: 0.46 ms an eye at arena_04's front row) and only 16-37 draws fewer,
  while the tests and the second set of worlds cost the plan 0.1-0.7 ms a frame
  (`out/vr/step5_crowd_cull.patch` in the VR worktree); and dropping bloom and glare
  from the eyes (`eye_cost`'s `frame.no_bloom`) saves 0.12-0.24 ms of GPU a frame at
  2644x2644 (post-processing 0.31-0.37 ms for both eyes, 0.18-0.20 without), too little
  for the halos they give, so they stay. The overlay's draws (`soft_raster.h`'s `OnHud`, from
  `vr_pose.h`'s `HudBoundary`: `post_boundary`, or 0 for a frame the eyes can't see) are
  drawn alone as the HUD panel (`BackBufferPart::kHud`): transparent black, alpha as
  coverage, premultiplied (`native_view_replay --overlay-only`, `--split-check`). Draws that
  tint what's behind them can only darken it there, by one alpha: REFRACT_WORLD glass (its
  output affine in the picture behind: shaded with it black and white) and Multiply cover but
  for the mean of what they let through (`shade.hlsli`'s `HudGlassTransmit`, `HudTransmit`),
  Multiply's colour still multiplying the panel's own, and a pass of its own before it adds
  its hue where the panel is clear, as if behind were 0.3 grey (`HudMultiplyHue`). On the
  song list (2026-10-18) that took `--split-check`'s mean from 5.3 to 3.1 of 255 and under
  the album art from 7.0 to 2.0; the menus' tinted header and footer bars stay the largest
  difference. With `vr_hud` (on) `VrFrame` draws it from the capture
  itself (not re-posed) into the HUD output slot (`GpuRenderer::HudSlot`) at the eyes' size
  with `native_view_msaa`, only when the capture is new and the SDK's queue has copied the
  last one out (`Session::HudFree`); `xr_present.cpp` copies it (`present.hlsl`'s
  `PSPresentHud`, alpha kept; into images the runtime reads as sRGB, typed so or typeless in
  an sRGB swapchain as SteamVR's are, `PSPresentHudLinear` through an sRGB view decodes the
  covered part unpremultiplied and premultiplies again, so partly covered pixels aren't
  darkened) into a third swapchain of the eyes' format and
  size, shown as an `XrCompositionLayerQuad` in LOCAL space after the projection layer,
  `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` alone (premultiplied). Frames without
  a new picture submit the layer with the image last released. It hangs where
  `vr::HudPlacement` puts it (16:9, `vr_hud_distance` 1.8 m ahead, `vr_hud_below` 0.25 m
  down, `vr_hud_width` 55 degrees across, all read each frame) from the head pose of the
  first located frame, taken again when `vr_hud_recenter` changes or the runtime moves LOCAL
  space (`XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING`). A frame whose panel starts at
  0 (Play a Show's intro, the calibration screens) puts the whole picture on it, opaque, and
  the eyes are the clear colour (black). The `xr` block's `hud` has its `renders`, `copies`,
  `layers`, `recenters` and its drawing's CPU and GPU ms per picture and per frame; `capture`
  writes `<name>.gpu.hud.png` (premultiplied colour and coverage alpha) with `.gpu.L.png`, which
  `native_view_replay <cap> out.png --overlay-only --diff <name>.gpu.hud.png` sets the CPU's
  against. On the null driver (2026-10-17) the CPU's and GPU's panels matched to a mean 0.03
  to 0.23 of 255 (alpha 0.00 to 0.04), and in 20th Century Boy at arena_04 a picture cost
  0.27 ms of CPU to submission and 0.25 ms of GPU (0.62 for Play a Show's whole frame);
  xrBeginFrame to xrEndFrame went from 3.83 ms without the panel to 4.23-4.45 with it.
  `vr_anchor` places the head's LOCAL space in the
  venue for a fixed viewpoint until the song's venue is found (below; default above and
  behind arena_04's front crowd; empty follows the game camera),
  `vr_world_scale` scales it, `vr_fake_pose` turns the head slowly for a headset that doesn't
  move. `native_view stats` has an `xr` block, and `capture` writes `<name>.gpu.L.png` and
  `.gpu.R.png` from the last VR frame's eyes (and `.gpu.hud.png`, above). Particles stay flat to the game camera and the
  crowd's impostor pictures are the game camera's view. On SteamVR 2.17.10's null driver
  (1852x2056 an eye, 90 Hz) the session reaches SYNCHRONIZED but never VISIBLE: the null
  headset is idle (activity level 0), so `shouldRender` stays off and `xrWaitFrame` gives ~33
  frames a second; the eyes are drawn and submitted anyway. Its first session after SteamVR
  starts is ended for SteamVR's room setup, which terminates band3 (on 2026-10-17 that one
  went VISIBLE and FOCUSED for a second first, the quad layer accepted). Per frame, 20th Century
  Boy at arena_04 on the R9700 (600-frame stretches): without `vr_show_all` each eye 0.56-0.88
  ms of CPU to submission, 0.62-0.81 ms of GPU, RePose's copy 0.11-0.18 ms, xrWaitFrame's
  return to xrEndFrame 4.1-4.4 ms; with it 2.7-3.0 ms of CPU, 2.9-3.5 of GPU and 0.6-0.8 for
  RePose an eye, 12.3-12.8 ms a frame, over 90 Hz's 11.1 (late 82-87% of frames): each eye
  draws the crowd's ~85 impostor passes again. The eyes' pictures differ by a 2-pixel shift
  at the stage, as a 63 mm IPD at ~20 m gives.
  The game camera's window (`vr_viewpoint` game_camera, the default, and every menu in any
  viewpoint; `vr_pose.h`'s `ResolveFrame`, chosen per frame by `xr::LayoutFor`): the head at
  the recenter stands where the game's camera is. The window is a rectangle in LOCAL space
  `vr_hud_distance` ahead of that head at eye height, turned by its yaw alone,
  `vr_window_width` (100) degrees across from there and 16:9 (4.29 x 2.41 m at 1.8 m),
  the same at every cut. The game camera's view (`GameFovOf`: `2 atan(1 / proj.m[0][0])`
  across, `proj.m[2][1]` up and down) fills it: the eyes see a true window onto the venue
  squeezed along the camera's forward (`WindowEye`, `VenueToWindow`: across and up 39.37
  units a metre times `vr_world_scale`, so the eyes are the user's IPD apart in the venue,
  along the forward k times that, k = tan(width / 2) / tan(hfov / 2), 1.44 for a 79-degree
  song shot, 2.8 for the 46-degree main hub, 8.5 for a 16-degree close-up). From the
  recenter's head the window is exactly the game's picture; elsewhere the parallax is
  exactly that squeezed venue's, so leaning moves the eye u a metre across and k u along
  the forward, and the stereo depth is flattened k times (a point Z units ahead looks
  Z / (k u) metres away). In songs `vr_window_max_squeeze` (2; 0 none, runtime) caps k: sz
  is at most 2 u, sy as the uncapped sz gives it, so a shot narrower than that is the
  game's picture whole, level and centred in the window, `max_squeeze / k` of it each way
  (`WindowScale::picture`: 0.53 at 35 degrees, 0.24 at 16; the frustum's plane at the
  window is then `max_squeeze u D` ahead), its depth flattened twice at most; the eyes'
  dark reaches the picture's edge (`PictureFrame`) and a song's panel in the window
  (`vr_hud_in_window`) lies on it. Menus fill the window at any field of view, their
  panel on it. Cuts fade over a size change; a zoom within a shot past the cap shows as
  the picture resizing. Each eye's view is that map into LOCAL then the eye's pose,
  scaled by k u so depth from the recenter's head is the camera's (`Eye::view`, which
  `RePose` uses), with the headset's own field of view; the window is level in the room
  whatever the camera's pitch and roll, the picture in it rolled as the game's is. On the
  Steam Frame (2026-10-18) the left eye's field of view is about 59 degrees out and 50 in,
  so a 100-degree window reaches each eye's inner edge from the recentred head. Around it
  is dark at `vr_window_dim`'s alpha: nine quads (`FrameQuads`: four strips tiling the
  window's plane around it out to 3 times its distance, and the four sides and back of the
  box they make, so every way out of the box but through the window is covered once),
  which the eyes draw themselves after the resolve and before the tracks (`Eye::dark`,
  black through `kEyeDarkCam`, alpha blended, no depth; `RePose`'s `AddDark`), so the 3D
  track can reach past the window's edge. `vr_window_quads` shows them instead as quad
  layers of one 8x8 swapchain between the eyes' layer and the panel's, as before, to
  compare. The panel lies on the window (its pose and size) in
  menus, and on its own place (`HudPlacement`) during songs, which are `InSong()` and the
  screen up being `game_screen` or `practice_game_screen` (`SongScreen`, from
  `PresenceMgr::UpdatePresence`), so the loading vignette and results count as menus;
  `vr_hud_in_window` puts it on the window in songs too. A frame with `HudBoundary` 0 gets
  black eyes (Present's `eyes_black`) and the panel on its own place. `VrFrame` feeds every
  capture it sees to `CameraCuts` first; at a cut, or as the eyes change between window,
  anchor and black, they fade in from black over `vr_cut_fade_ms` of the runtime's display
  time (`CutFade`), the panel not: drawn by the eyes under the tracks, which don't cut
  (`Eye::fade`, a black quad over the picture), or with `vr_window_quads` by
  `present.hlsl`'s `frame_size.w` over the eyes' whole picture.
  The fixed viewpoints (`vr_viewpoint` front_row, on_stage, venue; `vr_venue.h`): in songs
  no window, no dark and no squeeze, the eyes posed from an anchor (`EyePose`) the game's
  cuts don't move (no fade at a cut; one as the anchor is placed or moved, or the mode
  changes), with the 3D highway and the panel on its own place as before; menus keep the
  window. RB3's venues share no coordinates, so the anchor comes from what the game drew:
  `src/Hooks/vr_view.cpp` marks the meshes `BandCharacter::DrawLodOrShadow` and
  `WorldCrowd::DrawShowing` draw (`scene_capture.h`'s `MeshOwnerScope`,
  `FrameCapture::mesh_owners`, kept in captures' OWNR section), and `VenueRefOf` takes the
  world camera's draws of them: each member where its mesh stands (the middle across, its
  lowest vertex up: the stage floor is the members' median), `toward` the audience from the
  band's middle to the crowd's (-y without a crowd), and of the crowd in front of the band
  (as wide as it and 4 m either side) the last row (90th percentile of depth) and, 2 m
  either side of the band's middle, the first row (2nd percentile) and the audience's floor.
  `PresetAnchor` places front_row on the band's middle line 0.3 m before the first row,
  eyes 1.7 m (times `vr_world_scale`) over the audience's floor, facing the band; on_stage
  on a 0.25 m grid among the band at least 0.9 m from every member, nearest the middle line
  and 0.5 m downstage of the band's front line (the median member's depth), eyes 1.7 m over
  the stage floor, facing the audience; venue 2 m behind the crowd's last row, 0.35 of its
  distance from the band (2.5 to 7 m) above the stage floor, facing the band. Without a crowd
  front_row is 5 m past the front-most member over a floor 1.2 m under the stage's, venue
  15 m. `vr_viewpoint_offset` ("right,ahead,up[,yaw]", metres and degrees in the place's own
  frame) moves it. `xr::LayoutFor`'s `VenueTracker` takes the reference from a song's frames
  seen from a fixed viewpoint as they come, again only for more of the band or a first
  crowd, keeps it through the game camera's window and forgets it outside songs; each one
  found is logged with the three places (`vr: the venue: ...`). Until then songs are seen
  from `vr_anchor`. Each reference taken is cleared of the frame's geometry (`ClearPlaces`,
  again for another `vr_world_scale`): `ObstaclesOf` takes the world camera's solid and
  alpha-blended draws near the three places and along venue's line of sight (the band's
  among them; not the crowds', squashed anyway, nor additive light or billboards) as world
  triangles in runs of 32 with their boxes, and `BlockedAt` checks a place: nothing within
  0.3 m of either eye (`kHeadClear`), within 0.5 m over the head (`kHeadroom`), across the
  line of sight to the band's middle 1.2 m over the stage floor (straight ahead from
  on_stage) and 15 degrees either side of it for its first metre (venue's all the way but
  the stage's last 3 m), or standing (not lying flat: floors and decks don't count) within
  0.75 m ahead, 35 degrees either side (`kNearClear`: a stage monitor at arm's length). A
  place blocked steps, nearest first, as its mode may: front_row back into the crowd up to
  2 m, 0.25 or 0.5 m sideways and up to 0.3 m up; on_stage upstage up to 1.5 m and sideways,
  still 0.9 m from every member; venue back up to 3 m and down (to a standing viewer's
  height over the audience) up to 2 m or up to 1 m. Without a clear one it keeps the least
  blocked (head before headroom before sight before near), nearest. The move is in the log
  line (`moved x,y,z m from near`) and the harness's `vr` JSON (`venue.cleared`); it costs
  6 to 20 ms once per reference. In 20th Century Boy (2026-10-18): arena_04 front_row up to
  0.4 m back from its stage monitors (2.6,-654.3,270.8), big_club_04 front_row 0.1 to 0.2 m
  back and venue 2 m down from under the ceiling to see the band past a ceiling beam
  (5.6,-700.7,144.4), festival_01 front_row 0.6 m back off the barrier (-22.9,-760.1,248.4),
  small_club_01 venue lowered 0.25 m under its low ceiling; on_stage nowhere moved.
  The black front_row eyes before it (arena_04, -1.3,-638.7,269.5) weren't the place but a
  crowd card 2 cm from the eye that the squash below left whole: a billboard turns its quad
  to the eye about the world's translation and ignores the world's axes, so zeroing them
  didn't shrink it; `SquashMatrix` now also zeroes m[3][3], which scales a billboard's
  corners (`mesh.hlsl`, `soft_raster.cpp`; 1 in every world the game gives, `ReadXfm`).
  In a song from a fixed viewpoint (with `vr` on)
  `vr_view.cpp` draws the whole world in every shot as `vr_show_all` does (the hooks' test is
  `vr_show_all` or a fixed viewpoint in a song, `RunFrame` once a frame), and as it starts
  mid-shot undoes the shot that last hid things (`CamShot::UnHide`) and shows every crowd at
  once. The crowds' draws within `vr_crowd_clear` (1.5 m) of an eye across the floor are
  squashed to a point in its capture (`Eye::crowd_clear`, `RePose`: their world's or bones'
  axes zeroed, and the world's m[3][3], which scales a billboard card's corners, so the
  draws and passes stay), so the eye isn't inside a card.
  The band is held still across the game's cuts there too (`vr_hold_band`, on by default;
  `src/Hooks/vr_teleport.h`, game-side rather than in the renderer). RB3 moves every member
  at every cut, twice over: `BandCamShot::StartAnim` keeps each target's local transform and
  sets it to the shot's mark (`TeleportTarget(t, mXfm, false)`; marks were up to 3 m away at
  small_club_01 and 10 m at arena_04, often turned 30 to 120 degrees), `EndAnim` sets it back
  (`TeleportTarget(t, kept, true)`), and `BandWardrobe::StartVenueShot` flags each member to go
  home at its next `BandCharacter::Poll` (`Teleport` onto its waypoint, undoing the walking and
  turning its clips did during the shot, up to a metre or so and as much as a half turn).
  Held, `TeleportTarget` is skipped (`TeleportGate`: a member a shot moved before the hold
  began is still put back as that shot ends) and the Poll's go-home flag (Pollable +1142,
  Character +1534) cleared; instead a leash (`LeashStep`, in the Poll hook) moves the member's
  local transform toward its waypoint's world transform (BandCharacter +1528; what
  `Character::Teleport` copies), its heading turned about its feet, a little each frame:
  free within 0.5 m and 60 degrees, the excess taken back over 1.5 s, past 1.25 m or 120
  degrees over 0.4 s, never at once (a member walking off at 1 m/s settles 1.45 m out), then
  `RndTransformable::SetDirty` as `Teleport` does. Shadows, instruments and lights follow, as
  the game itself moves the member. Shots also hide band members (`StartAnim`'s
  `SetShowing(!mHide)`: a drum close-up hides the rest of the band); from a fixed viewpoint,
  held or not, `graphics.cpp`'s `StartAnim` hook clears each target's hide bit (0x04 of its
  flags at +0x60) while `StartAnim` reads it, as it does self_shadow for `force_self_shadow`.
  `vr_view.cpp` logs each stretch from a fixed viewpoint (`vr: from the fixed viewpoint, over
  N frames, the band jumped ...`): jumps (a member moving over 0.25 m or turning over 30
  degrees between two frames), teleports skipped, leash pulls, members kept showing, each
  member's farthest and mean distance and turn from its waypoint, and the hooks' time on the
  game's thread. In 20th Century Boy (2026-10-21, `out/vr/hold_ab_<venue>_front_row.b3t`:
  stretches of 30 s off, on, off, on): small_club_01 31 and 25 jumps off (the biggest 1.45
  and 3.28 m), 2 and 3 on (0.72 m, a member put back from a mark as the hold began, and 0.16
  m); arena_04 39 and 45 off (4.84 and 9.99 m), 2 and 5 on (0.44 and 0.91 m, the same); a
  whole song held at small_club_01, 4 (0.16 m) in 12,366 frames, 361 teleports skipped. Held,
  the members stand 0.3 to 0.6 m from their waypoints on average (the leash's free radius; the
  game's snaps home kept that 0.2 to 0.5 m at small_club_01, 0.7 to 1.9 m at arena_04 with
  its far marks), turned 13 to 43 degrees; at most 1.0 to 1.6 m and, briefly, a half turn
  (a clip turning faster than the leash). The hooks cost the game's thread 18 to 40 us a frame
  held (1.3 to 1.7 us not), the renderer nothing. While held, the game's own picture on the
  monitor shows its close-ups without the members on their marks.
  In 20th Century Boy (2026-10-18, captures with the stand-in head, no headset) the band
  was four members everywhere: arena_04 stage floor z 256, audience 205, front_row at
  5,-647,272, on_stage 41,-536,323, venue 10,-1082,436 (x,y,z; its drummer on a riser 11 m
  upstage); small_club_01 stage 13 and audience level with it, -11,-54,80 / 23,30,80 /
  -31,-333,139; big_club_04 stage 49, audience 5, -3,-278,72 / -39,-169,116 / 8,-701,228;
  festival_01 stage 257, audience 182, -24,-735,248 / -28,-442,324 / -84,-1508,533, the
  audience on -y in all four (the crowd's middle within 5 degrees of it). Switching to a
  fixed viewpoint mid-song can find fewer members at first (shots hide members other ways
  than their hide lists); the place is taken again, with a fade, as the rest are drawn.
  What it costs: the native view's frames at 1600x900 (15 s each, the game at 60 fps) went
  from 1.3-1.6 ms with the window to 5.0-5.8 ms from any of the three at arena_04,
  big_club_04 and festival_01 (1.4 to 1.8 at small_club_01), and the capture's left eye at
  1440x1440 from 0.7-1.1 ms of GPU (RePose 0.25-0.34 ms) to 2.3-3.5 ms (RePose 0.7-1.3 ms,
  5,700-9,800 draws): the whole band and crowd every frame, each eye drawing the crowd's
  impostor passes again, the cost work that's next. With `vr` on,
  `capture` writes the layout (`vr` in the reply, `<name>.vr.json`: with `picture_scale` and
  the picture's corners `picture_px`, the `viewpoint`, its `anchor`, the song's `venue` as
  found, and `eye_cost`, below, with the crowd draws it cleared), `.gpu.L/.R.png` from
  the last VR frame's views (without a session a stand-in head at the origin, both eyes at
  it, 60 degrees each way), `.gpu.C.png` (with a window: an eye at the recenter's head
  facing the window, the venue alone), and `.gpu.eyes.png` (the capture's camera, the
  world's draws alone), and `tools/vr_window_check.py` sets the window's quad of the
  centre eye against the latter and composes `<name>.vr.sim.png` as the compositor would. On SteamVR's null driver
  (2026-10-18; its head reports a 180-degree roll, so its pictures are upside down) the
  window matched the game camera's picture to a mean 0.8-4.2/255 on the vignettes, closet,
  music library, part select, loading, song and pause (moved a tenth of its width:
  7.6-36), but 16.3 on the main hub, where the eye lacks the 720p picture's bloom halos;
  Play a Show's intro was black eyes. `native_view stats`' `xr` block counts
  `window_frames`, `black_frames`, `fades` and `track_frames`.
  `eye_cost` is the left eye drawn as `VrFrame` draws it, GPU timestamps on, at the eyes'
  size and at the Steam Frame's (2644x2644, `frame`), medians of three rounds after one
  left out: RePose's ms, then the eye's frame drawn (`first`) and at once again (`second`,
  as the right eye's would be), each with the CPU's parts (`plan_ms` and of it the targets'
  `setup_ms`, `upload_ms`,
  `record_ms` and of it the texture passes' `record_passes_ms`, `submit_ms`, the slow SDL
  calls), what it drew and the GPU's ms by part (`world`, `pass_other`, `pass_shadow`,
  `pass_spot`, `mips`, `post` with the resolve's copies, `overlay`, `gamma`, `upload`,
  `idle`; `gpu_ms` their total). Without a session it draws into the left eye's output
  and waits for it; with one, which owns that output, it reads the picture back, whose
  recording costs more (1.2-1.3 ms more at 1440x1440, more again at 2644). The
  harness's `vr_eyes` draws a saved capture's eyes again with the settings as they
  stand, so builds and settings compare on one frame (`out/vr/gen_vr_eyes.py` and
  `eye_cost_table.py` in the VR worktree run the twelve fixed-viewpoint captures and five
  of the window's). On the twelve (2026-10-18, stand-in head, no session), each eye cost,
  at 2644x2644: CPU RePose 0.9-1.2 ms, plan 0.5-0.9, record 1.8-2.2 of which the texture
  passes 1.3-1.7 (small_club_01: 0.5-0.7 record, 0.1 passes), submit 0.06-0.14; GPU
  2.3-3.7 ms, of it the texture passes and their mips 1.5-2.0 (pass_other 0.9-1.2, mips
  0.55-0.73: 84-113 crowd impostor passes), the world 0.46-1.27, post-processing 0.16-0.19.
  At 1440x1440 the passes are the same and the world about half. The window's (five
  captures): CPU RePose 0.1-0.2, record 0.2-0.6, GPU 0.7-1.1. The second drawing cost the
  same as the first: each eye draws every texture pass again. `frame.no_bloom` is both eyes
  drawn in one go at 2644x2644 without bloom and glare: their GPU ms and post-processing's.
  `with_hud` (with `vr_hud`, both eyes in one go) is both eyes and then the HUD panel at
  the headset's size, by turns, as `VrFrame` draws a frame, each waited for: the eyes'
  CPU ms (`eyes_ms`, `eyes_setup_ms`) and the panel's (`hud_ms`, `hud_setup_ms`, its
  `hud_size`). The rounds before it draw at one size throughout, so only `with_hud` shows
  what changing size between the eyes and the panel costs (`out/vr/vr_eyes_hud.b3t` and
  `with_hud_table.py` in the VR worktree).
  The 3D highway (`vr_track_3d`, on): in songs each eye draws the tracks from where it is,
  as an object in front of the head, in the default window and from `vr_anchor` alike.
  RB3 draws each track after post-processing with a camera of its own (z range 0..1),
  before the flat HUD's 0..0.1 camera (score, stars, band meter, lyrics, the vocals'
  strip), a full-picture fade quad's camera and, when it's up, the pause menu's; the
  multiplier, the Overdrive meter, the hit text and the player's name ride on the track.
  `vr_pose.h`'s `TrackCamerasOf` finds them: one for one player (93.9 units behind and
  22.6 above the strikeline, the track's origin, looking 7 degrees down, 36.6 degrees
  across), two for two players (998x720 viewports at either side, each projection
  shifted out by as much again: `freecam::Decompose` takes an off-centre projection's
  shift out of the axes, which read before as cameras 7.7 units aside), three for four
  parts (turned 10 degrees apart; the vocals' strip is the flat HUD's). Their draws are
  the eyes' (`RasterOptions::eye_cams`, `OnHud`): drawn after the resolve over the
  venue, layered among themselves as in the game, never depth-tested against the venue,
  and left off the panel. `PlaceTracks` puts each camera's frame in LOCAL space so the
  reference track's strikeline is `vr_track_forward` (1.2 m) ahead of the recentred head
  along the floor and `vr_track_down` (0.3 m) below its eyes, `vr_track_scale` (30) of
  its units a metre, lying level as the game lays it out, its far end raised
  `vr_track_pitch` degrees; two players' tracks are moved aside by the picture's shift at
  their strikelines (side by side, parallel), four parts' keep their cameras' turn.
  `TrackEye` gives each eye the camera's view from where it is against that frame and
  `RePose` moves the cameras' draws to it (gems, beat lines and sustains fold their place
  into their matrix, which is kept: `FoldedInto`), gives the camera the whole picture,
  and cuts each draw where the game's picture ends below (`CutAtPlane`, a per-thread
  cache), so the track's near end stops where the game's picture cuts it instead of
  reaching under the head. The poses are held through the song (`TrackHold`; a camera
  moving more than a unit or a degree is placed again, logged). The renderers have no
  far plane: the track's far end is its own fade. At the defaults one player's track is
  0.65 m wide and 29.6 degrees across at its strikeline, 1.24 m away and 14 degrees down
  (the game's own angle onto it), its near end 0.53 m ahead, where the game's picture
  fades it out (y 106) 4.73 m ahead and 3.6 degrees down; two players' strikelines 0.52
  m either side, four parts' side tracks 0.82 m either side, 1.1 m ahead. The capture's
  `track_cost` (the left eye drawn with and without them, timestamps on, stand-in, 1440x1440)
  put the tracks and the eyes' dark at 0.10-0.23 ms of GPU an eye and 0.15-0.27 ms of
  CPU (RePose 0.04-0.11 of it) in four song captures (2026-10-18, no headset). `native_view_replay --track-at x,y,z` draws both eyes
  without a headset (a stand-in head moved by x,y,z for the parallax) and prints where
  each track is; `--list` prints the track cameras, `--track-eyes` splits by them.
  The HUD in pieces (`vr_hud_split`, on; `vr_hud.h`): on the user's first Steam Frame run
  (2026-10-21) the 3D highway looked right but the flat HUD didn't, one panel 1.8 m away that
  the track runs through (0.5 to 4.7 m) with the venue behind it, so the eyes couldn't settle
  on it and the gems at once. Now in songs with the tracks in front the panel's one picture
  is cut into pieces, each an `XrCompositionLayerQuad` of its rectangle of the panel's image
  (`subImage.imageRect`, whole texels: `PieceTexels`), no new swapchain or drawing; with the
  dark's nine (`vr_window_quads`) that's 1 + 9 + 4 layers at most, of the 16 OpenXR promises.
  `HudPiecesOf` takes each of the panel's draws that shows something (not blended away: the
  fade's quad at alpha 0) and its rectangle of the picture (its vertices through its camera's
  viewport, padded 0.004), and puts it in a piece by where it is (`HudPieceOf`): the band
  meter's wholly left of u 0.2, its middle below v 0.3; the score's (box, stars, progress bar,
  time) wholly right of u 0.72, its middle right of 0.775 (the song's title's lines end at
  0.773, right-aligned against it, however short) and between v 0.15 and 0.6; the top strip's
  with its middle above v 0.45 (the lyrics, the vocals' strip and its pills, the title, a
  Unison Bonus's banner); else the rest's. A draw over the whole picture (the fade at work), a
  draw of a 0..1 camera after the flat HUD's (the pause menu's) or a rest over 0.1 of the
  picture keeps the panel whole. On the captures (`native_view_replay <cap> x.png
  --overlay-only --track-eyes --hud-pieces`) one player's (trk_a), two players'
  (screen-mp2-25s) and four parts' (screen-mp4-10s, -25s) split into meter, score and top with
  nothing left over, and the three pause menus stayed whole; finding them took 0.010-0.021 ms.
  `HudPieceHold` (`VrFrame`'s `vr_hud_pieces_`, reset outside songs) takes a piece as each
  frame has it for 2 s after it first shows (RB3 slides and grows the HUD in), then pins its
  anchor (the meter's bottom right corner, the score's bottom left, the top's bottom edge at
  the picture's middle) till the song ends, so nothing in it moves as its rectangle changes,
  and keeps that rectangle the union of the last 4 to 8 s, so it grows at once, doesn't
  jitter as lyrics' lines change length, and lets go of what's gone. The first try, a union
  over the whole song from the anchor first seen, caught the HUD coming in: on the Steam
  Frame's session the meter was first logged 0.19 m tall (0.29 settled) and later 0.21 x
  0.53 m. With the settling, an 80 s run logged the meter at u 0.039..0.089 v 0.31..0.69 and
  the score at u 0.767..0.981 v 0.197..0.396 throughout, the top strip following the lyrics'
  lines (`vr: the HUD's pieces grew`, at most once a second). A frame that can't be split
  shows the whole panel once three come in a row. `PlaceHudPieces` hangs them from
  the recentred head, each facing it, level, at the size it'd be in the whole picture
  `vr_hud_piece_width` (55, the panel's) degrees across from where it hangs: the meter's
  corner `vr_hud_side_gap` (0.05 m) left of the leftmost track's edge at its strikeline and
  the score's as far right of the rightmost's, at the strikeline's height; where either's
  middle would be over `vr_hud_side_max` (40) degrees aside, both above the tracks' middle,
  the gap apart, their bottoms 13 degrees over every track's far end seen from the head (each
  track's instrument badge stands there; just clearing the far end covered four parts'
  middle badge, as did 8 degrees); the top
  strip's bottom middle `vr_lyrics_height` (0.05 m) over the window's top edge's middle, or
  from a fixed viewpoint `vr_lyrics_distance` (2.5 m) ahead and `vr_lyrics_elevation` (20)
  degrees up; the rest where the panel would have it. Menus, the pause menu, `vr_hud_in_window`
  and songs without the 3D track keep the whole panel. A quad shows a rectangle of the
  image, and four parts' vocals' strip runs over the score box's and meter's tops, so
  `PackHudPieces` gives each piece a place in the image (where it is unless that overlaps one
  placed before it, else the first free place) and `VrFrame` draws the panel from a copy of
  the capture with the moved pieces' draws shifted in clip space (`ShiftHudPieces`, 0.02-0.15
  ms on the captures; one player's score moves too, its progress icon's box meeting the
  title's at a corner); the layer shows the piece's image rectangle. At the defaults, from
  the head (stand-in, 2026-10-21, `out/vr/hud_split_eyes.b3t` and `hud_split_song.b3t` in the
  VR worktree): one player's meter 0.07 x 0.29 m (2.9 x 12.7 degrees) 1.22 m ahead, 0.42 m
  left, its middle 0.16 m below the eyes, the score 0.29 x 0.15 m 0.52 m right and 0.23 below
  (both 1.3 m away, as the strikeline), the lyrics and title 0.66 x 0.27 m, 2.2 m away, 1.37 m
  up through the window (38 degrees) or 2.4 m ahead and 1.06 m up from a fixed viewpoint;
  two players' and four parts' meter and score at the tracks' middle 1.2 m ahead, over their
  badges, four parts' vocals' strip 2.2 x 0.44 m (53 degrees) above the window. Finding the
  pieces took 0.01-0.03 ms a picture.
  The capture's `vr` JSON has the frame's pieces (`hud_split`: each one's rectangle, size,
  pose, middle from the head and corners in the left eye), which `tools/vr_window_check.py`
  lists and puts into `<name>.vr.sim.png` (and outlined in `.vr.pieces.png`); `native_view
  stats`' `xr` block's `hud` counts `split_frames` and `pieces_per_split_frame`, and the log
  says where they hang as they first show (`vr: the HUD in 3 pieces, ...`). With the Steam
  Frame's session synchronized but not worn (2026-10-21, SteamVR 2.18.4 at 144 Hz, so
  throttled and not for timing) all 262 frames of 40 s in the song ended with 3 piece
  layers, none refused, and the panel's picture cost 0.18 ms of CPU with the split as without.
  Each runtime frame's timing (`vr_timing.h`'s `FrameTiming`) is kept in a ring of the last
  8,192 (about 68 s at 120 Hz), which the harness's `vr_frames [name]` writes as CSV to
  `logs/vr_frames-<name>.csv` beside the exe: in ms from xrWaitFrame's return, xrBeginFrame's
  return, the views located, each eye's and the panel's submission, the GPU done (the
  `OutputDone` waits, eyes and panel apart), `Present`'s start, then its steps (the command
  allocator's fence, acquiring and waiting for the eyes' images and the panel's, recording and
  submitting the copies, releasing, xrEndFrame) and xrEndFrame's return; the time in
  xrWaitFrame, the steps from the frame before's wake and `predictedDisplayTime`, the display
  time less the wake (with `XR_KHR_win32_convert_performance_counter_time`, which band3 asks
  for where the runtime has it), the capture drawn (its age since the game's Present, the
  game's frame time, whether it's the one the frame before drew, game frames skipped, those
  published by Present), each eye's CPU, GPU (`native_gpu_timestamps`) and RePose ms, and the
  window's picture (published after xrEndFrame, the left eye; the next xrWaitFrame waits on
  it). The `xr` block's `late` is as before: WaitFrame's return (xrBeginFrame and
  xrLocateViews done) to xrEndFrame's return over the display period, the CPU's part of the
  frame without xrWaitFrame's wait; it doesn't say the runtime showed the frame late. Beside
  it: `missed` (the display time moved on 1.5 periods or more since the frame before's: the
  runtime showed a refresh without a new frame) and `slots_missed`, `over_period`
  (xrWaitFrame's return to xrEndFrame's over the period), `late_wakes` (xrWaitFrame returned
  1.5 periods or more after the frame before's), `repeats`, `behind_newer` and `slow`; and
  `timing`, the frames recorded since `native_view on` (as far as the ring reaches) split
  into `late` and `on_time`, each with those counts and every part's `p50`, `p95`, `p99`
  and `max`. A frame that ended 1.5 periods or more after its wake, or missed a slot, is
  logged with its breakdown (`vr: slow frame ...`), at most one a second with the count
  since. The period frames are judged by is the headset's refresh (`RefreshPeriod`: the
  shortest `predictedDisplayPeriod` of the last 256 frames): SteamVR predicts twice the
  refresh while it throttles a late app to every other refresh, and judged by that a
  throttled frame's two-refresh step looked on time. On the Steam Frame (2026-10-18, 120
  Hz, the user's two 60 s runs) the old count said 210 and 257 missed where 310 and 391
  refreshes were; `display_period_ms` is now the refresh, `predicted_period_ms` the last
  predicted, and `throttled` the frames predicted 1.5 refreshes or more.
  With `vr_gpu_queue_wait` (on) the CPU no longer waits for the eyes and the panel before
  `Present`: `GpuRenderer::OutputFence` hands over the `ID3D12Fence` SDL 3.4.14 signals to 1
  as each output's command buffer finishes (its `D3D12Fence`'s first member, checked each
  time: an `ID3D12Fence` on the SDK's device at 0 or 1), the SDK's queue `Wait`s on them
  before the copies, and the images are released and xrEndFrame called at once (OpenXR's
  D3D12 binding needs the writes submitted to the session's queue, which they are). After
  xrEndFrame the worker waits for the copies (`Session::CopiesDone`, `done_wait_ms`; past
  2 s it holds, as `WaitFlight` does) and only then lets `OutputDone` release SDL's fences,
  since SDL signals a fence back to 0 on the CPU when it reuses it and a queue that hadn't
  reached its wait would then wait for the fence's next use. Without a fence it falls back to
  the CPU's wait (logged once). In the song at arena_04 with SteamVR's session SYNCHRONIZED
  (the Frame not worn, 2026-10-18, 60 s each in one session): located to xrEndFrame's
  return p50 6.01 / p99 8.16 ms to 3.79 / 4.96, xrEndFrame 2.34 to 2.15 ms, refreshes missed
  179 a minute to 3; the CPU's wait had been 1.6 ms for the eyes (p95 3.2) and 2.0 for the
  panel (its 250 us polls sleeping to the timer's 1 ms and more).
  `vr_game_rate` (auto) runs the game at the headset's rate while a session runs: each VR
  frame asks `pacing::VrGameCap` for a cap (auto: the refresh's own period, a number of Hz,
  or off) and `pacing::SetVrCap` publishes it in place of frame_cap's (mode `vr`, logged
  `Frame cap: 120.00 Hz for the VR headset`), the vsync handled as frame_cap's cap; the
  worker clears it once no VR frames run. The phase lock doesn't steer it. With band3ctl's
  `--frame_cap=60` overridden (2026-10-18, the song at arena_04, session SYNCHRONIZED): the
  game at 120.2 fps, its thread ~2 ms of each 8.33 (present_stats' cap waited 6.36 ms a
  frame on average), the world every 4 frames (30 fps), the song's clock 30,044 ms over
  30,014 of wall clock (60 fps: 30,020 over 30,011), the eyes repeating a capture in 3% of
  headset frames (50% at 60), located to xrEndFrame p50/p99 3.87/5.04 ms, 8 refreshes
  missed a minute.
- `native_view_render_scale` (debug, 1): the presented picture is drawn at that times the
  window's size (after `native_max_height`; `native_view.cpp`'s `ScaleRender`), with the
  screen-sized passes following (`ScaleForPicture`), and `present.hlsl` scales it to the
  window (bilinear, so a smaller picture is aliased). It's read each frame, so `set` changes
  it at once, the targets and outputs remade as at a resize; `native_view stats` and
  `screenshot native` give the drawn size. It's for measuring the GPU at sizes a window
  can't reach: 3.182 at 1280x720 is 4073x2291, 9.33 MP, two 2160x2160 eyes; 2.25 is
  2880x1620, one eye. In 20th Century Boy at arena_04 on the R9700 (600-frame stretches,
  means), the GPU's frame went from 0.66-0.71 ms at 1280x720 to 1.34 at one eye's pixels and
  2.36-2.51 at two (world 0.22-0.26 to 0.73-0.80, post 0.09 to 0.46, overlay 0.07-0.10 to
  0.60); with `vr_show_all`, from 2.72-2.85 to 4.11 and 4.51-5.21 (world 0.92-1.03 to
  1.48-2.12). The CPU's record, submit and plan don't change with the size, so frames grow
  only by the GPU wait: 1.7-1.9 to 3.5 ms without `vr_show_all`, 6.0-6.3 to 7.9-8.3 with
  it. `native_view_mip_submits` off at 9.33 MP with `vr_show_all` cut submits from 86 to 2,
  record from 4.5 to 2.4 ms and submit from 1.4 to 0.07, but the GPU started later: the
  wait went from 3.0 to 5.7 ms and the frame from 8.3 to 8.9.

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
and `camera` the cuts seen. The meshes, textures and texture arrays are let go in one pass
every 8 frames (`kEvictScanEvery` in `gpu_view.cpp`; the walk was 0.2 ms of the worker's CPU
a frame), so what is past its keep stays up to 8 frames longer. One drawn again after its keep
but before the pass counts as drawn in more than one frame (a texture whose last use is past
its first, a mesh that moves to the arena) and is kept for 120 frames or by the clock, where
it would have been let go and made again: `resident_meshes` rose by about 70 in a song, and
the picture is the same. A full texture array lets go what Evict would have before it grows,
so what waits for the pass doesn't make an array grow. `evicted_meshes` and `evicted_textures`
come in steps, `meshes_by_time` and `textures_by_time` can be 8 frames old, and render targets
are checked every frame.

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
| `by_kind` | the same frames by what they drew under even/odd rendering ([native renderer](../native-renderer.md)); `{}` while it's off. Each kind's `parts_ms_per_frame` has `decode` (the worker decoding what capture left it to, at first sight of a song's or a shot's content; in `ms` too) and planning's parts too (`plan_setup`, `plan_walk`, `plan_targets`, `premake` (render targets and texture arrays made ahead between frames, `native_view_premake_targets` and `native_view_premake_arrays`: not in the total), `plan_arrays`, `plan_arena`, `plan_reserve`, and record's `post_plan`, `acquire` (getting its command buffers from SDL_gpu), `first_draw` (their first draws, where SDL takes their descriptor heaps), `draw_slow` (later draws over 0.05 ms: another heap pair) and `uniform_slow` (pipeline binds and uniform pushes over 0.05 ms: SDL making a uniform buffer)), which don't add up to the total, the world passes' before the frame (in `pre`: `pre_plan`, `pre_walk`, `pre_targets`, `pre_arrays`, `pre_upload`, `pre_record`, `pre_acquire`, `pre_first_draw`, `pre_draw_slow`, `pre_uniform_slow`, `pre_submit`, `pre_wait`), and `submit`, every submission of the frame's command buffers (those before its end are in `record` too); `per_frame` has the command buffers submitted (`submits`: one, and one more at each texture pass with mips and, with `native_view_submit_points`, at the resolve), `acquires`, `uniform_slow` and `draw_slow` (those slow calls), the world passes' `pre_submits`, `pre_targets_made` and `pre_arrays_grown`, the render targets made (`targets_made`, `targets_returning`), found made ahead (`premade_used`), made ahead between frames (`targets_premade`), made ahead and not yet used (`premade_unused`) and the depth textures targets without depth share (`shared_depths`), texture arrays made ahead between frames (`arrays_premade`, `native_view_premake_arrays`) and those given their first texture (`arrays_premade_used`), forgotten (`evicted_rts`) and released (`rts_released`), the textures and meshes drawn for the first time, the filtered texture reads (a texture a draw) the device's samplers can do (`samples_hw`; they do with `native_view_hardware_sampling`) and why the rest can't: padded into a larger array layer, as not a power of two (`samples_npot`), under 64 (`samples_small`) or past 2:1 (`samples_aspect`), or a sampler mode they lack (`samples_mode`), the arrays grown, an arena rebuild's meshes copied on the GPU (`arena_copied`), what was let go early for room (`textures_pressured`, `meshes_pressured`) and kept by the 30 s alone (`meshes_by_time`, `textures_by_time`, in a song), and `rts_mb`; `plan_max_ms` the most each part took; `plan_spikes` the frames whose planning took over 8 ms, every one, and `plan_spikes_at_cut` those at a camera cut or up to 2 game frames after; with `native_gpu_timestamps` (Direct3D 12) also `gpu_timed` (the frames with GPU timings), `gpu_ms` (the GPU's milliseconds by part, per frame timed; `idle` is the GPU waiting for the CPU between command buffers), `gpu_total_ms` (each frame's busy total, all but `idle`) and `gpu_marks_dropped`/`gpu_bad_spans` (0 when all went well) |
