# Native renderer and render checks

Experimental: band3's own renderer for the game's frames, an alternative to the emulated
Xbox 360 GPU, and the tools that check its picture against the game's.

## Switching renderers

`renderer` (Band3 → Graphics) picks what draws the game's picture: `emulated` (the
default), the emulated Xbox 360 GPU, or `native`, band3's own renderer (the native view's,
below) drawing each frame the game sends at the window's size, under the SDK's overlays.
**F8** (`bind_renderer`) switches between them at once, without a restart; the emulated GPU
keeps running either way. The picture keeps the game's 16:9 with black bars, or stretches
when the SDK's `present_letterbox` is off. On Windows the frames go to the window without
leaving the GPU; where that can't be done (other platforms, `native_view_backend` cpu, or
`native_present_zero_copy` off) each frame is read back and uploaded instead. The log says
which (`native present: zero-copy`, or `native present: uploading each frame (<why>)`).

| Setting | |
|---|---|
| `renderer` (Band3 → Graphics) | `emulated` (the default) or `native` |
| `native_present_zero_copy` (Band3 → Debug) | on (the default) shows the GPU's frames where they are; off reads each back and uploads it, to compare |

## Render checks

The native view (F9, experimental) draws the game's frames itself, from a capture of
what RB3 drew: on the GPU, or on a reference CPU rasterizer. It draws RB3's shading
(point, box and projected lights, characters' self-shadows, normal and detail maps),
its textures as the game's samplers read them (filtered, between mip levels, clamped
or wrapped), the passes RB3 draws into textures (outfit composites, the crowd's
impostors, shadow maps, NgLight's projected shadow, heads' normal maps, blurs), its
post-processing (depth of field, bloom or glare, the spotlights' beams and haze, soft
particles such as stage smoke, the colour matrix) and, last, the display's gamma ramp.
Render checks set its picture against the game's.

| Setting (Band3 → Debug) | |
|---|---|
| `native_view_backend` | `gpu` (the default) or `cpu`, the reference rasterizer. The GPU falls back to the CPU when it can't start |
| `native_view_record_targets` | records the passes RB3 draws into textures all the time, even while the native view is off. Off by default, as it costs a little game-thread time while characters load. Render checks need it from launch: RB3 composes a band's outfits once, in the main menu |
| `native_view_rt_fallback` | `guest` (the default) also keeps what guest memory holds of a texture RB3 draws, sampled where no recorded pass made it (right only with `--readback_resolve=full`); `none` keeps only which texture and version it is |
| `native_view_normal_maps` | on (the default) shades normal and detail maps, live and in `capture`'s `.gpu.png`; off shades those materials with the vertex normal, to compare. Captures from before the capture kept the meshes' tangents have none either way |
| `native_view_texture_filtering` | on (the default) samples textures as each draw's fetch constants say: bilinear or point, the mip level (or two, blended) by how far and at what angle the surface is, anisotropy, and wrapping, mirroring or clamping per axis, from the mip chains in guest memory (and a render target's own, made after its pass). As the game's own picture under band3 is drawn, the SDK's `anisotropic_override` (4:1 by default) applies to the samplers it would apply to there. Off reads every texture's nearest texel at full size, as before, to compare; captures from before the capture kept the samplers and mips are drawn so either way |

Launch render checks with:

```
python tools/band3ctl.py launch --fresh -- --native_view_record_targets=true --test_random_seed=21 --async_shader_compilation=false
python tools/band3ctl.py run tests/game/boot.b3t
python tools/band3ctl.py run tests/game/render_song.b3t
```

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
dialog and the results.

Besides the back buffer's draws and the texture passes, a capture keeps the
characters' shadow-map passes and NgLight's shadow casters, and the display's gamma
ramp the game was shown through (the screenshot has it). `capture`'s reply:

| Field | |
|---|---|
| `draws` | the draws the capture kept |
| `skipped_pass` | draws left out for their draw mode: the velocity buffer's and other passes the native view doesn't draw |
| `skipped_shadow` | shadow-map or shadow-caster draws outside their own pass; 0 expected |
| `passes`, `passes_carried` | the texture passes in the capture, and those carried in from earlier frames (a band's outfits: there only with `native_view_record_targets` on from launch) |
| `rt_sampled` | the render target versions the draws sample |
| `rt_filtered` | of those, the ones whose pass drew nothing the capture keeps (the velocity buffer, or draws with no material or geometry) |
| `rt_missing` | the others no pass in the capture made; 0 means none it could have had is missing |
| `rt_fallback` | `native_view_rt_fallback` |
| `proc_cmds` | what the frame drew: 7 everything; with even/odd rendering 1 the world, 2 post-processing; -1 unknown |
| `composed`, `world_frame`, `game_frame` | the capture has the world of `world_frame` in front of the overlay of its own `game_frame` (a post frame, which shows the world frame before it) |
| `held_fallback` | no such post frame came in 30 frames, so the capture took the last |
| `gpu`, `gpu_ms`, `gpu_passes`, `gpu_rt_missing` | the GPU's `<name>.gpu.png` at the screenshot's size, its time, the texture passes it drew, and its draws that sampled a render target nothing had drawn (drawn transparent black). `gpu_error` instead when there's no GPU device or `native_view_backend` is `cpu` |

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
| `--list` | the passes and every draw: mesh, material, where it lands, what its shader was given, its `cull` (2 `D3DCULL_CW`, 6 `D3DCULL_CCW`); and `shadow:` lines checking each self-shadowed draw's shadow map |
| `--shade <draw>` | everything one draw's shader was given |
| `--pick X,Y` | the draw that last wrote that pixel (at `--size`), its colour and shade |
| `--dump-rt <DxTex hex>[:<version>]` | what that render target holds after its pass (`out.alpha.png` its alpha) |
| `--dump-tex <draw>[:<map>][@<level>]` | a draw's diffuse texture as captured, or one of its shade's maps (`--shade`'s names: `normal`, `projected`...); with `@<level>`, that mip level of it. `--shade` prints each one's sampler and how many levels the capture kept |
| `--dump-alpha <png>`, `--dump-depth <png>` | the scene target's alpha or depth, as the GPU's `.gpu.alpha.png` and `.gpu.depth.png` |
| `--view alpha\|depth` | draws that view instead of the picture (with `--diff` against those PNGs) |
| `--dump-bloom <png>` | bloom's first level as the composite read it, after glare's pass |
| `--rt-none`, `--rt-guest` | never use guest memory's pixels for a render target; or draw no texture passes and use guest memory's alone |
| `--no-post`, `--post-only xfm\|dof\|bloom\|spot\|soft` | no post-processing; or only the colour matrix, depth of field, bloom (and glare), the spotlights' beams or the soft particles |
| `--no-gamma`, `--gamma-from <other.cap>` | no gamma ramp; or another capture's |
| `--no-shadow` | characters without their self-shadows |
| `--no-normal` | every material with its vertex normal, no normal or detail map |
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
