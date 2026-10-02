// Experimental: what one of the native view's full-screen passes reads
// (post.hlsl's PostUniforms), packed by post_model.cpp from a frame's
// PostParams and PostConsts (src/Render/post_params.h). Shared by post.hlsl
// and the CPU (post_model.h includes it as C++, with float4 and uint4 its
// own), as shade_params.hlsli is for shading. Everything is a float4 or
// uint4: HLSL's cbuffer packing and C++'s layout are then the same.

// flags.x: what the composite applies, as the flags RB3's shader manager
// picks its composite by (TheShaderMgr + 0x26 DOF, 0x27 bloom, 0x28 glare,
// 0x2A colour matrix, 0x25 spotlights, 0x3F soft particles, 0x2D noise and
// 0x2E its midtone weight, 0x2F trails; out/research/m4_shader_check.md,
// spotlight_survey.md 2, softparticle_survey.md 1, n1_post_noise.md)
static const uint kPostDof = 1u;    // depth of field: the blurred scene, by c24 and the depth
static const uint kPostBloom = 2u;  // bloom's three levels, screen-blended by c6
// glare: half of bloom's level 0 (after its glare pass) times c6, added
static const uint kPostGlare = 4u;
static const uint kPostXfm = 8u;    // the colour matrix, c92..c94
// the spotlights' depth volume, added by the density map's red (spot)
static const uint kPostSpot = 16u;
// the soft-particle buffer (s4), added after the DOF, before bloom
static const uint kPostSoft = 32u;
// the noise (film grain): the noise map overlaid by c112/c113, after the
// spotlights' term, before the colour matrix; with kPostNoiseMidtone
// weighted by the luminance's midtones
static const uint kPostNoise = 64u;
static const uint kPostNoiseMidtone = 128u;
// the trails (blend previous): the previous post frame faded, kept where
// it's brighter, last; on where the renderer has its previous frame
static const uint kPostTrails = 256u;

// the most taps a blur has (the bloom's Gaussian; the DOF's has 8)
static const uint kPostMaxTaps = 15u;

struct PostPass {
    // x: soft_raster.h's RasterView, for the resolve (0 the picture, 1 the
    // scene's alpha, 2 its depth); y: 1 when the 4x downsample is the bright
    // pass; z: the blur's taps; w: a texture pass's blur's samples per tap
    // (soft_raster.h's BlurSubTaps), 0 or 1 one
    uint4 mode;
    uint4 flags;          // x: the kPost bits above
    float4 target;        // the pass's target: width, height, 1/width, 1/height
    // c15: half a texel of the source, uv (xy); zw a texture pass's blur's
    // step between a tap's samples (BlurSubTaps)
    float4 half_pixel;
    float4 taps[15];      // a blur's taps: uv offset (xy) and weight (z)
    float4 c6;            // the bloom colour times its intensity
    float4 c24;           // the DOF's: (1/(scale-bias), -scale/(scale-bias), min, max)
    float4 xfm[3];        // c92..c94: output channel j = dot(xfm[j].xyz, rgb) + xfm[j].w
    float4 camera;        // the world camera's near, far and z range (lo, hi)
    // the spotlights' term: (c127.x, c127.y, c91.x), the volume times x + y *
    // the density, times z
    float4 spot;
    // the noise's: c112, the seeds (the two taps' uv offsets), and c113,
    // (base scale x, y, the second tap's scale on top, intensity)
    float4 noise_seeds;
    float4 noise;
    // the noise map's sampler (sample_model.h's PackSampler), and its size
    // (x, y) and, on the GPU, its layer in its texture array (z)
    uint4 noise_sampler;
    uint4 noise_tex;
    // c125, the trails': (threshold, fade (dt / duration), 1/3, 0)
    float4 trails;
};
