// The GPU backend's display gamma pass (gamma_ramp.h's GammaLut) over the
// finished picture, overlay included, with post.hlsl's VSFullscreen.
//
// Registers follow SDL_gpu's layout as post.hlsl's do: pixel resources in
// space2, pixel uniforms in space3.
//
// tools/shaders/build_shaders.py compiles it into gamma_shaders.gen.h; rerun
// it after changing this file.

#ifdef __spirv__
#define VK_BINDING(n, set) [[vk::binding(n, set)]]
#define VK_SAMPLER [[vk::combinedImageSampler]]
#else
#define VK_BINDING(n, set)
#define VK_SAMPLER
#endif

VK_SAMPLER VK_BINDING(0, 2) Texture2D<float4> color_tex : register(t0, space2);
VK_SAMPLER VK_BINDING(0, 2) SamplerState color_sampler : register(s0, space2);

// lut[v / 4][v % 4]: value v's output, red in the low byte, then green, blue
VK_BINDING(0, 3) cbuffer GammaUniforms : register(b0, space3) {
    uint4 lut[64];
};

struct GammaIn {
    float4 pos : SV_Position;
};

float4 PSGamma(GammaIn i) : SV_Target0 {
    const float3 c = color_tex.Load(int3(int2(i.pos.xy), 0)).rgb;
    // exact 8-bit values in and out
    const uint3 v = uint3(saturate(c) * 255.0 + 0.5);
    const uint r = lut[v.r >> 2u][v.r & 3u] & 0xffu;
    const uint g = (lut[v.g >> 2u][v.g & 3u] >> 8u) & 0xffu;
    const uint b = (lut[v.b >> 2u][v.b & 3u] >> 16u) & 0xffu;
    return float4(float3(r, g, b) / 255.0, 1.0);
}
