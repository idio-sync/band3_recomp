// Experimental: what one of the native view's full-screen passes reads
// (post.hlsl's PostUniforms), packed by post_model.cpp from a frame's
// PostParams and PostConsts (src/Render/post_params.h). Shared by post.hlsl
// and the CPU (post_model.h includes it as C++, with float4 and uint4 its
// own), as shade_params.hlsli is for shading. Everything is a float4 or
// uint4: HLSL's cbuffer packing and C++'s layout are then the same.

// flags.x: what the composite applies, as the flags RB3's shader manager
// picks its composite by (TheShaderMgr + 0x26 DOF, 0x27 bloom, 0x28 glare,
// 0x2A colour matrix, 0x25 spotlights; out/research/m4_shader_check.md,
// spotlight_survey.md 2)
static const uint kPostDof = 1u;    // depth of field: the blurred scene, by c24 and the depth
static const uint kPostBloom = 2u;  // bloom's three levels, screen-blended by c6
static const uint kPostGlare = 4u;  // glare: half of bloom's level 0 times c6, added
static const uint kPostXfm = 8u;    // the colour matrix, c92..c94
// the spotlights' depth volume, added by the density map's red (spot)
static const uint kPostSpot = 16u;

// the most taps a blur has (the bloom's Gaussian; the DOF's has 8)
static const uint kPostMaxTaps = 15u;

struct PostPass {
    // x: soft_raster.h's RasterView, for the resolve (0 the picture, 1 the
    // scene's alpha, 2 its depth); y: 1 when the 4x downsample is the bright
    // pass; z: the blur's taps
    uint4 mode;
    uint4 flags;          // x: the kPost bits above
    float4 target;        // the pass's target: width, height, 1/width, 1/height
    float4 half_pixel;    // c15: half a texel of the source, uv (xy)
    float4 taps[15];      // a blur's taps: uv offset (xy) and weight (z)
    float4 c6;            // the bloom colour times its intensity
    float4 c24;           // the DOF's: (1/(scale-bias), -scale/(scale-bias), min, max)
    float4 xfm[3];        // c92..c94: output channel j = dot(xfm[j].xyz, rgb) + xfm[j].w
    float4 camera;        // the world camera's near, far and z range (lo, hi)
    // the spotlights' term: (c127.x, c127.y, c91.x), the volume times x + y *
    // the density, times z
    float4 spot;
};
