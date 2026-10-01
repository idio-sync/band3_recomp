// Experimental: the native view's mesh shader, for the GPU backend
// (gpu_view.cpp). It draws what soft_raster.cpp draws, the same way: the
// shading itself is shade.hlsli, which the CPU compiles too, from the same
// ShadeParams (shade_model.cpp packs them).
//
// Registers follow SDL_gpu's layout (SDL_CreateGPUShader in SDL_gpu.h): vertex
// resources in space0, vertex uniforms in space1, pixel resources in space2,
// pixel uniforms in space3. For SPIR-V the [[vk::]] attributes put the same
// bindings in SDL's Vulkan descriptor sets 0-3.
//
// tools/shaders/build_shaders.py compiles it (DXBC for Direct3D 12, SPIR-V for
// Vulkan) into mesh_shaders.gen.h; run it after changing this file.

#ifdef __spirv__
#define VK_BINDING(n, set) [[vk::binding(n, set)]]
#define VK_LOCATION(n) [[vk::location(n)]]
#define VK_SAMPLER [[vk::combinedImageSampler]]
#else
#define VK_BINDING(n, set)
#define VK_LOCATION(n)
#define VK_SAMPLER
#endif

#define SHADE_IN(T) T
#include "shade_params.hlsli"
#include "shade.hlsli"

// Milo's matrices are row vectors (v' = v * M), and the uniforms arrive as
// they are in memory, so mul(v, M) with row_major needs no transpose.
VK_BINDING(0, 1) cbuffer VertexUniforms : register(b0, space1) {
    row_major float4x4 world;
    row_major float4x4 view_proj;
    uint skinned;     // bones[bone_base..] place the vertex instead of world
    uint bone_base;
    uint bone_count;
    uint vertex_pad;
    ShadeParams vs_shade;  // the texture's transform, and a vertex-lit draw's light
};

// the frame's bone matrices, four float4 rows each; SDL binds storage buffers
// as raw views, so a ByteAddressBuffer reads them as they are
VK_BINDING(0, 0) ByteAddressBuffer bones : register(t0, space0);

// Textures share arrays by size class, a texture to a layer, in its corner
// (gpu_view.cpp): the diffuse texture, the specular map and the glow map. The
// samplers are SDL_gpu's pairing; the shader reads texels itself, as
// soft_raster.cpp's Texel() does
VK_SAMPLER VK_BINDING(0, 2) Texture2DArray<float4> tex : register(t0, space2);
VK_SAMPLER VK_BINDING(0, 2) SamplerState tex_sampler : register(s0, space2);
VK_SAMPLER VK_BINDING(1, 2) Texture2DArray<float4> spec_tex : register(t1, space2);
VK_SAMPLER VK_BINDING(1, 2) SamplerState spec_sampler : register(s1, space2);
VK_SAMPLER VK_BINDING(2, 2) Texture2DArray<float4> glow_tex : register(t2, space2);
VK_SAMPLER VK_BINDING(2, 2) SamplerState glow_sampler : register(s2, space2);
// a copy of the picture as the resolve left it, for kShadeRefract (a 1x1
// stand-in for the others): read at the pixel's own position, as Shade() in
// soft_raster.cpp does
VK_SAMPLER VK_BINDING(3, 2) Texture2D<float4> behind_tex : register(t3, space2);
VK_SAMPLER VK_BINDING(3, 2) SamplerState behind_sampler : register(s3, space2);

// pixel_flags.x
// SrcAlpha and SrcAlphaAdd: the colour leaves already scaled by its alpha,
// which the blend can't clamp first (gpu_view.cpp's pipelines)
static const uint kPremultiply = 1;

VK_BINDING(0, 3) cbuffer PixelUniforms : register(b0, space3) {
    ShadeParams ps_shade;
    uint4 tex_layer;    // the diffuse texture's, the specular map's, the glow map's
    uint4 tex_size[3];  // and their own sizes (xy); their layers may be bigger
    uint4 pixel_flags;
};

// soft_raster.cpp's near plane: w below it is clipped
static const float kNearW = 1e-3;

// SDL_gpu names vertex attribute n TEXCOORDn on Direct3D, location n on Vulkan;
// the order is scene_capture.h's Vertex
struct VertexIn {
    VK_LOCATION(0) float3 pos : TEXCOORD0;
    VK_LOCATION(1) float3 nrm : TEXCOORD1;
    VK_LOCATION(2) float2 uv : TEXCOORD2;
    VK_LOCATION(3) float4 color : TEXCOORD3;  // UBYTE4_NORM, R in the low byte
    VK_LOCATION(4) uint4 bone : TEXCOORD4;
    VK_LOCATION(5) float4 weight : TEXCOORD5;
};

struct PixelIn {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float3 nrm : TEXCOORD1;
    float4 color : TEXCOORD2;
    float3 wpos : TEXCOORD3;           // world position
    float depth : TEXCOORD4;           // clip w
    float3 light_diffuse : TEXCOORD5;  // a vertex-lit draw's Lighting
    float3 light_added : TEXCOORD6;
    float2 ao_sh : TEXCOORD7;          // the point lights' AoShVertex
};

float4x4 Bone(uint i) {
    const uint at = (bone_base + i) * 64;
    return float4x4(asfloat(bones.Load4(at)), asfloat(bones.Load4(at + 16)),
                    asfloat(bones.Load4(at + 32)), asfloat(bones.Load4(at + 48)));
}

PixelIn VSMain(VertexIn v) {
    // the vertex colour's SH direction turns as the normal does
    const float3 dir = AoShDirection(v.color);
    float3 wp = 0, wn = 0, wd = 0;
    if (skinned != 0) {
        float total = 0;
        [unroll] for (int k = 0; k < 4; k++) {
            const float w = v.weight[k];
            if (w <= 0) continue;
            const float4x4 b = Bone(v.bone[k] < bone_count ? v.bone[k] : 0);
            wp += mul(float4(v.pos, 1), b).xyz * w;
            wn += mul(float4(v.nrm, 0), b).xyz * w;
            wd += mul(float4(dir, 0), b).xyz * w;
            total += w;
        }
        if (total <= 0) {
            const float4x4 b = Bone(0);
            wp = mul(float4(v.pos, 1), b).xyz;
            wn = mul(float4(v.nrm, 0), b).xyz;
            wd = mul(float4(dir, 0), b).xyz;
        }
    } else {
        wp = mul(float4(v.pos, 1), world).xyz;
        wn = mul(float4(v.nrm, 0), world).xyz;
        wd = mul(float4(dir, 0), world).xyz;
    }
    const float4 clip = mul(float4(wp, 1), view_proj);
    PixelIn o;
    // depth is kNearW / w, the CPU's 1/w scaled: z/w interpolates as 1/w does,
    // larger is nearer, the near plane is w = kNearW and there's no far plane,
    // whatever depth range the game's projection has
    o.pos = float4(clip.xy, kNearW, clip.w);
    o.uv = TexGen(vs_shade, v.uv);
    o.nrm = wn;
    o.color = v.color;
    o.wpos = wp;
    o.depth = clip.w;
    o.ao_sh = AoShVertex(vs_shade, wp, wn, wd, v.color);
    o.light_diffuse = float3(0, 0, 0);
    o.light_added = float3(0, 0, 0);
    if ((vs_shade.flags.x & kShadePerVertex) != 0u) {
        const Lighting l = Light(vs_shade, wp, wn, v.color, float4(1, 1, 1, 1), o.ao_sh);
        o.light_diffuse = l.diffuse;
        o.light_added = l.added;
    }
    return o;
}

// soft_raster.cpp's Texel(): the nearest texel, wrapping, by the same arithmetic
float4 Texel(Texture2DArray<float4> t, float2 uv, uint layer, uint2 size) {
    const float2 f = uv - floor(uv);
    const uint2 at = min(uint2(f * float2(size)), size - 1);
    return t.Load(int4(at, layer, 0));
}

float4 PSMain(PixelIn i) : SV_Target0 {
    const uint f = ps_shade.flags.x;
    float4 texel = float4(1, 1, 1, 1);
    float4 spec_map = float4(1, 1, 1, 1);
    float4 glow = float4(0, 0, 0, 0);
    if ((f & kShadeTextured) != 0u) texel = Texel(tex, i.uv, tex_layer.x, tex_size[0].xy);
    if ((f & kShadeSpecMap) != 0u) spec_map = Texel(spec_tex, i.uv, tex_layer.y, tex_size[1].xy);
    if ((f & kShadeGlow) != 0u) glow = Texel(glow_tex, i.uv, tex_layer.z, tex_size[2].xy);
    // SV_Position is the pixel's centre: its integer part is the pixel
    float4 behind = float4(1, 1, 1, 1);
    if ((f & kShadeRefract) != 0u) behind = behind_tex.Load(int3(int2(i.pos.xy), 0));
    Lighting vertex;
    vertex.diffuse = i.light_diffuse;
    vertex.added = i.light_added;
    float4 c = ShadePixel(ps_shade, i.wpos, i.nrm, i.color, texel, spec_map, glow, behind,
                          i.depth, i.ao_sh, vertex);
    if (AlphaCut(ps_shade, c.a)) discard;
    // Blend() in soft_raster.cpp clamps alpha, never the colour, before
    // scaling by it; a UNORM target clamps what reaches the blender, so the
    // scaling happens here, where colour above 1 still counts
    if ((pixel_flags.x & kPremultiply) != 0u) c.rgb *= saturate(c.a);
    return c;
}
