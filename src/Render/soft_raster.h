#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "src/Render/scene_capture.h"

// Experimental: draws a FrameCapture on the CPU. It stands in for a real
// GPU backend so the probe tests the captured data, not a graphics API.
//
// With the capture's passes (RasterOptions::texture_passes), the texture
// passes the frame samples are drawn too, in the order RB3 drew them, each
// into a target of its own (by DxTex) kept for the frame; a draw sampling a
// render target samples what its passes have drawn so far, which makes the
// versions right (the crowd's impostor is drawn eight times a frame, each
// sampled in between). Texture targets keep alpha: impostors are alpha-cut
// against the clear's 0, and outfit layers blend by it. A material's textures
// are read as the game's samplers read them (sample_model.h: filtered,
// between mip levels by the pixel's footprint), a render target's mips made
// after its pass as the GPU makes them (BuildMips). The spotlights'
// cones shade by spot_model.h instead, reading the world's depth where they
// are on the screen, and the depth volume's blurs blur it in place; the soft
// particles (scene_capture.h's IsSoftParticle) fade by that depth, and their
// buffer's blurs take their taps from one surface into the other. A shadow
// map's pass (kTexTypeShadowMap) draws depth alone, clip z/w less than what's
// there (cleared to 1), into a float buffer of its target's, which the
// SHADOW_BUFFER draws after it read (RasterOptions::self_shadow). NgLight's
// shadow (scene_capture.h's ProjectedTargetOf) is drawn as RB3 draws it: its
// casters' silhouettes into its texture (ShadowCasterPass), blurred twice in
// place (spot::SpotBlur); the projected light's draws read that as their s5.
//
// The back buffer's draws are split at post_boundary, as RB3 draws them: the
// world's go to a scene target, which keeps alpha as RB3's back buffer does
// (the bloom weight PSEUDO_HDR shaders write, WritesSceneAlpha below; cleared
// to 0) and whose depth stays readable; at the boundary it's resolved into the
// picture, where post-processing goes, and the overlay's draws (track, HUD) go
// on top. The resolve is RB3's post-processing (post_model.h), run on the
// scene target's colour, alpha and depth, or a copy (alpha made opaque) on
// frames without it or with RasterOptions::post off. An overlay draw that
// reads the picture behind it (RefractsWorld) reads a copy of it as the
// resolve left it; a world draw that does, the last world frame's scene
// (RasterOptions::pre_buffer). The display's gamma ramp (gamma_ramp.h) goes
// over the finished picture. The back buffer starts as TheRnd's clear colour
// (ClearRgba), and its draws go in their cameras' viewports, layered by their
// z ranges as RB3's are, where the capture has its cameras (LayoutBackBuffer);
// its DrawRect quads (flares, ScreenMasks, the intro movie) are drawn too,
// but the post copy, which post-processing redoes.
//
// A picture bigger than the game's 1280x720 has the passes that are pictures
// of the screen (the spotlights', the soft particles') drawn bigger with it
// (RasterOptions::target_scale, PassTargetSize), the others at their size.
//
// The overlay is multisampled, as RB3's is (RasterOptions::msaa,
// OverlaySamples): RB3 draws the world into a 1x back buffer and everything
// after DoPostProcess (the track, the HUD, panels after EndWorld) into a 2x
// one, which EndTiling resolves into the front buffer as the mean of each
// pixel's two samples. Every texture pass, the world and its depth (which
// post-processing reads) are 1x, there and here. The CPU does it the way a
// GPU's MSAA does: each overlay pixel has a colour and a depth per sample, at
// D3D's standard positions (kSamplePositions in soft_raster.cpp), the
// picture copied into each at the resolve; a triangle covering any of them is
// shaded once, at the pixel's centre, and depth-tested and blended into each
// sample it covers; the frame's end averages them into the picture.

namespace band3::render {

namespace post {
struct PostHistory;
}

// What a renderer hands back: the picture, or, to check the scene target, its
// alpha or its depth as grey (DepthViewGrey) where the world's draws left them
// at post_boundary, without the overlay
enum class RasterView { kFinal, kSceneAlpha, kSceneDepth };

struct RasterOptions;
struct DrawItem;
// whether an overlay draw over the whole picture is cut at the game's 16:9
// (RasterOptions::overlay_cut): its vertices' clip y, in the game's 16:9
// frame, all within the cut's; the renderers then leave it unstretched and
// draw it only in the 16:9 frame's columns
bool InOverlayCut(const DrawItem& it, const RasterOptions& o);

struct RasterOptions {
    uint32_t width = 640;
    uint32_t height = 360;
    bool textures = true;
    // RB3's lighting (shade_model.h) from each draw's ShadeState; off draws
    // every material unlit (ambient 1)
    bool lighting = true;
    // the placeholder lighting from before the game's (a fixed directional
    // light on materials that aren't prelit), which captures without shade
    // states always get: to compare the two on one capture
    bool legacy_light = false;
    bool skinning = true;
    bool blending = true;   // off draws every material opaque
    bool culling = true;    // off draws both sides of every triangle (DrawItem::cull)
    // in a capture without its cameras (FrameCapture::cameras), the back
    // buffer's depth is cleared each time a camera draws first, as the
    // renderers did before they layered cameras as RB3 does
    // (LayoutBackBuffer); off, never. A capture with them never clears.
    bool clear_depth_per_camera = true;
    // the texture passes the frame samples drawn natively, and sampled; off,
    // a render target is what guest memory held of it, or nothing
    bool texture_passes = true;
    // a render target no pass has drawn samples guest memory's pixels where
    // the capture kept them (native_view_rt_fallback guest), else
    // transparent black; off, they're never used (texture_passes off then
    // draws render targets untextured)
    bool rt_guest_pixels = true;
    // RB3's character self-shadows: the shadow maps (scene_capture.h's
    // kTexTypeShadowMap) drawn, their depth alone, and read by the
    // SHADOW_BUFFER draws after them (shade.hlsli's ShadowLit); off, those
    // draws are lit, as before the capture kept the maps
    bool self_shadow = true;
    // RB3's normal maps and detail maps (NORMAL_MAP, NORM_DETAIL), where the
    // capture kept the geometry's tangents (Geometry::tangents) and decoded
    // the maps: the normal tilted in the tangent frame the game's vertex
    // shaders build (shaders/shade.hlsli's MappedNormals); off, those draws
    // are shaded with the vertex normal, as before the capture kept tangents
    bool normal_maps = true;
    // the game's samplers (scene_capture.h's TexSampler, sample_model.h):
    // filtering, mip levels by the footprint, anisotropy and addressing as
    // each draw's fetch constants say, where the capture kept them; off,
    // every texture nearest at level 0, wrapping, as before
    bool filtering = true;
    // RB3's post-processing at post_boundary (post_model.h): depth of field,
    // bloom and the colour matrix, as the frame set them; off, the scene as
    // it is
    bool post = true;
    // with post, only these of its effects (post_model.h's kPost bits), 0 all
    uint32_t post_only = 0;
    // with post, the film grain (the composite's noise) where the frame has
    // it and the capture kept its map; off, left out
    bool grain = true;
    // with post, the camera motion blur (velocity blur) where the frame has
    // it and the capture kept the velocity buffer's cameras; off, left out
    bool velocity = true;
    // with post, the trails (blend previous), which read the previous post
    // frame: the live view's, which draws frame after frame and keeps each
    // post frame's composite (the GPU in GpuRenderer, the CPU in
    // post_history, which its owner keeps between frames). Off (captures,
    // replay: one frame, none before it) they're left out, which is the
    // game's picture wherever no trail has started (post_model.h's PlanPost).
    bool trails = false;
    post::PostHistory* post_history = nullptr;
    // A frame that post-processes nothing of its own under even/odd rendering
    // (frame_compose.h's ShowsPostBuffer: a world frame, or one that draws
    // neither) shows what the game's does: the post buffer as the last post
    // frame left it, its picture before the overlay, under the frame's own
    // overlay (DxRnd::DoPostProcess copies the post buffer to the screen
    // every frame, and makes it anew on post frames alone); its world isn't
    // drawn, as the next frame, composed with it, draws it. The live view's,
    // which draws frame after frame and keeps each post frame's picture (the
    // GPU in GpuRenderer, the CPU in post_history), for kPostBufferFrames
    // after it. Off (captures, replay: one frame), or with none kept, such a
    // frame draws its own world, post-processed as its parameters say
    // (post_model.h's PlanPost).
    bool post_buffer = false;
    // The world's REFRACT_WORLD draws (RefractsWorld: the title's road) read
    // the pre-process buffer, the last world frame's scene before
    // post-processing: the live view's, which keeps each world frame's scene
    // where it has such a draw (the GPU in GpuRenderer, the CPU in
    // post_history) for the world frames after it (frame_compose.h's
    // PreBufferFor). Off (captures, replay: one frame), or with none kept,
    // the frame's world is drawn kPreBufferPasses times first, the first
    // reading black and each after it the one before's scene, standing in
    // for the frames before it.
    bool pre_buffer = false;
    // with post, if given: bloom's level 0 as the composite read it
    // (post_model.h's RunPost), to check it against the game's
    std::vector<uint32_t>* post_bloom0 = nullptr;
    // the display gamma ramp the frame was shown through (FrameCapture::
    // gamma, gamma_ramp.h), last, over the overlay too, as the screen and the
    // harness's screenshot have it; off, the picture as RB3 drew it. Not
    // applied to the scene target's views.
    bool gamma = true;
    RasterView view = RasterView::kFinal;
    // The texture passes that are pictures of the screen (the spotlights'
    // depth volume and density map, the soft-particle surfaces: PassTargetSize)
    // drawn at their game size times this, so they keep their share of a
    // picture bigger than the game's 1280x720: the presenter's is its height
    // over 720. Every other pass stays the game's size. 1 is the game's.
    float target_scale = 1.0f;
    // the characters' shadow maps (512x512) drawn at this times their size,
    // their taps a texel of that apart (sharper self-shadows); 1 is the game's
    float shadow_scale = 1.0f;
    // the passes RB3 draws without a material, with TheRnd's default one
    // (scene_capture.h's NoMaterial); off, they're left out, as the
    // renderers did before the capture kept them
    bool default_material = true;
    // the overlay's samples per pixel (OverlaySamples): 2, the game's (its
    // D3DMULTISAMPLE_2_SAMPLES offscreen target), 4 smoother than the game,
    // 1 none, as the renderers drew before
    uint32_t msaa = 2;
    // native_fill_window: where the game's 16:9 ends in the picture, in clip
    // x and y (aspect_model.h's OverlayEdge). An overlay draw over the whole
    // picture has its vertices past it moved out to the picture's edge, so
    // menu art drawn a little past 16:9 reaches the window's edge rather
    // than stopping short of it. 1 1 (the picture's edge) leaves them.
    float overlay_edge[2] = {1.0f, 1.0f};
    // With overlay_edge, the song list's (aspect_model.h's kSongListCut): an
    // overlay draw over the whole picture lying wholly between these clip y
    // (bottom, top; in the game's 16:9 frame) is cut at the game's 16:9 across
    // rather than stretched, as a 16:9 screen cuts it (InOverlayCut). 0 0 none.
    float overlay_cut[2] = {0.0f, 0.0f};
    // On the GPU, what each indexed draw of the frame is (mesh, target,
    // counts, textures and their samplers), kept while the GPU draws it
    // (GpuRenderer::DescribeIndexedDraw): with the dred setting, so a GPU
    // hang's report (crash_trace.cpp) names the draw the GPU stopped at.
    // Not labels in the command list: SDL's go through WinPixEventRuntime.dll,
    // which band3 doesn't ship, so they'd never reach DRED. The CPU ignores it.
    bool gpu_labels = false;
    // On the GPU, a frame drawn into the presenter's output
    // (GpuRenderer::RenderFrameToOutput) is submitted and left to the GPU,
    // its fence for the caller to wait out (OutputDone); off, it's waited for
    // before RenderFrameToOutput returns, as it was before the pipeline.
    // native_present_pipeline's alone: left to the GPU with the worker
    // waiting for it at once, the debug layer saw the SDK's command lists go
    // wrong (a barrier out of step, a list executed still open) and AMD GPUs
    // hung, which waiting inside didn't.
    bool gpu_no_wait = false;
    // On the GPU (native_world_ahead), a composed post frame whose world was
    // drawn ahead, by GpuRenderer::RenderWorldAhead just before it, post-
    // processes that scene rather than drawing the world again. The CPU
    // ignores it.
    bool world_ahead = false;
    // On the GPU (native_gpu_timestamps), Direct3D 12 only: timestamps
    // between the frame's parts, into GpuStats::gpu_ms once the GPU has
    // finished it (gpu_timing_model.h). The CPU ignores it.
    bool gpu_timestamps = false;
    // On the GPU (native_view_inline_mips), Direct3D 12 only: a texture
    // pass's mips drawn at the end of the command buffer the frame submits
    // there, as SDL's GenerateMipmaps makes them (gpu_view.cpp's mips.hlsl);
    // off, SDL's own, in a command buffer of their own between that one and
    // the next. The CPU ignores it.
    bool inline_mips = true;
    // On the GPU (native_bc_textures), block-compressed textures (DXT1,
    // DXT2_3, DXT4_5, DXN) are kept as blocks, sent and sampled as BC1, BC2,
    // BC3 and BC5 where the device has those formats, and the capture's
    // decode keeps them so (deferred_decode.h's SetKeepBlocks); off, as RGBA.
    // Read as each texture is placed, so one keeps the way it was placed. The
    // CPU ignores it (it decodes any kept as blocks: EnsureRgba).
    bool bc_textures = false;
    // On the GPU, the most frames apart the world is drawn now (the live
    // view's: frame_pacing.h's WorldPeriod), which geometry and textures
    // drawn in one frame are kept for (gpu_view.h's ResidencyKeepFrames), so
    // the world's, drawn by one frame a period, aren't sent again each time.
    // 0 lets them go once a frame doesn't draw them. The CPU ignores it.
    uint32_t world_period = 0;
    // On the GPU, the game is in a song (native_view.h's InSong): meshes and
    // textures drawn in it are kept by the clock as well as by kEvictAfter
    // frames (gpu_view.h's residency, ClockKeep). Off, by the frames alone.
    // The CPU ignores it.
    bool clock_keep = false;
};

// whether the renderers draw d, as far as the options say
// (RasterOptions::default_material)
inline bool DrawnByOptions(const FrameCapture& f, const DrawItem& d, const RasterOptions& o) {
    if (o.default_material) return true;
    const ShadeInputs* s =
        d.shade >= 0 && size_t(d.shade) < f.shades.size() ? &f.shades[d.shade] : nullptr;
    return !NoMaterial(d, s);
}

// A frame's overlay's samples per pixel: RasterOptions::msaa (2 or 4; any
// other is 1) for a picture (RasterView::kFinal), 1 for a view of the scene
// target, which ends at the resolve
inline uint32_t OverlaySamples(const RasterOptions& o) {
    if (o.view != RasterView::kFinal) return 1;
    return o.msaa == 2 || o.msaa == 4 ? o.msaa : 1;
}

struct RasterStats {
    uint32_t draws = 0;
    uint32_t triangles = 0;
    uint32_t pixels = 0;
    uint32_t passes = 0;      // texture passes drawn
    uint32_t rt_missing = 0;  // draws that sampled a render target nothing had drawn
    double ms = 0;
};

// rgba is width * height RGBA8, R in the low byte; ids, if given, which of
// frame.draws last wrote each pixel (-1 none), to find what drew something
RasterStats Rasterize(const FrameCapture& frame, const RasterOptions& options,
                      std::vector<uint32_t>& rgba, std::vector<int32_t>* ids = nullptr);

// One stretch of a frame's draws as both renderers run them, in order: into
// the back buffer (pass null, those of them DrawnToBackBuffer), or a texture
// pass (those DrawnInTexturePass).
struct PassRun {
    const Pass* pass;
    uint32_t first, end;
};
// The frame's back-buffer stretches, and the texture passes that something
// drawn after them samples (by texture, any version: a pass that clears hides
// the ones before it), as its diffuse texture, as its projected light's map
// (ProjectedTargetOf), with normal_maps as its normal or detail map
// (MapTargetOf) or, with self_shadow, as its shadow map (ShadowMapOf),
// but none from post-processing on, which isn't
// drawn yet, other than the spotlights' (the depth volume's cones and blurs,
// and the density map its cones read: spot_model.h), the soft particles' (the
// particles into the first surface, its blur into the second and back),
// which the composite's terms sample where they're on (post_model.h's
// PlanPost), and shadow maps (for a character in the overlay). A pass whose
// version is one past what a draw before it samples, where no pass made that
// one (a texture drawn every frame, whose last frame's pass the capture
// didn't record), is drawn first, for that draw. A capture without passes is
// one back-buffer stretch.
std::vector<PassRun> PlanPasses(const FrameCapture& frame, const RasterOptions& options);

// Whether pass p's draw is RndSoftParticleBuffer::BlurSurface's: a DrawRect
// blur (shader 1) into one of its surfaces (PostConsts::soft_surface) from
// the other, with the taps its shade state kept (c31.. their uv offsets,
// c47.. their weights): across ([0] into [1]), then down ([1] back into
// [0]), at -1.5..2.5 texels along and 0.5 across, weights .1 .25 .3 .25 .1
// (out/research/softparticle_survey.md 1). The taps fall between texels,
// so they're bilinear, and the two passes move the buffer a texel right and
// down, as the game's do. The renderers draw it from the source surface's
// target with the taps, rather than as a quad sampling it.
inline constexpr int kSoftBlurTaps = 5;
bool SoftBlur(const FrameCapture& frame, const DrawItem& d, const ShadeInputs* state,
              const Pass& p);

// The colour the back buffer starts as, RGBA8: TheRnd's clear colour
// (FrameCapture::clear_color), or 0xff202020 in a capture from before it
uint32_t ClearRgba(const FrameCapture& frame);

// The depth a back-buffer draw's pixels test and write, in the renderers'
// units of 1/w (larger nearer, 0 cleared): (p w + q + r z) / w, w and z its
// clip w and z, which runs straight across the screen as 1/w does. The
// default, 0 1 0, is 1/w itself.
struct DepthMap {
    float p = 0, q = 1, r = 0;
    bool Identity() const { return p == 0 && q == 1 && r == 0; }
};

// How the back buffer's draws are placed when the capture has its cameras
// (FrameCapture::cameras, scene_capture.h's CameraView): each mesh draw in
// its camera's viewport, and no depth clear between cameras, which RB3
// layers by their z ranges instead (the world's camera 0.1..1; a song's
// track cameras 0..1 and its 0..0.1 camera in front of them); but the
// overlay's depth starts cleared after the resolve, as DxRnd::DoPostProcess
// clears its offscreen target's (BeginTiling, depth 0). The device's depth
// for a camera with z range (z0, z1) is d = 1 - z0 - (z1 - z0) z/w; the
// renderers keep d mapped so that a reference camera's draws (the one that
// drew the most of the world, before post-processing) have 1/w as before,
// d = B + A/w for it, which post-processing and the spotlights read the
// depth as: every draw's depth is (d - B) / A (its DepthMap), in the same
// order as RB3's. A camera whose projection has z = a w + b (any
// perspective one) maps as p = (B' - B) / A, q = A' / A with its own A' and
// B'; another (oblique, orthographic) by its clip z. A DrawRect quad, which
// RB3 draws at the device's depth 1 with the viewport off, is (1 - B) / A,
// in the whole picture. Without cameras, or without a perspective reference
// camera, every draw is 1/w (DepthMap's default) and in the whole picture.
struct BackBufferLayout {
    bool cameras = false;  // the capture has them: no depth clear per camera
    bool mapped = false;   // a reference camera: depths are mapped by its A, B
    float ref_a = 1, ref_b = 0;
    float ref_zrange[2] = {0, 1};
    float ref_proj[2] = {0, 0};  // its z = a w + b's a and b
};
BackBufferLayout LayoutBackBuffer(const FrameCapture& frame);

// a back-buffer draw's viewport in a width x height picture (x, y, w, h)
// and its depth, as `layout` places it
void PlaceBackBufferDraw(const BackBufferLayout& layout, const FrameCapture& frame,
                         const DrawItem& d, uint32_t width, uint32_t height, float viewport[4],
                         DepthMap& depth);

// The size both renderers draw texture pass p's target at: the game's
// (Pass::width, height), but the passes that are pictures of the screen, the
// spotlights' depth volume and density map (kTexTypeDepthVolume,
// kTexTypeDensityMap) and the soft-particle surfaces (PostConsts::
// soft_surface), at that times options.target_scale, and the shadow maps
// times options.shadow_scale, each side rounded to an even number. Their
// draws' viewports (Pass::viewport, in the game's texels) scale with them
// (ScalePassViewport); what reads them reads them by uv (the composite, the
// cones' density, the blurs' taps: uv offsets the game set for its size, so
// a blur covers as much of the screen at any size), but for a shadow map's
// half-texel offset (shade_model.h's RescaleShadowCoord). NgLight's shadow
// (ShadowCasterPass) stays 256x256: it's the light's picture, not the
// screen's, and its two blurs leave nothing finer than its texels.
void PassTargetSize(const FrameCapture& frame, const Pass& p, const RasterOptions& options,
                    uint32_t& width, uint32_t& height);
// pass p's camera viewport (x, y, w, h) in a target of w x h rather than its
// own size
inline void ScalePassViewport(const Pass& p, uint32_t w, uint32_t h, float vp[4]) {
    for (int i = 0; i < 4; i++) vp[i] = p.viewport[i];
    if (w == p.width && h == p.height) return;
    const float sx = float(w) / float(p.width), sy = float(h) / float(p.height);
    vp[0] *= sx;
    vp[1] *= sy;
    vp[2] *= sx;
    vp[3] *= sy;
}

// A texture pass's blur (spot::SpotBlur's, SoftBlur's) into a target drawn
// at w x h, bigger than the game's (PassTargetSize). The game's taps are uv
// offsets a texel or a half apart, each reading a texel or the mean of two;
// at the same uv in a bigger target they'd read single texels with others
// between them left out, combing whatever is finer than the game's texels
// (a cone's edge against the scene). So each tap is instead the mean of
// `count` samples `step` (uv) apart along the taps' line, spread over one of
// the game's texels: the game's tap over the texels it covers. count 1 at
// the game's size (or smaller).
struct BlurSubTaps {
    uint32_t count = 1;
    float step[2] = {0, 0};
};
BlurSubTaps BlurSubTapsFor(const ShadeInputs& state, int taps, const Pass& p, uint32_t w,
                           uint32_t h);

// a texture pass's draws but FinishDrawTarget's mip downsamples: the
// renderers make mips themselves, or sample level 0
inline bool DrawnInTexturePass(const DrawItem& d) { return d.mip_level == 0; }

// Whether pass p is NgLight::RenderShadows' pass of its shadow casters (its
// draws in draw mode 3): no camera selects it, so the capture has no clear
// for it, but SetAndClearShadowViewport clears it to transparent black
// (PassClearFlags) before the casters draw their silhouettes into it, opaque
// white (shade_model.cpp's PackShade), untextured (SamplesDiffuse), culled
// and blended as each says
bool ShadowCasterPass(const FrameCapture& frame, const Pass& p);
// the D3DCLEAR bits pass p starts with: its camera's, or a shadow caster
// pass's colour clear (Pass::clear_color is then 0, transparent black)
inline uint32_t PassClearFlags(const FrameCapture& frame, const Pass& p) {
    return p.clear_flags | (ShadowCasterPass(frame, p) ? 0x0fu : 0u);
}
// whether a draw samples its diffuse texture: not a shadow caster, whose
// shader has no DIFFUSE_MAP though its material has a texture
inline bool SamplesDiffuse(const DrawItem& d) { return d.draw_mode != kDrawModeShadowCasters; }

// a texture that texture passes draw, which a renderer samples from its own
// target
inline bool IsPassTarget(const Texture* t) {
    return t && t->tex_obj && IsPassTargetType(t->tex_type);
}

// Whether a world draw writes the scene target's alpha. RB3 writes the back
// buffer's alpha only with a PSEUDO_HDR shader (its bloom weight) or a
// material that asks (alpha_write; SetColorWriteMask, rb3-xenon
// rndobj/Shader.cpp), and blends it apart from the colour, ONE ONE MAX
// (DxRnd::SetDefaultRenderStates, Mat_NG.cpp's SetBasicState): a draw that
// blends leaves the larger of its alpha and what's there, one that doesn't
// (Src) its own. Into a texture its alpha is written always (Offscreen); the
// renderers blend that by the colour's factors.
inline bool WritesSceneAlpha(const ShadeState* s) {
    return s && (s->Option(shader_opt::kPseudoHdr) || s->alpha_write);
}

// Whether a draw's shader is REFRACT_WORLD (option bit 46; the score box's
// glass, the title's road): its texture (or its lit colour) times the
// picture behind it, where its refract normal map moves it (shade.hlsli's
// RefractUv). That's DxRnd::GetCurrentFrameTex (rb3-xenon Rnd_Xbox.cpp:632,
// bound by Mat_NG.cpp:324). Drawn over the overlay, it's PostProcessTexture,
// the picture as DoPostProcess left it (SavePostBuffer) before the overlay's
// draws, which the renderers keep a copy of for it. Drawn in the world,
// before post-processing, it's PreProcessTexture, not resolved then (outside
// HiResScreen): what SavePreBuffer left in it at the last world frame's end,
// that frame's scene. So the title's road is a feedback loop, each frame's
// the lit colour times the last's, which settles dark and reflective
// (out/research/n5_title_street.md). The renderers keep it frame after frame
// where they draw frame after frame (RasterOptions::pre_buffer), else draw
// the world kPreBufferPasses times first.
inline bool RefractsWorld(const ShadeState* s) {
    return s && s->Option(shader_opt::kRefractWorld);
}

// whether any of the frame's world draws (before post_boundary, into the
// back buffer) is REFRACT_WORLD
bool WorldRefracts(const FrameCapture& frame);

// The world passes drawn before a frame whose world refracts, where no
// pre-process buffer is kept for it (RasterOptions::pre_buffer): the first
// reads black, each after it the one before's scene, and the frame the
// last's. Two put the title's road (out/n1/f/title, replay's --crop
// 560,500,340,220) at a mean of 1.1 of 255 against the game, where the
// loop has all but settled (1.2 after one or after six); black alone leaves
// it 1.8, darker, and no picture behind at all 14.1, orange.
inline constexpr int kPreBufferPasses = 2;

// the renderers' depth, kNearW / w (w the clip w, larger is nearer, 0 where
// nothing drew), as RasterView::kSceneDepth shows it: grey falling off with
// log2 of the distance, white to w 16, black from w 4096 and where nothing
// drew (a venue's cameras see from about 100 to a few thousand)
inline constexpr float kNearW = 1e-3f;
inline float DepthViewGrey(float depth) {
    if (!(depth > 0)) return 0;
    const float w = kNearW / depth;
    const float g = 1.0f - (std::log2(std::max(w, 1.0f)) - 4.0f) / 8.0f;
    return std::clamp(g, 0.0f, 1.0f);
}

// RndTex::Type's kRenderedNoZ bit: the texture has no depth buffer
inline constexpr uint32_t kTexTypeNoZ = 0x20;

// D3DCOLOR (ARGB) as RGBA8, R in the low byte
inline uint32_t ArgbToRgba(uint32_t c) {
    return (c >> 16 & 0xff) | (c & 0xff00) | (c & 0xff) << 16 | (c & 0xff000000u);
}

// Draws `frame` on the CPU as Rasterize() does up to the pass that makes
// `version` of the texture `tex_obj` (0: to the frame's end, its last), and
// gives back what that texture's target holds then (RGBA8, the pass's size,
// alpha kept; a shadow map's depth as opaque grey, white at its near plane
// to black at its far one): what native_view_replay's --dump-rt shows. False
// if no pass in the capture draws it.
bool RasterizeTarget(const FrameCapture& frame, const RasterOptions& options, uint32_t tex_obj,
                     uint32_t version, std::vector<uint32_t>& rgba, uint32_t& width,
                     uint32_t& height, RasterStats* stats = nullptr);

}  // namespace band3::render
