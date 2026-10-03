// Experimental: the camera motion blur's object pass, for the GPU backend
// (gpu_view.cpp's post_process): each mesh RB3 draws into the velocity
// texture with its own motion (scene_capture.h's VelocityObject), over the
// camera pass's texels (post.hlsl's PSVelocity), as RndVelocityBuffer::
// DrawMesh draws it. The maths is post_model.hlsli's VelocityObject*, which
// the CPU runs too (post_model.cpp's ObjectPass), from VelocityObjectPass
// (post_params.hlsli).
//
// Registers follow SDL_gpu's layout as mesh.hlsl's do: vertex resources in
// space0 (the frame's bones, mesh.hlsl's buffer), vertex uniforms in space1,
// pixel resources in space2, pixel uniforms in space3.
//
// tools/shaders/build_shaders.py compiles it into velocity_shaders.gen.h; run
// it after changing this file or the .hlsli it includes.

#ifdef __spirv__
#define VK_BINDING(n, set) [[vk::binding(n, set)]]
#define VK_LOCATION(n) [[vk::location(n)]]
#define VK_SAMPLER [[vk::combinedImageSampler]]
#else
#define VK_BINDING(n, set)
#define VK_LOCATION(n)
#define VK_SAMPLER
#endif

#define POST_IN(T) T
#include "post_params.hlsli"
#include "post_model.hlsli"

VK_BINDING(0, 1) cbuffer ObjectVertexUniforms : register(b0, space1) {
    VelocityObjectPass vobj;
};

// The frame's bones as gpu_view.cpp packs them, 64 bytes each: an object's
// palette entries, this frame's from vobj.mesh.z and the last frame's after
// them, each its three rows (VS c9.. and c129..) and a fourth unread
VK_BINDING(0, 0) ByteAddressBuffer bones : register(t0, space0);

// the scene's depth as the world's draws left it (kNearW / w, 0 where nothing
// drew), read texel for texel as the game's pixel shader reads s9 (point)
VK_SAMPLER VK_BINDING(0, 2) Texture2D<float> depth_tex : register(t0, space2);
VK_SAMPLER VK_BINDING(0, 2) SamplerState depth_sampler : register(s0, space2);

VK_BINDING(0, 3) cbuffer ObjectPixelUniforms : register(b0, space3) {
    VelocityObjectPass pobj;
};

static const float kNearW = 1e-3;

// mesh.hlsl's, scene_capture.h's Vertex
struct VertexIn {
    VK_LOCATION(0) float3 pos : TEXCOORD0;
    VK_LOCATION(1) float3 nrm : TEXCOORD1;
    VK_LOCATION(2) float2 uv : TEXCOORD2;
    VK_LOCATION(3) float4 color : TEXCOORD3;
    VK_LOCATION(4) uint4 bone : TEXCOORD4;
    VK_LOCATION(5) float4 weight : TEXCOORD5;
    VK_LOCATION(6) float4 tan : TEXCOORD6;
};

// this frame's clip position, as the game's vertex shader gives it, and the
// last frame's
struct ObjectOut {
    float4 pos : SV_Position;
    float4 cur : TEXCOORD0;
    float4 prev : TEXCOORD1;
};

// palette entry `entry`'s world position of `at`
float3 PaletteWorld(uint entry, float4 at) {
    const uint a = entry * 64;
    return float3(dot(asfloat(bones.Load4(a)), at), dot(asfloat(bones.Load4(a + 16)), at),
                  dot(asfloat(bones.Load4(a + 32)), at));
}

ObjectOut VSVelocityObject(VertexIn v) {
    const float4 at = float4(v.pos, 1.0);
    const uint n = vobj.mesh.y, first = vobj.mesh.z;
    float3 cur = 0, prev = 0;
    if (vobj.mesh.x != 0u) {
        const float4 w = VelocityObjectWeights(v.weight);
        const float ws[4] = {w.x, w.y, w.z, w.w};
        [unroll] for (int k = 0; k < 4; k++) {
            const uint b = v.bone[k] < n ? v.bone[k] : 0u;
            cur = cur + PaletteWorld(first + b, at) * ws[k];
            prev = prev + PaletteWorld(first + n + b, at) * ws[k];
        }
    } else {
        cur = PaletteWorld(first, at);
        prev = PaletteWorld(first + n, at);
    }
    ObjectOut o;
    o.cur = VelocityObjectClip(vobj, cur, false);
    o.prev = VelocityObjectClip(vobj, prev, true);
    // on D3D9's pixel centres, half a pixel right and down (mesh.hlsl's
    // clip_offset), and clipped at the camera's near plane, depth 1 - near /
    // w (VelocityObjectDepth)
    o.pos = float4(o.cur.x + vobj.target.z * o.cur.w, o.cur.y - vobj.target.w * o.cur.w,
                   o.cur.w - vobj.depth_range.x, o.cur.w);
    return o;
}

float4 PSVelocityObject(ObjectOut i) : SV_Target0 {
    const float2 uv = VelocityObjectUv(i.cur);
    const float2 size = float2(pobj.depth.xy);
    const int2 at = int2(clamp(floor(uv * size), float2(0.0, 0.0), size - 1.0));
    return VelocityObjectTexel(pobj, i.cur, i.prev, depth_tex.Load(int3(at, 0)) / kNearW);
}
