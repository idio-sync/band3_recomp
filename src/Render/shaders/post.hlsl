// The GPU backend's full-screen passes between the world's draws and the
// overlay's: RB3's post-processing (maths in post_model.hlsli, shared with
// the CPU), the resolve for frames without it, and the overlay's start.
//
// Registers follow SDL_gpu's layout as mesh.hlsl's do: pixel resources in
// space2, pixel uniforms in space3; the vertex shader has none.
//
// tools/shaders/build_shaders.py compiles it into post_shaders.gen.h; rerun
// it after changing this file or the .hlsli it includes.

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

// t0 the source (scene colour, alpha the bloom weight, or a downsample/blur
// level); t1 the scene's depth (kNearW / w, 0 where nothing drew); t2 DOF;
// t3..t5 bloom; t6..t8 spotlight volume, density, soft particles (one-layer
// arrays); t9 the noise map, layer params.noise_tex.z, filtered by
// sample_model.hlsli; t10 velocity; t11 the previous post frame. Samplers are
// linear clamp, as RB3 sets them.
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
VK_SAMPLER VK_BINDING(9, 2) Texture2DArray<float4> noise_tex : register(t9, space2);
VK_SAMPLER VK_BINDING(9, 2) SamplerState noise_sampler : register(s9, space2);
VK_SAMPLER VK_BINDING(10, 2) Texture2D<float4> velocity_tex : register(t10, space2);
VK_SAMPLER VK_BINDING(10, 2) SamplerState velocity_sampler : register(s10, space2);
VK_SAMPLER VK_BINDING(11, 2) Texture2D<float4> prev_tex : register(t11, space2);
VK_SAMPLER VK_BINDING(11, 2) SamplerState prev_sampler : register(s11, space2);

VK_BINDING(0, 3) cbuffer PostUniforms : register(b0, space3) {
    PostPass params;
};

static const float kNearW = 1e-3;

#define SAMPLE_TEX Texture2DArray<float4> t, uint layer
#define SAMPLE_ARGS t, layer
#define SAMPLE_LOAD(level, x, y) t.Load(int4(x, y, layer, level))
#define SAMPLE_LOOP [loop]
#include "sample_model.hlsli"

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

// The overlay's multisampled target's start (begin_back): the picture, as
// DxRnd::DoPostProcess's CopyPostProcess, and depth 0 as BeginTiling clears
// it, or with mode.y the world's (old captures without cameras)
struct OverlayStartOut {
    float4 color : SV_Target0;
    float depth : SV_Depth;
};

OverlayStartOut PSOverlayStart(PostIn i) {
    const int3 at = int3(int2(i.pos.xy), 0);
    OverlayStartOut o;
    o.color = float4(color_tex.Load(at).rgb, 1.0);
    o.depth = params.mode.y != 0u ? depth_tex.Load(at) : 0.0;
    return o;
}

// the 4x downsample, or the bright pass (mode.y)
float4 PSDownsample(PostIn i) : SV_Target0 {
    const float2 uv = PixelUv(i);
    float4 t[4];
    [unroll] for (int k = 0; k < 4; k++)
        t[k] = color_tex.SampleLevel(color_sampler, QuadTap(uv, params.half_pixel, k), 0);
    return Quad(t[0], t[1], t[2], t[3], params.mode.y != 0u);
}

// mode.w > 1 (BlurSubTaps): each tap averages mode.w samples half_pixel.zw
// apart
float4 PSBlur(PostIn i) : SV_Target0 {
    const float2 uv = PixelUv(i);
    float4 sum = 0.0;
    const uint n = params.mode.w;
    if (n <= 1u) {
        for (uint k = 0; k < params.mode.z; k++)
            sum += color_tex.SampleLevel(color_sampler, uv + params.taps[k].xy, 0) *
                   params.taps[k].z;
        return sum;
    }
    const float2 step = params.half_pixel.zw;
    const float2 first = -0.5 * float(n - 1u) * step;
    for (uint k = 0; k < params.mode.z; k++) {
        float4 tap = 0.0;
        for (uint j = 0; j < n; j++)
            tap += color_tex.SampleLevel(color_sampler,
                                         uv + params.taps[k].xy + first + float(j) * step, 0);
        sum += tap / float(n) * params.taps[k].z;
    }
    return sum;
}

float4 PSGlare(PostIn i) : SV_Target0 {
    const float2 uv = PixelUv(i);
    const float2 stride = GlareStep(uv);
    float2 at = uv;
    float3 sum = 0.0;
    [unroll] for (int k = 0; k < kGlareTaps; k++) {
        sum += GlareTerm(color_tex.SampleLevel(color_sampler, at, 0).rgb, GlareWeight(at));
        at += stride;
    }
    return float4(GlareOut(sum), 1.0);
}

// depth point-sampled (s9) in integers as the CPU does; mode.yz its size
float4 PSVelocity(PostIn i) : SV_Target0 {
    const uint2 v = uint2(i.pos.xy);
    const uint2 size = params.mode.yz;
    const uint2 level = uint2(params.target.xy);
    const uint2 at = min((2u * v + 1u) * size / (2u * level), size - 1u);
    return VelocityTexel(params, PixelUv(i), depth_tex.Load(int3(int2(at), 0)) / kNearW);
}

// unsaturated CompositeColor, and CompositeAlpha
float3 CompositeAt(PostIn i, out float alpha) {
    const int3 at = int3(int2(i.pos.xy), 0);
    const float2 uv = PixelUv(i);
    const uint f = params.flags.x;
    float4 scene = color_tex.Load(at);
    if ((f & kPostVelocity) != 0u) {
        const float4 v = velocity_tex.SampleLevel(velocity_sampler, uv, 0);
        if (VelocityBlurs(params, v)) {
            const float2 step = VelocityStep(params, v);
            float4 acc = scene * kVelocityCentre;
            [unroll] for (int k = -5; k < 5; k++)
                acc += color_tex.SampleLevel(color_sampler, VelocityTap(uv, step, k), 0) *
                       VelocityWeight(k);
            scene = acc * kVelocityNorm;
        }
    }
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
    float3 noise[2] = {float3(0.0, 0.0, 0.0), float3(0.0, 0.0, 0.0)};
    if ((f & kPostNoise) != 0u) {
        [unroll] for (int k = 0; k < 2; k++)
            noise[k] = SampleTexture(noise_tex, params.noise_tex.z, params.noise_tex.xy,
                                     params.noise_sampler, NoiseUv(params, uv, k),
                                     NoiseDx(params, k), NoiseDy(params, k)).rgb;
    }
    alpha = CompositeAlpha(params, scene, dof, depth);
    return CompositeColor(params, scene, dof, depth, l0, l1, l2, volume, density, soft, noise[0],
                          noise[1]);
}

// opaque: the overlay draws over it
float4 PSComposite(PostIn i) : SV_Target0 {
    float alpha;
    return float4(saturate(CompositeAt(i, alpha)), 1.0);
}

// The live view's composite, also writing the post buffer (colour and
// alpha, as the game's resolve keeps it) for the next frame's trails
struct CompositeOut {
    float4 color : SV_Target0;
    float4 history : SV_Target1;
};

CompositeOut PSCompositeHistory(PostIn i) {
    float alpha;
    const float3 rgb = CompositeAt(i, alpha);
    float4 c = float4(saturate(rgb), alpha);
    if ((params.flags.x & kPostTrails) != 0u)
        c = Trails(params, rgb, prev_tex.Load(int3(int2(i.pos.xy), 0)));
    CompositeOut o;
    o.color = float4(c.rgb, 1.0);
    o.history = c;
    return o;
}
