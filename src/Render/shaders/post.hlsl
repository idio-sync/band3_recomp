// Experimental: the native view's full-screen passes, for the GPU backend
// (gpu_view.cpp): what happens to the scene target between the world's draws
// and the overlay's (soft_raster.h). RB3's post-processing (post_model.h):
// the 4x downsample (or bright pass), the blurs and the composite, whose
// maths is post_model.hlsli's, which the CPU runs too; and the resolve, the
// scene into the picture as it is (or, to check the scene target, its alpha
// or its depth as grey) on frames without post-processing.
//
// Registers follow SDL_gpu's layout as mesh.hlsl's do: pixel resources in
// space2, pixel uniforms in space3; the vertex shader has none.
//
// tools/shaders/build_shaders.py compiles it into post_shaders.gen.h; run it
// after changing this file or the .hlsli it includes.

#ifdef __spirv__
#define VK_BINDING(n, set) [[vk::binding(n, set)]]
#define VK_SAMPLER [[vk::combinedImageSampler]]
#else
#define VK_BINDING(n, set)
#define VK_SAMPLER
#endif

#define POST_IN(T) T
#include "post_params.hlsli"
#include "post_model.hlsli"

// t0 the pass's source: the scene target's colour (alpha the bloom weight),
// read texel for texel by the resolve and the composite, or the level a
// downsample or blur reads, bilinear; t1 the scene's depth (kNearW / w,
// larger is nearer, 0 where nothing drew); t2 the DOF's level; t3..t5
// bloom's; t6 the spotlights' depth volume and t7 their density map, t8 the
// soft-particle buffer, render targets of texture passes (gpu_view.cpp's,
// arrays of one layer). The samplers are linear and clamp, as RB3 sets them.
VK_SAMPLER VK_BINDING(0, 2) Texture2D<float4> color_tex : register(t0, space2);
VK_SAMPLER VK_BINDING(0, 2) SamplerState color_sampler : register(s0, space2);
VK_SAMPLER VK_BINDING(1, 2) Texture2D<float> depth_tex : register(t1, space2);
VK_SAMPLER VK_BINDING(1, 2) SamplerState depth_sampler : register(s1, space2);
VK_SAMPLER VK_BINDING(2, 2) Texture2D<float4> dof_tex : register(t2, space2);
VK_SAMPLER VK_BINDING(2, 2) SamplerState dof_sampler : register(s2, space2);
VK_SAMPLER VK_BINDING(3, 2) Texture2D<float4> bloom0_tex : register(t3, space2);
VK_SAMPLER VK_BINDING(3, 2) SamplerState bloom0_sampler : register(s3, space2);
VK_SAMPLER VK_BINDING(4, 2) Texture2D<float4> bloom1_tex : register(t4, space2);
VK_SAMPLER VK_BINDING(4, 2) SamplerState bloom1_sampler : register(s4, space2);
VK_SAMPLER VK_BINDING(5, 2) Texture2D<float4> bloom2_tex : register(t5, space2);
VK_SAMPLER VK_BINDING(5, 2) SamplerState bloom2_sampler : register(s5, space2);
VK_SAMPLER VK_BINDING(6, 2) Texture2DArray<float4> volume_tex : register(t6, space2);
VK_SAMPLER VK_BINDING(6, 2) SamplerState volume_sampler : register(s6, space2);
VK_SAMPLER VK_BINDING(7, 2) Texture2DArray<float4> density_tex : register(t7, space2);
VK_SAMPLER VK_BINDING(7, 2) SamplerState density_sampler : register(s7, space2);
VK_SAMPLER VK_BINDING(8, 2) Texture2DArray<float4> soft_tex : register(t8, space2);
VK_SAMPLER VK_BINDING(8, 2) SamplerState soft_sampler : register(s8, space2);

VK_BINDING(0, 3) cbuffer PostUniforms : register(b0, space3) {
    PostPass params;
};

static const float kNearW = 1e-3;

struct PostIn {
    float4 pos : SV_Position;
};

// one triangle over the whole target
PostIn VSFullscreen(uint id : SV_VertexID) {
    const float2 p = float2((id << 1) & 2, id & 2);
    PostIn o;
    o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

// soft_raster.h's DepthViewGrey
float DepthViewGrey(float depth) {
    if (!(depth > 0.0)) return 0.0;
    const float w = kNearW / depth;
    return saturate(1.0 - (log2(max(w, 1.0)) - 4.0) / 8.0);
}

// the pixel's centre in uv
float2 PixelUv(PostIn i) { return i.pos.xy * params.target.zw; }

float4 PSResolve(PostIn i) : SV_Target0 {
    const int3 at = int3(int2(i.pos.xy), 0);
    const float4 c = color_tex.Load(at);
    if (params.mode.x == 1u) return float4(c.aaa, 1.0);
    if (params.mode.x == 2u) {
        const float g = DepthViewGrey(depth_tex.Load(at));
        return float4(g, g, g, 1.0);
    }
    return float4(c.rgb, 1.0);
}

// the 4x downsample, or the bright pass (mode.y)
float4 PSDownsample(PostIn i) : SV_Target0 {
    const float2 uv = PixelUv(i);
    float4 t[4];
    [unroll] for (int k = 0; k < 4; k++)
        t[k] = color_tex.SampleLevel(color_sampler, QuadTap(uv, params.half_pixel, k), 0);
    return Quad(t[0], t[1], t[2], t[3], params.mode.y != 0u);
}

// a blur: mode.z taps of params.taps
float4 PSBlur(PostIn i) : SV_Target0 {
    const float2 uv = PixelUv(i);
    float4 sum = 0.0;
    for (uint k = 0; k < params.mode.z; k++)
        sum += color_tex.SampleLevel(color_sampler, uv + params.taps[k].xy, 0) * params.taps[k].z;
    return sum;
}

// the composite into the picture, opaque: the overlay draws over it
float4 PSComposite(PostIn i) : SV_Target0 {
    const int3 at = int3(int2(i.pos.xy), 0);
    const float2 uv = PixelUv(i);
    const uint f = params.flags.x;
    const float4 scene = color_tex.Load(at);
    const float depth = GameDepth(params, depth_tex.Load(at) / kNearW);
    float4 dof = 0.0;
    if ((f & kPostDof) != 0u) dof = dof_tex.SampleLevel(dof_sampler, uv, 0);
    float3 l0 = 0.0, l1 = 0.0, l2 = 0.0;
    if ((f & (kPostBloom | kPostGlare)) != 0u)
        l0 = bloom0_tex.SampleLevel(bloom0_sampler, uv, 0).rgb;
    if ((f & kPostBloom) != 0u) {
        l1 = bloom1_tex.SampleLevel(bloom1_sampler, uv, 0).rgb;
        l2 = bloom2_tex.SampleLevel(bloom2_sampler, uv, 0).rgb;
    }
    float3 volume = 0.0;
    float density = 0.0;
    if ((f & kPostSpot) != 0u) {
        volume = volume_tex.SampleLevel(volume_sampler, float3(uv, 0.0), 0).rgb;
        density = density_tex.SampleLevel(density_sampler, float3(uv, 0.0), 0).r;
    }
    float3 soft = 0.0;
    if ((f & kPostSoft) != 0u) soft = soft_tex.SampleLevel(soft_sampler, float3(uv, 0.0), 0).rgb;
    return float4(Composite(params, scene, dof, depth, l0, l1, l2, volume, density, soft), 1.0);
}
