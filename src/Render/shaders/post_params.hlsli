// One full-screen pass's inputs (post.hlsl's PostUniforms), packed by
// post_model.cpp. Included as C++ by post_model.h. Only float4 and uint4, so
// HLSL's cbuffer packing matches the C++ layout.

// flags.x, after TheShaderMgr's composite flags (+0x26 DOF, 0x27 bloom, 0x28
// glare, 0x2A colour matrix, 0x25 spotlights, 0x3F soft particles, 0x2D noise,
// 0x2E its midtone weight, 0x2F trails, 0x39 velocity blur;
// m4_shader_check.md, spotlight_survey.md 2, softparticle_survey.md 1,
// n1_post_noise.md, n5_hub_soft.md)
static const uint kPostDof = 1u;
static const uint kPostBloom = 2u;
static const uint kPostGlare = 4u;
static const uint kPostXfm = 8u;    // the colour matrix, c92..c94
static const uint kPostSpot = 16u;
static const uint kPostSoft = 32u;  // s4
static const uint kPostNoise = 64u;
static const uint kPostNoiseMidtone = 128u;
// only where the renderer has its previous frame
static const uint kPostTrails = 256u;
static const uint kPostVelocity = 512u;

// the bloom's Gaussian; the DOF's has 8
static const uint kPostMaxTaps = 15u;

struct PostPass {
    // x: RasterView for the resolve (0 picture, 1 scene alpha, 2 depth); y: 1
    // when the 4x downsample is the bright pass; z: blur taps; w: a texture
    // pass's blur samples per tap (BlurSubTaps), 0 meaning 1
    uint4 mode;
    uint4 flags;          // x: the kPost bits above
    float4 target;        // width, height, 1/width, 1/height
    // c15: half a source texel in uv (xy); zw the BlurSubTaps step
    float4 half_pixel;
    float4 taps[15];      // uv offset (xy) and weight (z)
    float4 c6;            // the bloom colour times its intensity
    float4 c24;           // the DOF's: (1/(scale-bias), -scale/(scale-bias), min, max)
    float4 xfm[3];        // c92..c94: output channel j = dot(xfm[j].xyz, rgb) + xfm[j].w
    float4 camera;        // the world camera's near, far and z range (lo, hi)
    // (c127.x, c127.y, c91.x): volume * (x + y * density) * z
    float4 spot;
    // c112, the two taps' uv offsets; c113, (scale x, y, tap 1's extra scale,
    // intensity)
    float4 noise_seeds;
    float4 noise;
    // PackSampler's; size (xy) and, on the GPU, array layer (z)
    uint4 noise_sampler;
    uint4 noise_tex;
    // c125: (threshold, dt / duration, 1/3, 0)
    float4 trails;
    // VelocityTexel's: vel_prev the previous view-projection's rows (PS
    // c134..c137); vel_near the frustum's near point (xyz) and far plane (w,
    // c89.y); vel_corner the top-left ray and its change across u (1) and
    // down v (2); vel_depth x: w / far where nothing drew, y: c122.x, the step
    // scale
    float4 vel_prev[4];
    float4 vel_near;
    float4 vel_corner[3];
    float4 vel_depth;
};

// The motion blur's object pass (VelocityObject): view_proj VS c0..c3 (this
// frame) and c4..c7 (last), depth_range PS c8; target (w, h, 1/w, 1/h); mesh.x
// skinned, y palette entries, z this frame's palette start in the GPU's bones
// (the last frame's follows); depth the scene depth's size (xy).
struct VelocityObjectPass {
    float4 view_proj[8];
    float4 depth_range;
    float4 target;
    uint4 mesh;
    uint4 depth;
};
