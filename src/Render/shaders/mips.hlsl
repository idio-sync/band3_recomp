// A texture pass's mips for the GPU backend, drawn in the frame's own command
// buffer exactly as SDL_GenerateMipmapsForGPUTexture's D3D12 blits draw them.
// Unlike SDL's, these go through BeginPass, so they don't take a sampler
// outside gpu_view.cpp's kSamplerBatch step.
//
// Registers follow SDL_gpu's layout as post.hlsl's do: pixel resources in
// space2, pixel uniforms in space3.
//
// tools/shaders/build_shaders.py compiles it into mips_shaders.gen.h; rerun
// it after changing this file.

#ifdef __spirv__
#define VK_BINDING(n, set) [[vk::binding(n, set)]]
#define VK_SAMPLER [[vk::combinedImageSampler]]
#else
#define VK_BINDING(n, set)
#define VK_SAMPLER
#endif

// the target itself; a pass reads the level above the one it draws
VK_SAMPLER VK_BINDING(0, 2) Texture2DArray<float4> source_tex : register(t0, space2);
VK_SAMPLER VK_BINDING(0, 2) SamplerState source_sampler : register(s0, space2);

// level.x: the level read
VK_BINDING(0, 3) cbuffer MipUniforms : register(b0, space3) {
    uint4 level;
};

// SDL's VertexToPixel
struct MipIn {
    float2 uv : TEXCOORD0;
    float4 pos : SV_Position;
};

// SDL's FullscreenVert
MipIn VSMip(uint id : SV_VertexID) {
    const float2 p = float2((id << 1) & 2, id & 2);
    MipIn o;
    o.uv = p;
    o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// SDL's BlitFrom2DArray, uv offset 0, scale 1, layer 0
float4 PSMip(MipIn i) : SV_Target0 {
    return source_tex.SampleLevel(source_sampler, float3(i.uv, 0.0), float(level.x));
}
