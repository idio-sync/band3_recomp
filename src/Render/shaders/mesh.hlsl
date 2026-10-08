// The GPU backend's mesh shader (gpu_view.cpp), matching soft_raster.cpp.
// Shading is shade.hlsli, shared with the CPU; spotlight cones use
// spot_model.hlsli.
//
// Registers follow SDL_gpu's layout (SDL_CreateGPUShader): vertex resources
// space0, vertex uniforms space1, pixel resources space2, pixel uniforms
// space3; [[vk::]] maps them to Vulkan descriptor sets 0-3.
//
// tools/shaders/build_shaders.py compiles it into mesh_shaders.gen.h; rerun
// it after changing this file.

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
#define SPOT_IN(T) T
#include "spot_params.hlsli"
#include "spot_model.hlsli"

// Milo's matrices are row vectors (v' = v * M); row_major needs no transpose.
VK_BINDING(0, 1) cbuffer VertexUniforms : register(b0, space1) {
    row_major float4x4 world;
    row_major float4x4 view_proj;
    uint skinned;     // bones[bone_base..] place the vertex instead of world
    uint bone_base;
    uint bone_count;
    // shadow map pass: depth is clip z, not kNearW / w
    uint shadow_depth;
    // times clip w: half a pixel for D3D9's pixel centres (PixelCentre); 0
    // for DrawRect's quads
    float4 clip_offset;
    // soft_raster.h's DepthMap: (x w + y + z clip.z) / w in kNearW / w units
    float4 depth_map;
    // where 16:9 ends in clip xy (RasterOptions::overlay_edge); 1 1 off
    float4 overlay_edge;
    ShadeParams vs_shade;
};

// four float4 rows per bone; SDL binds storage buffers as raw views
VK_BINDING(0, 0) ByteAddressBuffer bones : register(t0, space0);

// Textures share arrays by size class, one per layer, in its corner of every
// level (gpu_view.cpp), so the shader loads and filters texels itself
// (sample_model.hlsli): a hardware sampler couldn't wrap within a corner. The
// samplers are only SDL_gpu's pairing.
VK_SAMPLER VK_BINDING(0, 2) Texture2DArray<float4> tex : register(t0, space2);
VK_SAMPLER VK_BINDING(0, 2) SamplerState tex_sampler : register(s0, space2);
VK_SAMPLER VK_BINDING(1, 2) Texture2DArray<float4> spec_tex : register(t1, space2);
VK_SAMPLER VK_BINDING(1, 2) SamplerState spec_sampler : register(s1, space2);
VK_SAMPLER VK_BINDING(2, 2) Texture2DArray<float4> glow_tex : register(t2, space2);
VK_SAMPLER VK_BINDING(2, 2) SamplerState glow_sampler : register(s2, space2);
VK_SAMPLER VK_BINDING(3, 2) Texture2DArray<float4> proj_tex : register(t3, space2);
VK_SAMPLER VK_BINDING(3, 2) SamplerState proj_sampler : register(s3, space2);
VK_SAMPLER VK_BINDING(4, 2) Texture2DArray<float4> gobo_tex : register(t4, space2);
VK_SAMPLER VK_BINDING(4, 2) SamplerState gobo_sampler : register(s4, space2);
// the resolved picture, for kShadeRefract (1x1 stand-in otherwise)
VK_SAMPLER VK_BINDING(5, 2) Texture2D<float4> behind_tex : register(t5, space2);
VK_SAMPLER VK_BINDING(5, 2) SamplerState behind_sampler : register(s5, space2);
// kShadeShadow's map, clip z/w in R32_FLOAT (1x1 stand-in otherwise)
VK_SAMPLER VK_BINDING(6, 2) Texture2D<float> shadow_tex : register(t6, space2);
VK_SAMPLER VK_BINDING(6, 2) SamplerState shadow_sampler : register(s6, space2);
VK_SAMPLER VK_BINDING(7, 2) Texture2DArray<float4> normal_tex : register(t7, space2);
VK_SAMPLER VK_BINDING(7, 2) SamplerState normal_sampler : register(s7, space2);
VK_SAMPLER VK_BINDING(8, 2) Texture2DArray<float4> detail_tex : register(t8, space2);
VK_SAMPLER VK_BINDING(8, 2) SamplerState detail_sampler : register(s8, space2);

// pixel_flags.x: SrcAlpha and SrcAlphaAdd premultiply in FinishMesh, after
// the clamp the target would do
static const uint kPremultiply = 1;

VK_BINDING(0, 3) cbuffer PixelUniforms : register(b0, space3) {
    ShadeParams ps_shade;
    // diffuse, specular, glow, projected ([0].xyzw); gobo, normal, detail
    // ([1].xyz)
    uint4 tex_layer[2];
    // xy, smaller than the layer: the first five in that order, then shadow,
    // normal, detail
    uint4 tex_size[8];
    // x: kPremultiply; y: bit per map (tex_layer's order) for DXN kept as BC5;
    // z, w: R8 maps' expand codes, a byte each, maps 0-3 in z, 4-6 in w
    uint4 pixel_flags;
    // PackSampler's, in tex_layer's order
    uint4 tex_samp[8];
};

// For PSSpotCone, which still takes its texture and alpha cut from
// PixelUniforms. PSSoftParticle uses the viewport, size and
// spot.depth_range.y (the far plane).
VK_BINDING(1, 3) cbuffer SpotUniforms : register(b1, space3) {
    SpotParams spot;
    float4 spot_viewport;  // x, y, 1/width, 1/height
    uint4 spot_sizes;      // the scene depth's width and height (xy)
};

// The scene's depth (kNearW / w, 0 where nothing drew) and the density map
// (one layer); PSMain doesn't read them.
VK_SAMPLER VK_BINDING(9, 2) Texture2D<float> scene_depth_tex : register(t9, space2);
VK_SAMPLER VK_BINDING(9, 2) SamplerState scene_depth_sampler : register(s9, space2);
VK_SAMPLER VK_BINDING(10, 2) Texture2DArray<float4> density_tex : register(t10, space2);
VK_SAMPLER VK_BINDING(10, 2) SamplerState density_sampler : register(s10, space2);

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
    VK_LOCATION(6) float4 tan : TEXCOORD6;  // w the handedness
};

struct PixelIn {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float3 nrm : TEXCOORD1;
    float4 color : TEXCOORD2;
    float3 wpos : TEXCOORD3;
    float depth : TEXCOORD4;           // clip w
    float3 light_diffuse : TEXCOORD5;  // a vertex-lit draw's Lighting
    float3 light_added : TEXCOORD6;
    float2 ao_sh : TEXCOORD7;
    float3 tan : TEXCOORD8;
    float3 bitan : TEXCOORD9;
    float2 clip : TEXCOORD10;  // before clip_offset, for RefractUv
};

float4x4 Bone(uint i) {
    const uint at = (bone_base + i) * 64;
    return float4x4(asfloat(bones.Load4(at)), asfloat(bones.Load4(at + 16)),
                    asfloat(bones.Load4(at + 32)), asfloat(bones.Load4(at + 48)));
}

// native_fill_window: a vertex past 16:9 (edge, clip xy) moves out to the
// picture's edge, so menu art drawn slightly past 16:9 reaches the window's
float2 StretchEdges(float4 clip, float2 edge) {
    float2 xy = clip.xy;
    [unroll] for (int a = 0; a < 2; a++) {
        if (clip.w > 0 && edge[a] < 1 && abs(xy[a]) > edge[a] * clip.w)
            xy[a] = sign(xy[a]) * clip.w;
    }
    return xy;
}

PixelIn VSMain(VertexIn v) {
    const float3 dir = AoShDirection(v.color);
    const bool mapped = (vs_shade.flags.x & kShadeNormalMap) != 0u;
    float3 nrm = v.nrm, tangent = 0;
    if (mapped) {
        const TangentFrame f = TextureFrame(vs_shade, v.nrm, v.tan);
        nrm = f.n;
        tangent = f.u;
    }
    float3 wp = 0, wn = 0, wd = 0, wu = 0;
    if (skinned != 0) {
        float total = 0;
        [unroll] for (int k = 0; k < 4; k++) {
            const float w = v.weight[k];
            if (w <= 0) continue;
            const float4x4 b = Bone(v.bone[k] < bone_count ? v.bone[k] : 0);
            wp += mul(float4(v.pos, 1), b).xyz * w;
            wn += mul(float4(nrm, 0), b).xyz * w;
            wd += mul(float4(dir, 0), b).xyz * w;
            wu += mul(float4(tangent, 0), b).xyz * w;
            total += w;
        }
        if (total <= 0) {
            const float4x4 b = Bone(0);
            wp = mul(float4(v.pos, 1), b).xyz;
            wn = mul(float4(nrm, 0), b).xyz;
            wd = mul(float4(dir, 0), b).xyz;
            wu = mul(float4(tangent, 0), b).xyz;
        }
    } else if ((vs_shade.flags.x & kShadeBillboard) != 0u) {
        wp = Billboard(vs_shade, v.pos) + world[3].xyz;
        wn = Billboard(vs_shade, nrm);
        wd = Billboard(vs_shade, dir);
        wu = Billboard(vs_shade, tangent);
    } else {
        wp = mul(float4(v.pos, 1), world).xyz;
        wn = mul(float4(nrm, 0), world).xyz;
        wd = mul(float4(dir, 0), world).xyz;
        wu = mul(float4(tangent, 0), world).xyz;
    }
    float4 clip = mul(float4(wp, 1), view_proj);
    clip.xy = StretchEdges(clip, overlay_edge.xy);
    PixelIn o;
    o.clip = clip.xy;
    clip.xy += clip_offset.xy * clip.w;
    // kNearW / w, the CPU's 1/w scaled: larger is nearer, no far plane,
    // whatever the game's projection; a back-buffer draw's through depth_map.
    // A shadow map keeps clip z, clipped at 0 as the game's device does.
    const float depth =
        kNearW * (depth_map.x * clip.w + depth_map.y + depth_map.z * clip.z);
    o.pos = float4(clip.xy, shadow_depth != 0u ? clip.z : depth, clip.w);
    o.uv = TexGen(vs_shade, v.uv);
    o.nrm = wn;
    o.color = v.color;
    o.wpos = wp;
    o.depth = clip.w;
    o.ao_sh = AoShVertex(vs_shade, wp, wn, wd, v.color);
    o.tan = wu;
    o.bitan = mapped ? Bitangent(wn, wu, v.tan.w) : float3(0, 0, 0);
    o.light_diffuse = float3(0, 0, 0);
    o.light_added = float3(0, 0, 0);
    if ((vs_shade.flags.x & kShadePerVertex) != 0u) {
        const Lighting l = Light(vs_shade, wp, wn, wn, v.color, float4(1, 1, 1, 1), o.ao_sh,
                                 float4(0, 0, 0, 0), float4(0, 0, 0, 0), 1.0, float3(0, 0, 0));
        o.light_diffuse = l.diffuse;
        o.light_added = l.added;
    }
    return o;
}

// nearest, wrapping; soft_raster.cpp's Texel()
float4 Texel(Texture2DArray<float4> t, float2 uv, uint layer, uint2 size) {
    const float2 f = uv - floor(uv);
    const uint2 at = min(uint2(f * float2(size)), size - 1);
    return t.Load(int4(at, layer, 0));
}

#define SAMPLE_TEX Texture2DArray<float4> t, uint layer
#define SAMPLE_ARGS t, layer
#define SAMPLE_LOAD(level, x, y) t.Load(int4(x, y, layer, level))
#define SAMPLE_LOOP [loop]
#include "sample_model.hlsli"

// by the game's sampler s, else nearest at level 0 (soft_raster.cpp's Read)
float4 ReadTexture(Texture2DArray<float4> t, uint layer, uint2 size, uint4 s, float2 uv,
                   float2 dx, float2 dy) {
    if ((s.x & kSampleFiltered) == 0u) return Texel(t, uv, layer, size);
    return SampleTexture(t, layer, size, s, uv, dx, dy);
}

// Filtered map k (tex_layer's order) as the CPU decodes it. Rearranging
// components commutes with filtering, as each is filtered alone and border
// taps are uniform. DXN kept as BC5 loads as (x, y, 0, 1), but DecodeBlock
// repeats y into z and w as a fetch would; a head's normal map drawn as a
// texture pass's diffuse has its alpha read. k_8 kept as R8 loads as
// (byte, 0, 0, 1); DecodeLevel8 swizzles it: the code (R8ExpandCode) has two
// bits a component, x's lowest: 1 byte, 2 zero, 3 one; 0 for other textures.
float4 MapTexel(float4 c, uint k) {
    if (((pixel_flags.y >> k) & 1u) != 0u) return c.xyyy;
    const uint code = ((k < 4u ? pixel_flags.z : pixel_flags.w) >> (8u * (k & 3u))) & 255u;
    if (code == 0u) return c;
    float4 o;
    [unroll] for (int i = 0; i < 4; i++) {
        const uint v = (code >> (2 * i)) & 3u;
        o[i] = v == 1u ? c.x : v == 2u ? c.y : c.w;
    }
    return o;
}

// bilinear, transparent black border; soft_raster.cpp's SampleBorder()
float4 ProjTexel(Texture2DArray<float4> t, float2 uv, uint layer, uint2 size) {
    // beyond 2 every tap is the border; NaN (on the light's plane) is too
    if (!(abs(uv.x) < 2.0 && abs(uv.y) < 2.0)) return float4(0, 0, 0, 0);
    const float2 xy = uv * float2(size) - 0.5;
    const float2 f = floor(xy);
    const float2 w = xy - f;
    const int2 at = int2(f);
    float4 c[4];
    [unroll] for (int k = 0; k < 4; k++) {
        const int2 q = at + int2(k & 1, k >> 1);
        c[k] = float4(0, 0, 0, 0);
        if (q.x >= 0 && q.y >= 0 && q.x < int(size.x) && q.y < int(size.y))
            c[k] = t.Load(int4(q, layer, 0));
    }
    const float4 top = c[0] + (c[1] - c[0]) * w.x;
    const float4 bottom = c[2] + (c[3] - c[2]) * w.x;
    return top + (bottom - top) * w.y;
}

// bilinear, clamped; soft_raster.cpp's SampleLinear
float4 BehindTexel(float2 uv) {
    uint w, h;
    behind_tex.GetDimensions(w, h);
    const float2 xy = uv * float2(w, h) - 0.5;
    const float2 f = floor(xy);
    const float2 t = xy - f;
    const int2 at = int2(f);
    float4 c[4];
    [unroll] for (int k = 0; k < 4; k++) {
        const int2 q = clamp(at + int2(k & 1, k >> 1), int2(0, 0), int2(w, h) - 1);
        c[k] = behind_tex.Load(int3(q, 0));
    }
    const float4 top = c[0] + (c[1] - c[0]) * t.x;
    const float4 bottom = c[2] + (c[3] - c[2]) * t.x;
    return top + (bottom - top) * t.y;
}

// before FinishMesh
float4 MeshColor(PixelIn i) {
    const uint f = ps_shade.flags.x;
    // before anything branches; RasterTri works out the same ones
    const float2 dx = ddx_fine(i.uv);
    const float2 dy = ddy_fine(i.uv);
    const float2 detail_uv = DetailUv(ps_shade, i.uv);
    const float2 detail_dx = ddx_fine(detail_uv);
    const float2 detail_dy = ddy_fine(detail_uv);
    float4 texel = float4(1, 1, 1, 1);
    float4 spec_map = float4(1, 1, 1, 1);
    float4 glow = float4(0, 0, 0, 0);
    if ((f & kShadeTextured) != 0u)
        texel = MapTexel(
            ReadTexture(tex, tex_layer[0].x, tex_size[0].xy, tex_samp[0], i.uv, dx, dy), 0);
    if ((f & kShadeSpecMap) != 0u)
        spec_map = MapTexel(ReadTexture(spec_tex, tex_layer[0].y, tex_size[1].xy, tex_samp[1],
                                        i.uv, dx, dy),
                            1);
    if ((f & kShadeGlow) != 0u)
        glow = MapTexel(
            ReadTexture(glow_tex, tex_layer[0].z, tex_size[2].xy, tex_samp[2], i.uv, dx, dy), 2);
    float4 normal = float4(0.5, 0.5, 0, 1);
    float4 detail = float4(0.5, 0.5, 0, 1);
    // s1: the normal map, or REFRACT_WORLD's refract normal map
    if ((f & (kShadeNormalMap | kShadeRefractMap)) != 0u)
        normal = MapTexel(ReadTexture(normal_tex, tex_layer[1].y, tex_size[6].xy, tex_samp[5],
                                      i.uv, dx, dy),
                          5);
    if ((f & kShadeNormalMap) != 0u && (f & kShadeDetailMap) != 0u)
        detail = MapTexel(ReadTexture(detail_tex, tex_layer[1].z, tex_size[7].xy, tex_samp[6],
                                      detail_uv, detail_dx, detail_dy),
                          6);
    float4 proj = float4(0, 0, 0, 0);
    float4 gobo = float4(0, 0, 0, 0);
    if ((f & (kShadeProjMultiply | kShadeProjGobo)) != 0u) {
        const float2 puv = ProjUv(ps_shade, i.wpos);
        proj = MapTexel(ProjTexel(proj_tex, puv, tex_layer[0].w, tex_size[3].xy), 3);
        if ((f & kShadeProjGobo) != 0u)
            gobo = MapTexel(ProjTexel(gobo_tex, puv, tex_layer[1].x, tex_size[4].xy), 4);
    }
    float4 behind = float4(1, 1, 1, 1);
    if ((f & kShadeRefract) != 0u)
        behind = BehindTexel(RefractUv(ps_shade, i.clip, i.depth, normal));
    Lighting vertex;
    vertex.diffuse = i.light_diffuse;
    vertex.added = i.light_added;
    // as soft_raster.cpp's ShadowLitCpu
    float lit = 1.0;
    if ((f & kShadeShadow) != 0u) {
        const ShadowTapSet t = ShadowTaps(ShadowCoord(ps_shade, i.wpos), float2(tex_size[5].xy));
        const float4 stored = float4(shadow_tex.Load(int3(int(t.x.x), int(t.y.x), 0)),
                                     shadow_tex.Load(int3(int(t.x.y), int(t.y.y), 0)),
                                     shadow_tex.Load(int3(int(t.x.z), int(t.y.z), 0)),
                                     shadow_tex.Load(int3(int(t.x.w), int(t.y.w), 0)));
        lit = ShadowLit(t, stored);
    }
    return ShadePixel(ps_shade, i.wpos, i.nrm, i.tan, i.bitan, i.color, texel, spec_map, glow,
                      normal, detail, behind, i.depth, i.ao_sh, proj, gobo, lit, vertex);
}

float4 FinishMesh(float4 c) {
    if (AlphaCut(ps_shade, c.a)) discard;
    // clamp first, as a UNORM target would before blending (soft_raster.cpp's
    // Blend())
    if ((pixel_flags.x & kPremultiply) != 0u) c.rgb = saturate(c.rgb) * saturate(c.a);
    return c;
}

float4 PSMain(PixelIn i) : SV_Target0 { return FinishMesh(MeshColor(i)); }

// RndShadowMap::PrepShadow's draws (draw mode 1): clip z/w into R32_FLOAT
float PSShadowDepth(PixelIn i) : SV_Target0 { return i.pos.z; }

// 0..1 across the viewport; SV_Position is half a pixel past the game's
// sample (clip_offset)
float2 ScreenUv(float4 pos) { return (pos.xy - 0.5 - spot_viewport.xy) * spot_viewport.zw; }

// 1/w, 0 where nothing drew; soft_raster.cpp's SceneInvW
float SceneInvW(float2 uv) {
    const uint2 size = spot_sizes.xy;
    const uint2 at = min(uint2(saturate(uv) * float2(size)), size - 1);
    return scene_depth_tex.Load(int3(at, 0)) / kNearW;
}

// A spotlight cone's proxy pixel, as soft_raster.cpp's SpotPixel. Alpha 0,
// which the Add pipeline leaves as the clear's.
float4 PSSpotCone(PixelIn i) : SV_Target0 {
    const float2 uv = ScreenUv(i.pos);
    const float inv_w = SceneInvW(uv);
    float xsec = 1.0;
    if ((ps_shade.flags.x & kShadeTextured) != 0u) {
        const uint2 tsize = tex_size[0].xy;
        const float k = saturate(SpotGoboCoord(spot, i.wpos));
        const uint2 t = min(uint2(uint(k * float(tsize.x)), 0), tsize - 1);
        xsec = MapTexel(tex.Load(int4(t, tex_layer[0].x, 0)), 0).x;
    }
    const float density = density_tex.SampleLevel(density_sampler, float3(uv, 0.0), 0).y;
    if (AlphaCut(ps_shade, 0.0)) discard;
    const float3 c =
        SpotCone(spot, i.wpos, i.depth, SpotSceneDepth(spot, inv_w), xsec, density);
    return float4(c, 0.0);
}

// IsSoftParticle draws, as soft_raster.cpp's SoftPixelFade
float4 PSSoftParticle(PixelIn i) : SV_Target0 {
    float4 c = MeshColor(i);
    c.a *= SoftFade(SoftSceneDepth(spot.depth_range.y, SceneInvW(ScreenUv(i.pos))), i.depth);
    return FinishMesh(c);
}
