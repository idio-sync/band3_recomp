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
//     have L0 only, and its glare pass after the blur (kBloomGlareShader: a
//     ghost of the bright parts mirrored through the centre,
//     post_model.hlsli's Glare*);
//   camera motion blur (velocity blur): a velocity texture half the
//     picture's size each way, each texel how far the picture moved there
//     since the last frame, from the depth and the previous frame's camera
//     (RndVelocityBuffer::Draw), then the meshes RB3 draws there with their
//     own motion (the characters: its object pass, DrawMesh) over that; the
//     composite takes the scene blurred along it, where it moved enough
//     (post_model.hlsli's Velocity*);
//   the composite: the scene lerped toward D0 by the depth, the
//     soft-particle buffer added (RndSoftParticleBuffer's, scene_capture.h's
//     IsSoftParticle), bloom screen-blended (or glare added), the
//     spotlights' depth volume added (spot_model.h: NgSpotlightDrawer's), the
//     noise (film grain: the noise map the capture kept, FrameCapture::
//     noise_map, read twice by the game's random seeds and overlaid), the
//     colour matrix (shaders/post_model.hlsli). The renderers draw the soft
//     particles' and the spotlights' buffers as texture passes before it.
// The levels are 8-bit, as the 360's render targets are; the GPU's are RGBA8
// and the CPU rounds each pass's output to 8 bits likewise (a value halfway
// between two steps to the lower, as the GPU's come out). Both backends run
// the same passes with the same numbers: PlanPost's PostPass and the taps
// here, the per-pixel maths in post_model.hlsli. The composite's values come
// from PostConsts (what the game's composite drew with): a frame without them
// drew no post-processing. The camera and the blurs' widths are PostParams'.

namespace band3::render::post {

using shade::float4;
using shade::uint;
using shade::uint4;

#include "src/Render/shaders/post_params.hlsli"

static_assert(sizeof(PostPass) == 40 * 16, "PostPass is float4s and uint4s only, as HLSL packs it");

// The 360's back buffer, 1280x720: RB3's post-processing sizes its levels by
// it, and the blurs' taps are offsets in its levels' texels. The native view's
// levels are its own size over 4, 16 and 64, and take the game's offsets in
// uv, so a blur covers as much of the picture at any size.
inline constexpr uint32_t kGameWidth = 1280;
inline constexpr uint32_t kGameHeight = 720;

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
// c24, the colour matrix, the world camera, the spotlights' term, the
// noise's), the blurs' taps, the textures the spotlights' term reads: the
// depth volume and the density map the frame's spotlight passes drew, the
// soft-particle surface the composite adds (DxTex, 0 none), and the noise
// map (the frame's FrameCapture::noise_map, null without the noise)
struct PostPlan {
    PostPass composite;
    float4 dof_taps[2][8];        // across, then down
    float4 bloom_taps[3][2][15];  // each level's, across then down
    uint32_t spot_volume = 0, spot_density = 0;
    uint32_t soft = 0;
    const Texture* noise = nullptr;
    // whether the frame's composite is one the game resolved into its post
    // buffer (a post frame's, with its constants): the renderer keeps its
    // output as the next frame's previous, which the trails read
    bool trails_update = false;
    // the motion blur's object pass, drawn over the velocity texture after
    // its camera pass, in order: the frame's VelocityObjects and each one's
    // numbers (its mesh.z, the GPU's bones, the renderer's to fill in)
    std::vector<const VelocityObject*> velocity_objects;
    std::vector<VelocityObjectPass> velocity_object_passes;
};

// The previous post frame the trails read (the post buffer, s14): the last
// post frame's composite, RGBA8 (R low) at the picture's size, its alpha
// the composite's (CompositeAlpha, or the trails' 1 where the trail was
// kept), and the game frame it was (0 none). Only a renderer drawing frame
// after frame has one (RasterOptions::trails): a capture is one frame.
struct PostHistory {
    std::vector<uint32_t> rgba;
    uint32_t w = 0, h = 0;
    uint64_t game_frame = 0;
};

// The frame's PostPlan, false if it post-processes nothing: its DoPostProcess
// didn't run, or ran disabled, or its FinishPostProcess didn't (a world frame
// with even/odd rendering), or the composite had no effect on (it's then a
// copy). The spotlights' term is on where the game's composite had it
// (PostConsts::spot_flag) and the frame has a depth volume's pass to read;
// the soft particles' where it had them (TheShaderMgr + 0x3F) and the frame
// has the pass that drew its particles into PostConsts::soft_surface[0]
// (captures from before it have neither: the term reads 0 there). The
// noise is on where the game's composite had it (TheShaderMgr + 0x2D) and
// the capture kept its map; on a world frame where the proc has it on
// (NgPostProc::CheckNoise's test) and the capture has the last post
// frame's map, with seeds of its own (the game's are random each frame:
// these are the frame number's, or the proc's two when stationary). With
// `noise` false it's left off. The trails are on where the game's composite
// had them (TheShaderMgr + 0x2F, c125), or on a world frame where the proc
// has them (RndPostProc::BlendPrevious), faded by a post frame's time at
// its emulated rate; a renderer without the previous post frame leaves
// them off (a capture's: the term then is the colour alone, as it is in
// steady state for every music-video proc but video_trails).
// The camera motion blur is on where the game's composite had it
// (TheShaderMgr + 0x39, by its c122), or on a world frame where DoVelocity
// would turn it on (post_params.h's VelocityExpected, by the velocity
// buffer's last c122), and the capture has the velocity buffer's cameras
// (PostParams::vel_read: captures from before don't); with `velocity` false
// it's left off. A frame whose game drew the motion blur's object pass
// (FrameCapture::velocity_objects: a post frame's) has it drawn over the
// camera's (PostPlan::velocity_objects).
// With `only` (kPost bits, 0 all) the effects outside it are left off, to see
// each on its own.
bool PlanPost(const FrameCapture& frame, uint32_t only, PostPlan& plan, bool noise = true,
              bool velocity = true);

// A texture the composite reads besides the scene's own levels, RGBA8 (R
// low), w x h; none (read as 0) if px is null
struct PostImage {
    const uint32_t* px = nullptr;
    uint32_t w = 0, h = 0;
};

// The post-processed picture, on the CPU: `scene` the world's draws (RGBA8, R
// low, alpha the bloom weight) and `depth` theirs (1/w, 0 where nothing
// drew), width x height; `volume` and `density` what the frame's spotlight
// passes drew into the plan's spot_volume and spot_density, `soft` what its
// soft-particle passes left in the plan's soft, each read bilinear at each
// pixel's uv; the noise map, the plan's, by its sampler (sample_model.h:
// mip levels by NoiseDx and NoiseDy); `out` RGBA8, alpha 0xff. `bloom0`, if given, gets bloom's level
// 0 as the composite read it (after glare's pass), RGBA8, Quarter(width) x
// Quarter(height), or nothing on a frame without bloom or glare. With
// `history`, the trails read it where it's the picture's size and from an
// earlier frame (else they're left off), and a post frame's composite
// (PostPlan::trails_update) goes into it, as the frame `game_frame`.
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
// the velocity pass's texel at uv over native depth inv_w (VelocityTexel,
// unrounded), and the composite's scene at uv blurred along the velocity
// texel `velocity` (bilinear) through `tap`, which reads the scene at a uv:
// the scene `centre` as it is outside the mask
void VelocityTexelCpu(const PostPass& pass, const float uv[2], float inv_w, float out[4]);
// the object pass's texel from the clip positions cur and prev over native
// depth inv_w (VelocityObjectTexel), and a vertex's two clip positions
// (VelocityObjectWeights, VelocityObjectClip) by object o's palettes
void VelocityObjectTexelCpu(const VelocityObjectPass& pass, const float cur[4],
                            const float prev[4], float inv_w, float out[4]);
void VelocityObjectVertexCpu(const VelocityObject& o, const VelocityObjectPass& pass,
                             const Vertex& v, float cur[4], float prev[4]);
void VelocityBlurCpu(const PostPass& pass, const float uv[2], const float velocity[4],
                     const float centre[4], void (*tap)(const float at[2], float out[4]),
                     float out[4]);
// the trails over a composite's colour `rgb` (unsaturated), from the
// previous post frame's texel `prev` (RGBA 0..1): the colour and alpha out
void TrailsCpu(const PostPass& pass, const float rgb[3], const float prev[4], float out[4]);
// tap 0 or 1's uv at the pixel's uv, and its derivatives (NoiseUv, NoiseDx,
// NoiseDy)
void NoiseTapCpu(const PostPass& pass, const float uv[2], int tap, float at[2], float dx[2],
                 float dy[2]);

}  // namespace band3::render::post
