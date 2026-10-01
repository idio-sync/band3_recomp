#pragma once

#include <cstdint>
#include <vector>

#include "src/Render/scene_capture.h"
#include "src/Render/shade_model.h"

// Experimental: RB3's post-processing for the native view, from a frame's
// PostParams and PostConsts (post_params.h): what the game's NgPostProc and
// NgDOFProc draw at DxRnd::DoPostProcess, on the scene target the world's
// draws left (soft_raster.h), before the overlay's draws.
//
//   depth of field: the scene 4x smaller (D0), blurred by 8 Poisson taps
//     across, then 8 others down (NgDOFProc::DoPost);
//   bloom: the scene 4x smaller, each texel weighted by its alpha (the bright
//     pass, L0), blurred by a 15-tap Gaussian across then down; L0 4x smaller
//     again (L1) and blurred, and L1 (L2) (NgPostProc::DoBloom); glare frames
//     have L0 only (and a glare pass after its blur, whose shader isn't known:
//     left out);
//   the composite: the scene lerped toward D0 by the depth, bloom
//     screen-blended (or glare added), the colour matrix
//     (shaders/post_model.hlsli).
// The levels are 8-bit, as the 360's render targets are; the GPU's are RGBA8
// and the CPU rounds each pass's output to 8 bits likewise. Both backends run
// the same passes with the same numbers: PlanPost's PostPass and the taps
// here, the per-pixel maths in post_model.hlsli. The composite's values come
// from PostConsts (what the game's composite drew with): a frame without them
// drew no post-processing. The camera and the blurs' widths are PostParams'.

namespace band3::render::post {

using shade::float4;
using shade::uint;
using shade::uint4;

#include "src/Render/shaders/post_params.hlsli"

static_assert(sizeof(PostPass) == 25 * 16, "PostPass is float4s and uint4s only, as HLSL packs it");

// The 360's back buffer, 1280x720: RB3's post-processing sizes its levels by
// it, and the blurs' taps are offsets in its levels' texels. The native view's
// levels are its own size over 4, 16 and 64, and take the game's offsets in
// uv, so a blur covers as much of the picture at any size.
inline constexpr uint32_t kGameWidth = 1280;
inline constexpr uint32_t kGameHeight = 720;

// RasterOptions::post_only's bit for the composite's spotlight term
// (spot_model.h, PostConsts::spot_flag), which isn't drawn yet: alone, it
// leaves every other effect off
inline constexpr uint kPostSpot = 16u;

// a level 4x smaller than `size`, as RB3 makes them (integer, at least 1)
inline uint32_t Quarter(uint32_t size) { return size >= 4 ? size / 4 : 1; }

// SetBloomBlurWeights's 15 taps (rb3-xenon Utl.cpp): offsets -6.5..7.5 texels
// of a level `size` texels across (or down), as uv, and their weights
inline constexpr float kBloomWeights[15] = {
    0.0159283932f, 0.0270778369f, 0.0424231887f, 0.0612547919f, 0.0815124959f,
    0.0999667868f, 0.1129886061f, 0.1176957935f, 0.1129886061f, 0.0999667868f,
    0.0815124959f, 0.0612547919f, 0.0424231887f, 0.0270778369f, 0.0159283932f};
void BloomTaps(bool vertical, uint32_t size, float4 taps[15]);

// SetVHBlurWeights's 8 taps (rb3-xenon DOFProc_NG.cpp): a Poisson disc of
// its own for each direction, scaled to the DOF level of the game's size by
// RndPostProc's blur width scale times sDOFWidthFactor (0.666), as uv, each
// weighing 1/8
inline constexpr float kDofWidthFactor = 0.666f;
void DofTaps(bool vertical, float width_scale, float4 taps[8]);

// What a frame's post-processing does: the composite's PostPass (flags, c6,
// c24, the colour matrix, the world camera), and the blurs' taps
struct PostPlan {
    PostPass composite;
    float4 dof_taps[2][8];        // across, then down
    float4 bloom_taps[3][2][15];  // each level's, across then down
};

// The frame's PostPlan, false if it post-processes nothing: its DoPostProcess
// didn't run, or ran disabled, or its FinishPostProcess didn't (a world frame
// with even/odd rendering), or the composite had no effect on (it's then a
// copy). With `only` (kPost bits, 0 all) the effects outside it are left off,
// to see each on its own.
bool PlanPost(const FrameCapture& frame, uint32_t only, PostPlan& plan);

// The post-processed picture, on the CPU: `scene` the world's draws (RGBA8, R
// low, alpha the bloom weight) and `depth` theirs (1/w, 0 where nothing
// drew), width x height; `out` RGBA8, alpha 0xff.
void RunPost(const PostPlan& plan, const std::vector<uint32_t>& scene,
             const std::vector<float>& depth, uint32_t width, uint32_t height,
             std::vector<uint32_t>& out);

// post_model.hlsli's functions on the CPU, for the tests
float GameDepthCpu(const PostPass& pass, float inv_w);
float DofAmountCpu(const float c24[4], float depth);
void CompositeCpu(const PostPass& pass, const float scene[4], const float dof[4], float depth,
                  const float l0[3], const float l1[3], const float l2[3], float out[3]);

}  // namespace band3::render::post
