// A texture pass's mips, for the GPU backend (gpu_view.cpp): each level drawn
// from the one above it in the frame's own command buffer, as SDL_gpu's
// SDL_GenerateMipmapsForGPUTexture draws them on Direct3D 12 (a blit a level:
// its full-screen triangle and texture coordinates, and BlitFrom2DArray's
// SampleLevel of the level above, linear and clamping, at those coordinates),
// so each level comes out as SDL's did: a 2x2 box where the level above is
// even, its own taps where it's odd. SDL's own take a sampler outside
// gpu_view.cpp's kSamplerBatch step, which these, drawn through BeginPass,
// don't.
//
// Registers follow SDL_gpu's layout as post.hlsl's do: pixel resources in
// space2, pixel uniforms in space3.
//
// tools/shaders/build_shaders.py compiles it into mips_shaders.gen.h; run it
// after changing this file.

#ifdef __spirv__
#define VK_BINDING(n, set) [[vk::binding(n, set)]]
#define VK_SAMPLER [[vk::combinedImageSampler]]
#else
#define VK_BINDING(n, set)
#define VK_SAMPLER
#endif

// t0 the target, every level; the pass draws into one and reads the one
// above it
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

// SDL's FullscreenVert: one triangle over the whole target, uv 0..1 across
// it from the top left
MipIn VSMip(uint id : SV_VertexID) {
    const float2 p = float2((id << 1) & 2, id & 2);
    MipIn o;
    o.uv = p;
    o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// SDL's BlitFrom2DArray over the whole of the level above (its uv offset 0
// and scale 1) from layer 0
float4 PSMip(MipIn i) : SV_Target0 {
    return source_tex.SampleLevel(source_sampler, float3(i.uv, 0.0), float(level.x));
}
