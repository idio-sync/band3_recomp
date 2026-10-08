#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "src/Render/scene_capture.h"

// Draws a FrameCapture on the CPU, a reference for the GPU backend that tests
// the captured data rather than a graphics API.
//
// Texture passes (RasterOptions::texture_passes) are drawn in RB3's order,
// each into its own target (by DxTex) kept for the frame; a draw sampling a
// render target sees what its passes have drawn so far, which gets versions
// right (the crowd impostor is redrawn eight times a frame). Texture targets
// keep alpha: impostors are alpha-cut against the clear's 0, outfit layers
// blend by it. Render target mips are made after their pass (BuildMips).
// Spotlight cones shade by spot_model.h from the world's depth; the depth
// volume's blurs run in place; soft particles fade by that depth and their
// blurs ping-pong between surfaces. A shadow map pass (kTexTypeShadowMap)
// draws depth only (z/w less-than, cleared to 1) into a float buffer that
// later SHADOW_BUFFER draws read. NgLight's shadow (ProjectedTargetOf): caster
// silhouettes (ShadowCasterPass) blurred twice in place, read as s5.
//
// Back-buffer draws split at post_boundary: the world goes to a scene target
// (alpha kept as RB3's back buffer keeps it, cleared to 0; depth readable),
// resolved into the picture by RB3's post-processing (post_model.h) or a copy
// with alpha made opaque; the overlay goes on top. RefractsWorld overlay
// draws read a copy of the resolve; world ones, the last world frame's scene
// (RasterOptions::pre_buffer). Gamma (gamma_ramp.h) goes last. The back
// buffer starts as ClearRgba; with cameras, draws go in their viewports
// layered by z range (LayoutBackBuffer). DrawRect quads are drawn too, except
// the post copy, which post-processing redoes.
//
// Above 1280x720, passes that are pictures of the screen scale with it
// (RasterOptions::target_scale, PassTargetSize).
//
// The overlay is multisampled as in RB3 (RasterOptions::msaa,
// OverlaySamples): the world goes to a 1x back buffer, everything after
// DoPostProcess to a 2x one that EndTiling resolves as the per-pixel mean.
// Texture passes, the world and its depth stay 1x. Like GPU MSAA: per-sample
// colour and depth at D3D's standard positions (kSamplePositions), shaded
// once at the pixel centre, averaged at the frame's end.

namespace band3::render {

namespace post {
struct PostHistory;
}

// the picture, or the scene target's alpha or depth (DepthViewGrey) at
// post_boundary, without the overlay
enum class RasterView { kFinal, kSceneAlpha, kSceneDepth };

struct RasterOptions;
struct DrawItem;
// whether a full-picture overlay draw lies within RasterOptions::overlay_cut;
// it's then drawn unstretched, only in the 16:9 frame's columns
bool InOverlayCut(const DrawItem& it, const RasterOptions& o);

struct RasterOptions {
    uint32_t width = 640;
    uint32_t height = 360;
    bool textures = true;
    // RB3's lighting (shade_model.h); off draws everything unlit (ambient 1)
    bool lighting = true;
    // the old placeholder light (fixed directional, non-prelit materials),
    // which captures without shade states always get
    bool legacy_light = false;
    bool skinning = true;
    bool blending = true;   // off draws every material opaque
    bool culling = true;    // off draws both sides of every triangle (DrawItem::cull)
    // without FrameCapture::cameras, clear back-buffer depth when each camera
    // first draws; captures with cameras never clear (LayoutBackBuffer)
    bool clear_depth_per_camera = true;
    // off, a render target is what guest memory held of it, or nothing
    bool texture_passes = true;
    // an undrawn render target samples guest memory's pixels where kept
    // (native_view_rt_fallback guest), else transparent black
    bool rt_guest_pixels = true;
    // shadow maps drawn and read by SHADOW_BUFFER draws (ShadowLit); off,
    // those draws are lit
    bool self_shadow = true;
    // NORMAL_MAP/NORM_DETAIL where the capture kept tangents and maps
    // (MappedNormals); off, the vertex normal
    bool normal_maps = true;
    // the game's samplers (TexSampler, sample_model.h); off, nearest at
    // level 0, wrapping
    bool filtering = true;
    // RB3's post-processing at post_boundary (post_model.h): DoF, bloom,
    // colour matrix
    bool post = true;
    // with post, only these effects (post_model.h's kPost bits), 0 all
    uint32_t post_only = 0;
    // with post: film grain, where the capture kept its noise map
    bool grain = true;
    // with post: camera motion blur, where the capture kept the velocity
    // buffer's cameras
    bool velocity = true;
    // with post: trails (blend previous), which need the previous post frame,
    // so live view only (GpuRenderer, or post_history on the CPU). Off for
    // single-frame captures and replay.
    bool trails = false;
    post::PostHistory* post_history = nullptr;
    // A frame with no post-processing of its own under even/odd rendering
    // (ShowsPostBuffer) shows the post buffer kept from the last post frame
    // (up to kPostBufferFrames) under its own overlay, as
    // DxRnd::DoPostProcess copies it to the screen every frame; its world is
    // left to the next, composed frame. Live view only; off, or with none
    // kept, it draws and post-processes its own world.
    bool post_buffer = false;
    // World REFRACT_WORLD draws (the title's road) read the last world frame's
    // pre-post scene, kept by the live view (PreBufferFor). Off, or with none
    // kept, the world is drawn kPreBufferPasses times first, the first
    // reading black.
    bool pre_buffer = false;
    // with post, if given: bloom level 0 as the composite read it (RunPost)
    std::vector<uint32_t>* post_bloom0 = nullptr;
    // the frame's display gamma ramp (gamma_ramp.h), applied last; not to
    // the scene target's views
    bool gamma = true;
    RasterView view = RasterView::kFinal;
    // scale for passes that are pictures of the screen (PassTargetSize), so
    // they keep their share above 1280x720: picture height / 720. Other
    // passes stay game size.
    float target_scale = 1.0f;
    // scale for the 512x512 character shadow maps (sharper self-shadows)
    float shadow_scale = 1.0f;
    // draw the passes RB3 draws with TheRnd's default material (NoMaterial);
    // off, leave them out
    bool default_material = true;
    // overlay samples per pixel (OverlaySamples): 2 is the game's
    // D3DMULTISAMPLE_2_SAMPLES, 4 smoother, 1 none
    uint32_t msaa = 2;
    // native_fill_window: the game's 16:9 edge in clip x and y (OverlayEdge).
    // Full-picture overlay vertices past it move out to the picture's edge,
    // so menu art reaches the window's edge. 1 1 leaves them.
    float overlay_edge[2] = {1.0f, 1.0f};
    // With overlay_edge (kSongListCut): full-picture overlay draws wholly
    // within these clip y (bottom, top; 16:9 frame) are cut at 16:9 instead
    // of stretched (InOverlayCut). 0 0 none.
    float overlay_cut[2] = {0.0f, 0.0f};
    // GPU, dred setting: describe each indexed draw (DescribeIndexedDraw) so
    // a hang report (crash_trace.cpp) names it. Not command-list labels:
    // SDL's need WinPixEventRuntime.dll, which band3 doesn't ship.
    bool gpu_labels = false;
    // GPU, native_present_pipeline only: RenderFrameToOutput submits without
    // waiting; the caller waits on OutputDone. Leaving it to the GPU while the
    // worker waited at once broke the SDK's command lists (debug layer) and
    // hung AMD GPUs; waiting inside didn't.
    bool gpu_no_wait = false;
    // GPU (native_world_ahead): a composed post frame whose world
    // RenderWorldAhead drew just before post-processes that scene instead of
    // redrawing it
    bool world_ahead = false;
    // GPU (native_gpu_timestamps), D3D12 only: per-part timestamps into
    // GpuStats::gpu_ms (gpu_timing_model.h)
    bool gpu_timestamps = false;
    // GPU (native_view_inline_mips), D3D12 only: texture pass mips drawn at
    // the end of the frame's command buffer (mips.hlsl); off, SDL's
    // GenerateMipmaps in a command buffer of their own
    bool inline_mips = true;
    // GPU (native_view_submit_points): extra submits besides each texture
    // pass with mips: 0 none, 1 at the resolve (SubmitAtResolve)
    uint32_t submit_points = 0;
    // GPU (native_bc_textures): DXT1/DXT2_3/DXT4_5/DXN kept as blocks and
    // sampled as BC1/2/3/5 where supported (SetKeepBlocks); off, RGBA. Read
    // when each texture is placed. The CPU decodes blocks (EnsureRgba).
    bool bc_textures = false;
    // GPU (native_r8_textures): k_8 textures (movie planes) kept as bytes and
    // sampled as R8 where supported, swizzled in the shader (mesh.hlsl's
    // MapTexel; SetKeepR8); off, RGBA. Read when each texture is placed.
    bool r8_textures = false;
    // GPU: frames between world draws (WorldPeriod), how long meshes and
    // textures are kept (ResidencyKeepFrames) so the world's aren't resent.
    // 0 lets them go once a frame doesn't draw them.
    uint32_t world_period = 0;
    // GPU, in a song (InSong): also keep meshes and textures by the clock,
    // not only kEvictAfter frames (ClockKeep)
    bool clock_keep = false;
};

inline bool DrawnByOptions(const FrameCapture& f, const DrawItem& d, const RasterOptions& o) {
    if (o.default_material) return true;
    const ShadeInputs* s =
        d.shade >= 0 && size_t(d.shade) < f.shades.size() ? &f.shades[d.shade] : nullptr;
    return !NoMaterial(d, s);
}

// 1 for scene-target views, which end at the resolve
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

// rgba is width * height RGBA8, R in the low byte; ids, if given, the index
// in frame.draws that last wrote each pixel (-1 none)
RasterStats Rasterize(const FrameCapture& frame, const RasterOptions& options,
                      std::vector<uint32_t>& rgba, std::vector<int32_t>* ids = nullptr);

// A run of draws, in order: back buffer (pass null; DrawnToBackBuffer) or a
// texture pass (DrawnInTexturePass).
struct PassRun {
    const Pass* pass;
    uint32_t first, end;
};
// The back-buffer runs plus the texture passes something later samples (by
// texture, any version; a clearing pass hides earlier ones): as diffuse,
// projected light map (ProjectedTargetOf), normal/detail map with
// normal_maps (MapTargetOf), or shadow map with self_shadow (ShadowMapOf).
// From post-processing on, only the spotlights' and soft particles' passes,
// which the composite samples (PlanPost), and shadow maps. A pass one version
// past what an earlier draw samples, with no pass making that version (last
// frame's pass unrecorded), is drawn first for that draw. A capture without
// passes is one back-buffer run.
std::vector<PassRun> PlanPasses(const FrameCapture& frame, const RasterOptions& options);

// Whether pass p's draw is RndSoftParticleBuffer::BlurSurface's DrawRect blur
// (shader 1) between its surfaces (PostConsts::soft_surface): across ([0]
// into [1]), then down ([1] into [0]), taps from the shade state (c31.. uv
// offsets, c47.. weights): -1.5..2.5 texels along, 0.5 across, weights
// .1 .25 .3 .25 .1. Taps are bilinear and the two passes shift the buffer a
// texel right and down, as in the game. Drawn from the source target with the
// taps rather than as a quad.
inline constexpr int kSoftBlurTaps = 5;
bool SoftBlur(const FrameCapture& frame, const DrawItem& d, const ShadeInputs* state,
              const Pass& p);

// RGBA8: TheRnd's clear colour (FrameCapture::clear_color), or 0xff202020 in
// older captures
uint32_t ClearRgba(const FrameCapture& frame);

// A back-buffer draw's depth in the renderers' 1/w units (larger nearer, 0
// cleared): (p w + q + r z) / w from clip w and z, linear in screen space.
// 0 1 0 is 1/w.
struct DepthMap {
    float p = 0, q = 1, r = 0;
    bool Identity() const { return p == 0 && q == 1 && r == 0; }
};

// Back-buffer placement with FrameCapture::cameras: each mesh draw in its
// camera's viewport, no depth clear between cameras (RB3 layers them by z
// range: world 0.1..1, song track cameras 0..1 with a 0..0.1 camera in
// front); overlay depth is cleared after the resolve, as DxRnd::DoPostProcess
// clears its offscreen target (BeginTiling, depth 0). Device depth for z
// range (z0, z1) is d = 1 - z0 - (z1 - z0) z/w. The reference camera (most of
// the world before post) has d = B + A/w; every draw's depth is (d - B) / A
// (its DepthMap), so the reference stays 1/w for post-processing and
// spotlights, and RB3's order is kept. Perspective cameras (z = a w + b) map
// as p = (B' - B) / A, q = A' / A; others (oblique, orthographic) by clip z.
// DrawRect quads (device depth 1, viewport off) are (1 - B) / A, full
// picture. Without cameras or a perspective reference, every draw is 1/w in
// the full picture.
struct BackBufferLayout {
    bool cameras = false;  // no depth clear per camera
    bool mapped = false;   // depths mapped by the reference camera's A, B
    float ref_a = 1, ref_b = 0;
    float ref_zrange[2] = {0, 1};
    float ref_proj[2] = {0, 0};  // a and b of z = a w + b
};
BackBufferLayout LayoutBackBuffer(const FrameCapture& frame);

// viewport is x, y, w, h in a width x height picture
void PlaceBackBufferDraw(const BackBufferLayout& layout, const FrameCapture& frame,
                         const DrawItem& d, uint32_t width, uint32_t height, float viewport[4],
                         DepthMap& depth);

// Texture pass p's target size: the game's (Pass::width, height), times
// target_scale for screen pictures (kTexTypeDepthVolume, kTexTypeDensityMap,
// PostConsts::soft_surface) and shadow_scale for shadow maps, each side
// rounded to even. Viewports scale with them (ScalePassViewport); readers
// use uv, so blurs cover the same screen share at any size, except a shadow
// map's half-texel offset (RescaleShadowCoord). NgLight's shadow
// (ShadowCasterPass) stays 256x256: it isn't a picture of the screen and its
// blurs leave nothing finer than its texels.
void PassTargetSize(const FrameCapture& frame, const Pass& p, const RasterOptions& options,
                    uint32_t& width, uint32_t& height);
// pass p's camera viewport (x, y, w, h) in a w x h target
inline void ScalePassViewport(const Pass& p, uint32_t w, uint32_t h, float vp[4]) {
    for (int i = 0; i < 4; i++) vp[i] = p.viewport[i];
    if (w == p.width && h == p.height) return;
    const float sx = float(w) / float(p.width), sy = float(h) / float(p.height);
    vp[0] *= sx;
    vp[1] *= sy;
    vp[2] *= sx;
    vp[3] *= sy;
}

// A blur (SpotBlur, SoftBlur) into a target bigger than the game's
// (PassTargetSize). The game's uv taps would skip texels in between and comb
// fine detail, so each tap becomes the mean of `count` samples `step` (uv)
// apart along the taps' line, spanning one game texel. count 1 at game size
// or smaller.
struct BlurSubTaps {
    uint32_t count = 1;
    float step[2] = {0, 0};
};
BlurSubTaps BlurSubTapsFor(const ShadeInputs& state, int taps, const Pass& p, uint32_t w,
                           uint32_t h);

// excludes FinishDrawTarget's mip downsamples: the renderers make mips
// themselves
inline bool DrawnInTexturePass(const DrawItem& d) { return d.mip_level == 0; }

// Whether pass p is NgLight::RenderShadows' shadow caster pass (draw mode 3).
// No camera selects it, so the capture has no clear for it, but
// SetAndClearShadowViewport clears it to transparent black (PassClearFlags);
// casters draw opaque white silhouettes (PackShade), untextured
// (SamplesDiffuse)
bool ShadowCasterPass(const FrameCapture& frame, const Pass& p);
// D3DCLEAR bits: the camera's, plus colour for a shadow caster pass
// (Pass::clear_color is then 0)
inline uint32_t PassClearFlags(const FrameCapture& frame, const Pass& p) {
    return p.clear_flags | (ShadowCasterPass(frame, p) ? 0x0fu : 0u);
}
// shadow casters' shader has no DIFFUSE_MAP though the material has a texture
inline bool SamplesDiffuse(const DrawItem& d) { return d.draw_mode != kDrawModeShadowCasters; }

// drawn by texture passes; renderers sample their own target
inline bool IsPassTarget(const Texture* t) {
    return t && t->tex_obj && IsPassTargetType(t->tex_type);
}

// Whether a world draw writes the scene target's alpha: only PSEUDO_HDR
// shaders (bloom weight) or alpha_write materials (SetColorWriteMask,
// rb3-xenon rndobj/Shader.cpp). Alpha blends separately, ONE ONE MAX
// (DxRnd::SetDefaultRenderStates, Mat_NG.cpp's SetBasicState); a
// non-blending draw writes its own. Texture targets always get alpha,
// blended by the colour's factors.
inline bool WritesSceneAlpha(const ShadeState* s) {
    return s && (s->Option(shader_opt::kPseudoHdr) || s->alpha_write);
}

// Whether a draw's shader is REFRACT_WORLD (option bit 46; score box glass,
// the title's road): its colour times the picture behind it, offset by its
// refract normal map (RefractUv). The picture is DxRnd::GetCurrentFrameTex
// (rb3-xenon Rnd_Xbox.cpp:632, bound by Mat_NG.cpp:324): in the overlay,
// PostProcessTexture (SavePostBuffer, before overlay draws); in the world,
// PreProcessTexture, the last world frame's scene from SavePreBuffer. So the
// title's road is a feedback loop that settles dark and reflective
// (out/research/n5_title_street.md); see RasterOptions::pre_buffer.
inline bool RefractsWorld(const ShadeState* s) {
    return s && s->Option(shader_opt::kRefractWorld);
}

// whether any world draw (before post_boundary) is REFRACT_WORLD
bool WorldRefracts(const FrameCapture& frame);

// World passes drawn first without a kept pre-process buffer: the first reads
// black, each later one the previous scene. Two put the title's road within
// a mean of 1.1/255 of the game (out/n1/f/title); black alone is 1.8 off.
inline constexpr int kPreBufferPasses = 2;

// The renderers' depth is kNearW / w (clip w; larger nearer, 0 nothing
// drawn). kSceneDepth shows it as grey by log2 distance: white to w 16,
// black from w 4096.
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

// Rasterizes up to the pass making `version` of `tex_obj` (0: its last) and
// returns that target (RGBA8, pass size, alpha kept; shadow maps as grey
// depth, white near to black far), for native_view_replay's --dump-rt. False
// if no pass draws it.
bool RasterizeTarget(const FrameCapture& frame, const RasterOptions& options, uint32_t tex_obj,
                     uint32_t version, std::vector<uint32_t>& rgba, uint32_t& width,
                     uint32_t& height, RasterStats* stats = nullptr);

}  // namespace band3::render
