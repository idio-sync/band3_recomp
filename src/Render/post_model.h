#pragma once

#include <cstdint>
#include <vector>

#include "src/Render/scene_capture.h"
#include "src/Render/shade_model.h"

// RB3's post-processing (NgPostProc, NgDOFProc at DxRnd::DoPostProcess) for
// the native view, from a frame's PostParams and PostConsts, run on the scene
// target before the overlay's draws:
//
//   depth of field: the scene 4x smaller (D0), blurred by 8 Poisson taps
//     across, then 8 others down (NgDOFProc::DoPost);
//   bloom: the bright pass 4x smaller (L0), blurred by a 15-tap Gaussian
//     across then down; L1 and L2 each 4x smaller again (NgPostProc::
//     DoBloom); glare frames have L0 only, then its glare pass
//     (kBloomGlareShader, post_model.hlsli's Glare*);
//   camera motion blur: a half-size velocity texture from the depth and the
//     previous camera (RndVelocityBuffer::Draw), with the meshes RB3 gives
//     their own motion drawn over it (its object pass, DrawMesh); the
//     composite blurs the scene along it (post_model.hlsli's Velocity*);
//   the composite: scene lerped toward D0 by depth, soft particles added,
//     bloom screen-blended (or glare added), the spotlights' depth volume
//     added (spot_model.h), film grain from FrameCapture::noise_map, the
//     colour matrix. The soft-particle and spotlight buffers are drawn as
//     texture passes before it.
// Levels are 8-bit like the 360's targets: RGBA8 on the GPU, rounded to 8
// bits (halfway down, as the GPU does) on the CPU. Both backends share
// PlanPost's numbers and post_model.hlsli's per-pixel maths.

namespace band3::render::post {

using shade::float4;
using shade::uint;
using shade::uint4;

#include "src/Render/shaders/post_params.hlsli"

static_assert(sizeof(PostPass) == 40 * 16, "PostPass is float4s and uint4s only, as HLSL packs it");

// The 360's back buffer, which the game's blur taps are in texels of. The
// native view's levels are its own size over 4, 16 and 64 and take those
// offsets as uv, so a blur covers as much of the picture at any size.
inline constexpr uint32_t kGameWidth = 1280;
inline constexpr uint32_t kGameHeight = 720;

inline uint32_t Quarter(uint32_t size) { return size >= 4 ? size / 4 : 1; }

// SetBloomBlurWeights's 15 taps (rb3-xenon Utl.cpp): offsets -6.5..7.5 texels
// of a level `size` texels across (or down), as uv
inline constexpr float kBloomWeights[15] = {
    0.0159283932f, 0.0270778369f, 0.0424231887f, 0.0612547919f, 0.0815124959f,
    0.0999667868f, 0.1129886061f, 0.1176957935f, 0.1129886061f, 0.0999667868f,
    0.0815124959f, 0.0612547919f, 0.0424231887f, 0.0270778369f, 0.0159283932f};
void BloomTaps(bool vertical, uint32_t size, float4 taps[15]);

// SetVHBlurWeights's 8 taps (rb3-xenon DOFProc_NG.cpp): a Poisson disc per
// direction scaled by the blur width scale times sDOFWidthFactor, as uv,
// each weighing 1/8
inline constexpr float kDofWidthFactor = 0.666f;
void DofTaps(bool vertical, float width_scale, float4 taps[8]);

// What a frame's post-processing does. spot_volume, spot_density and soft are
// the DxTex the spotlight and soft-particle passes drew (0 none); noise is
// FrameCapture::noise_map, null without noise.
struct PostPlan {
    PostPass composite;
    float4 dof_taps[2][8];        // across, then down
    float4 bloom_taps[3][2][15];  // each level's, across then down
    uint32_t spot_volume = 0, spot_density = 0;
    uint32_t soft = 0;
    const Texture* noise = nullptr;
    // a post frame's composite, which the game resolved into its post
    // buffer: kept as the trails' previous frame
    bool trails_update = false;
    // the motion blur's object pass, drawn in order over the camera pass
    // (mesh.z, the GPU's bones, is the renderer's to fill in)
    std::vector<const VelocityObject*> velocity_objects;
    std::vector<VelocityObjectPass> velocity_object_passes;
};

// What a renderer drawing frame after frame keeps between frames (a capture
// has none). All RGBA8, R low; a frame number of 0 means none.
struct PostHistory {
    // the post buffer the trails read (s14): the last post frame's
    // composite, alpha CompositeAlpha's (or 1 where the trail was kept)
    std::vector<uint32_t> rgba;
    uint32_t w = 0, h = 0;
    uint64_t game_frame = 0;
    // the post buffer as the screen shows it (RasterOptions::post_buffer):
    // the last post frame's picture before its overlay, alpha 0xff
    std::vector<uint32_t> picture;
    uint32_t picture_w = 0, picture_h = 0;
    uint64_t picture_frame = 0;
    // the pre-process buffer (DxRnd's SavePreBuffer, RasterOptions::
    // pre_buffer): the last world frame's scene before post-processing,
    // alpha the bloom weight; kept where that world has a REFRACT_WORLD
    // draw, for the next one to read
    std::vector<uint32_t> pre;
    uint32_t pre_w = 0, pre_h = 0;
    uint64_t pre_frame = 0;
};

// The frame's PostPlan, false if it post-processes nothing (DoPostProcess
// didn't run or ran disabled, FinishPostProcess didn't, or no effect is on).
// A post frame takes its effects and constants from PostConsts; a world-only
// frame works them out from its proc's PostParams, as the game would on the
// next frame. Each effect also needs what it reads: the spotlights a depth
// volume pass, the soft particles the pass that drew soft_surface[0], the
// noise a kept map (world frames get seeds from the frame number, the game's
// being random), the motion blur the velocity buffer's cameras. Trails need
// the previous post frame, which only RunPost's history has. `noise` and
// `velocity` false, or `only` (kPost bits, 0 all), leave effects off.
bool PlanPost(const FrameCapture& frame, uint32_t only, PostPlan& plan, bool noise = true,
              bool velocity = true);

// A texture the composite reads besides the scene's levels, RGBA8 (R low);
// read as 0 if px is null
struct PostImage {
    const uint32_t* px = nullptr;
    uint32_t w = 0, h = 0;
};

// The post-processed picture on the CPU. `scene` is RGBA8 (R low, alpha the
// bloom weight), `depth` 1/w (0 where nothing drew); `volume`, `density` and
// `soft` are what the plan's spot_volume, spot_density and soft passes drew;
// `out` is RGBA8, alpha 0xff. `bloom0` gets bloom's level 0 after glare
// (empty without bloom or glare). The trails read `history` where it's this
// size and from an earlier frame; a post frame's composite goes into it as
// `game_frame`.
void RunPost(const PostPlan& plan, const std::vector<uint32_t>& scene,
             const std::vector<float>& depth, uint32_t width, uint32_t height,
             const PostImage& volume, const PostImage& density, const PostImage& soft,
             std::vector<uint32_t>& out, std::vector<uint32_t>* bloom0 = nullptr,
             PostHistory* history = nullptr, uint64_t game_frame = 0);

// post_model.hlsli's functions on the CPU, for the tests
float GameDepthCpu(const PostPass& pass, float inv_w);
float DofAmountCpu(const float c24[4], float depth);
void CompositeCpu(const PostPass& pass, const float scene[4], const float dof[4], float depth,
                  const float l0[3], const float l1[3], const float l2[3], const float volume[3],
                  float density, const float soft[3], const float noise0[3],
                  const float noise1[3], float out[3]);
// unrounded
void VelocityTexelCpu(const PostPass& pass, const float uv[2], float inv_w, float out[4]);
void VelocityObjectTexelCpu(const VelocityObjectPass& pass, const float cur[4],
                            const float prev[4], float inv_w, float out[4]);
// a vertex's current and previous clip positions by o's palettes
void VelocityObjectVertexCpu(const VelocityObject& o, const VelocityObjectPass& pass,
                             const Vertex& v, float cur[4], float prev[4]);
// `tap` reads the scene at a uv; `centre` is the unblurred scene at uv
void VelocityBlurCpu(const PostPass& pass, const float uv[2], const float velocity[4],
                     const float centre[4], void (*tap)(const float at[2], float out[4]),
                     float out[4]);
// `rgb` unsaturated, `prev` the previous post frame's texel (0..1)
void TrailsCpu(const PostPass& pass, const float rgb[3], const float prev[4], float out[4]);
// NoiseUv, NoiseDx, NoiseDy for tap 0 or 1
void NoiseTapCpu(const PostPass& pass, const float uv[2], int tap, float at[2], float dx[2],
                 float dy[2]);

}  // namespace band3::render::post
