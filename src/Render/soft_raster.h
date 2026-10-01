#pragma once

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
// against the clear's 0, and outfit layers blend by it. Mips aren't sampled:
// every texture is read nearest at level 0, on the GPU too.

namespace band3::render {

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
    bool clear_depth_per_camera = true;
    // the texture passes the frame samples drawn natively, and sampled; off,
    // a render target is what guest memory held of it, or nothing
    bool texture_passes = true;
    // a render target no pass has drawn samples guest memory's pixels where
    // the capture kept them (native_view_rt_fallback guest), else
    // transparent black; off, they're never used (texture_passes off then
    // draws render targets untextured)
    bool rt_guest_pixels = true;
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
// the ones before it), but none from post-processing on, which isn't drawn
// yet. A capture without passes is one back-buffer stretch.
std::vector<PassRun> PlanPasses(const FrameCapture& frame, const RasterOptions& options);

// a texture pass's draws but FinishDrawTarget's mip downsamples: the
// renderers make mips themselves, or sample level 0
inline bool DrawnInTexturePass(const DrawItem& d) { return d.mip_level == 0; }

// a texture that texture passes draw, which a renderer samples from its own
// target
inline bool IsPassTarget(const Texture* t) {
    return t && t->tex_obj && IsPassTargetType(t->tex_type);
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
// alpha kept): what native_view_replay's --dump-rt shows. False if no pass in
// the capture draws it.
bool RasterizeTarget(const FrameCapture& frame, const RasterOptions& options, uint32_t tex_obj,
                     uint32_t version, std::vector<uint32_t>& rgba, uint32_t& width,
                     uint32_t& height, RasterStats* stats = nullptr);

}  // namespace band3::render
