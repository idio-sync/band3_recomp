// Experimental: the native view's full-screen passes, for the GPU backend
// (gpu_view.cpp): what happens to the scene target between the world's draws
// and the overlay's (soft_raster.h). For now that's the resolve, the scene
// into the picture as it is (or, to check the scene target, its alpha or its
// depth as grey); RB3's post-processing comes here.
//
// Registers follow SDL_gpu's layout as mesh.hlsl's do: pixel resources in
// space2, pixel uniforms in space3; the vertex shader has none.
//
// tools/shaders/build_shaders.py compiles it into post_shaders.gen.h; run it
// after changing this file.

#ifdef __spirv__
#define VK_BINDING(n, set) [[vk::binding(n, set)]]
#define VK_SAMPLER [[vk::combinedImageSampler]]
#else
#define VK_BINDING(n, set)
#define VK_SAMPLER
#endif

// the scene target's colour (alpha the bloom weight) and depth (kNearW / w,
// larger is nearer, 0 where nothing drew), read texel for texel
VK_SAMPLER VK_BINDING(0, 2) Texture2D<float4> scene_tex : register(t0, space2);
VK_SAMPLER VK_BINDING(0, 2) SamplerState scene_sampler : register(s0, space2);
VK_SAMPLER VK_BINDING(1, 2) Texture2D<float> depth_tex : register(t1, space2);
VK_SAMPLER VK_BINDING(1, 2) SamplerState depth_sampler : register(s1, space2);

// post_view.x: soft_raster.h's RasterView, 0 the picture, 1 the scene's
// alpha, 2 its depth
VK_BINDING(0, 3) cbuffer PostUniforms : register(b0, space3) {
    uint4 post_view;
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

float4 PSResolve(PostIn i) : SV_Target0 {
    const int3 at = int3(int2(i.pos.xy), 0);
    const float4 c = scene_tex.Load(at);
    if (post_view.x == 1u) return float4(c.aaa, 1.0);
    if (post_view.x == 2u) {
        const float g = DepthViewGrey(depth_tex.Load(at));
        return float4(g, g, g, 1.0);
    }
    return float4(c.rgb, 1.0);
}
