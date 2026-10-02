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
// resolve left it. The display's gamma ramp (gamma_ramp.h) goes over the
// finished picture.

namespace band3::render {

// What a renderer hands back: the picture, or, to check the scene target, its
// alpha or its depth as grey (DepthViewGrey) where the world's draws left them
// at post_boundary, without the overlay
enum class RasterView { kFinal, kSceneAlpha, kSceneDepth };

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
    // with post, if given: bloom's level 0 as the composite read it
    // (post_model.h's RunPost), to check it against the game's
    std::vector<uint32_t>* post_bloom0 = nullptr;
    // the display gamma ramp the frame was shown through (FrameCapture::
    // gamma, gamma_ramp.h), last, over the overlay too, as the screen and the
    // harness's screenshot have it; off, the picture as RB3 drew it. Not
    // applied to the scene target's views.
    bool gamma = true;
    RasterView view = RasterView::kFinal;
};

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
// PlanPost), and shadow maps (for a character in the overlay). A capture
// without passes is one back-buffer stretch.
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

// Whether a draw's shader is REFRACT_WORLD (option bit 46, which shader_opt
// doesn't name; the score box's glass): its texture times the picture behind
// it. Drawn over the overlay, that's DxRnd::GetCurrentFrameTex's
// PostProcessTexture, the picture as DoPostProcess left it (SavePostBuffer)
// before the overlay's draws, which the renderers keep a copy of for it.
inline bool RefractsWorld(const ShadeState* s) { return s && ((s->options >> 46) & 1); }

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
