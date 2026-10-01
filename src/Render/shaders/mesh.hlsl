// Experimental: the native view's mesh shader, for the GPU backend
// (gpu_view.cpp). It draws what soft_raster.cpp draws, the same way: the
// shading itself is shade.hlsli, which the CPU compiles too, from the same
// ShadeParams (shade_model.cpp packs them); a spotlight's cone shades by
// spot_model.hlsli instead (PSSpotCone), from SpotParams (spot_model.cpp),
// and a soft particle is a mesh's shading faded by the scene's depth
// (PSSoftParticle).
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
#define SPOT_IN(T) T
#include "spot_params.hlsli"
#include "spot_model.hlsli"

// Milo's matrices are row vectors (v' = v * M), and the uniforms arrive as
// they are in memory, so mul(v, M) with row_major needs no transpose.
VK_BINDING(0, 1) cbuffer VertexUniforms : register(b0, space1) {
    row_major float4x4 world;
    row_major float4x4 view_proj;
    uint skinned;     // bones[bone_base..] place the vertex instead of world
    uint bone_base;
    uint bone_count;
    uint vertex_pad;
    // added to the clip position's x, y times its w: (1/width, -1/height) of
    // the viewport, half a pixel right and down, for a draw on D3D9's pixel
    // centres (soft_raster.cpp's PixelCentre); 0 for DrawRect's quads
    float4 clip_offset;
    ShadeParams vs_shade;  // the texture's transform, and a vertex-lit draw's light
};

// the frame's bone matrices, four float4 rows each; SDL binds storage buffers
// as raw views, so a ByteAddressBuffer reads them as they are
VK_BINDING(0, 0) ByteAddressBuffer bones : register(t0, space0);

// Textures share arrays by size class, a texture to a layer, in its corner
// (gpu_view.cpp): the diffuse texture, the specular map, the glow map and the
// projected light's two (s5, and the gobo s10). The samplers are SDL_gpu's
// pairing; the shader reads texels itself, as soft_raster.cpp's Texel() and
// SampleBorder() do
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
// a copy of the picture as the resolve left it, for kShadeRefract (a 1x1
// stand-in for the others): read at the pixel's own position, as Shade() in
// soft_raster.cpp does
VK_SAMPLER VK_BINDING(5, 2) Texture2D<float4> behind_tex : register(t5, space2);
VK_SAMPLER VK_BINDING(5, 2) SamplerState behind_sampler : register(s5, space2);

// pixel_flags.x
// SrcAlpha and SrcAlphaAdd: the colour leaves already scaled by its alpha,
// which the blend can't clamp first (gpu_view.cpp's pipelines)
static const uint kPremultiply = 1;

VK_BINDING(0, 3) cbuffer PixelUniforms : register(b0, space3) {
    ShadeParams ps_shade;
    // the diffuse texture's, the specular map's, the glow map's and the
    // projected light's ([0].xyzw), the gobo's ([1].x)
    uint4 tex_layer[2];
    uint4 tex_size[5];  // and their own sizes (xy), in that order; their layers may be bigger
    uint4 pixel_flags;
};

// A spotlight's cone (PSSpotCone): its numbers, where its pass's viewport is
// (the pixel's place on the screen, at which it reads the scene's depth and
// the density map) and the scene depth's size. A second uniform buffer, so
// PixelUniforms stays the mesh's: the cone takes its cross-section texture's
// layer and size, and the alpha cut, from there. A soft particle
// (PSSoftParticle) takes the viewport and size from it too, and the camera's
// far plane from spot.depth_range (the same c89), the rest of `spot` unused.
VK_BINDING(1, 3) cbuffer SpotUniforms : register(b1, space3) {
    SpotParams spot;
    float4 spot_viewport;  // x, y, 1/width, 1/height
    uint4 spot_sizes;      // the scene depth's width and height (xy)
};

// and what it reads besides its cross-section texture (tex): the scene's
// depth as the world's draws left it (kNearW / w, 0 where nothing drew) and
// the density map (a texture pass's target, an array of one layer). The
// slots after the mesh's six, which PSMain doesn't read; PSSoftParticle reads
// the scene's depth.
VK_SAMPLER VK_BINDING(6, 2) Texture2D<float> scene_depth_tex : register(t6, space2);
VK_SAMPLER VK_BINDING(6, 2) SamplerState scene_depth_sampler : register(s6, space2);
VK_SAMPLER VK_BINDING(7, 2) Texture2DArray<float4> density_tex : register(t7, space2);
VK_SAMPLER VK_BINDING(7, 2) SamplerState density_sampler : register(s7, space2);

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
    float4 clip = mul(float4(wp, 1), view_proj);
    // the pixel this pipeline samples at x + .5 then sees what the game's
    // sampled at x
    clip.xy += clip_offset.xy * clip.w;
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
        const Lighting l = Light(vs_shade, wp, wn, v.color, float4(1, 1, 1, 1), o.ao_sh,
                                 float4(0, 0, 0, 0), float4(0, 0, 0, 0));
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

// bilinear at uv, outside the texture a transparent black border, by
// soft_raster.cpp's SampleBorder()'s arithmetic: the projected light's maps
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

// What a mesh's pixel shades to (shade.hlsli's ShadePixel), before the alpha
// cut and the blend's premultiply (FinishMesh)
float4 MeshColor(PixelIn i) {
    const uint f = ps_shade.flags.x;
    float4 texel = float4(1, 1, 1, 1);
    float4 spec_map = float4(1, 1, 1, 1);
    float4 glow = float4(0, 0, 0, 0);
    if ((f & kShadeTextured) != 0u) texel = Texel(tex, i.uv, tex_layer[0].x, tex_size[0].xy);
    if ((f & kShadeSpecMap) != 0u)
        spec_map = Texel(spec_tex, i.uv, tex_layer[0].y, tex_size[1].xy);
    if ((f & kShadeGlow) != 0u) glow = Texel(glow_tex, i.uv, tex_layer[0].z, tex_size[2].xy);
    float4 proj = float4(0, 0, 0, 0);
    float4 gobo = float4(0, 0, 0, 0);
    if ((f & (kShadeProjMultiply | kShadeProjGobo)) != 0u) {
        const float2 puv = ProjUv(ps_shade, i.wpos);
        proj = ProjTexel(proj_tex, puv, tex_layer[0].w, tex_size[3].xy);
        if ((f & kShadeProjGobo) != 0u)
            gobo = ProjTexel(gobo_tex, puv, tex_layer[1].x, tex_size[4].xy);
    }
    // SV_Position is the pixel's centre: its integer part is the pixel
    float4 behind = float4(1, 1, 1, 1);
    if ((f & kShadeRefract) != 0u) behind = behind_tex.Load(int3(int2(i.pos.xy), 0));
    Lighting vertex;
    vertex.diffuse = i.light_diffuse;
    vertex.added = i.light_added;
    return ShadePixel(ps_shade, i.wpos, i.nrm, i.color, texel, spec_map, glow, behind, i.depth,
                      i.ao_sh, proj, gobo, vertex);
}

float4 FinishMesh(float4 c) {
    if (AlphaCut(ps_shade, c.a)) discard;
    // Blend() in soft_raster.cpp clamps alpha, never the colour, before
    // scaling by it; a UNORM target clamps what reaches the blender, so the
    // scaling happens here, where colour above 1 still counts
    if ((pixel_flags.x & kPremultiply) != 0u) c.rgb *= saturate(c.a);
    return c;
}

float4 PSMain(PixelIn i) : SV_Target0 { return FinishMesh(MeshColor(i)); }

// Where a texture pass's pixel is on the screen, 0..1 across its viewport:
// SV_Position is the pixel's centre, half a pixel past where the game's
// sampled it (VSMain's clip_offset)
float2 ScreenUv(float4 pos) { return (pos.xy - 0.5 - spot_viewport.xy) * spot_viewport.zw; }

// the scene's depth there (1/w, 0 where nothing drew), nearest, clamped:
// soft_raster.cpp's SceneInvW
float SceneInvW(float2 uv) {
    const uint2 size = spot_sizes.xy;
    const uint2 at = min(uint2(saturate(uv) * float2(size)), size - 1);
    return scene_depth_tex.Load(int3(at, 0)) / kNearW;
}

// What a spotlight's cone adds to the depth volume at a pixel of its proxy,
// as soft_raster.cpp's SpotPixel works it out: the scene's depth read at the
// pixel's place on the screen (nearest, clamped), the cross-section texture
// at SpotGoboCoord's (nearest, clamped; 1 untextured), the density map
// bilinear. Alpha 0, which the Add pipeline leaves as the clear's.
float4 PSSpotCone(PixelIn i) : SV_Target0 {
    const float2 uv = ScreenUv(i.pos);
    const float inv_w = SceneInvW(uv);
    float xsec = 1.0;
    if ((ps_shade.flags.x & kShadeTextured) != 0u) {
        const uint2 tsize = tex_size[0].xy;
        const float k = saturate(SpotGoboCoord(spot, i.wpos));
        const uint2 t = min(uint2(uint(k * float(tsize.x)), 0), tsize - 1);
        xsec = tex.Load(int4(t, tex_layer[0].x, 0)).x;
    }
    const float density = density_tex.SampleLevel(density_sampler, float3(uv, 0.0), 0).y;
    if (AlphaCut(ps_shade, 0.0)) discard;
    const float3 c =
        SpotCone(spot, i.wpos, i.depth, SpotSceneDepth(spot, inv_w), xsec, density);
    return float4(c, 0.0);
}

// A soft particle (scene_capture.h's IsSoftParticle) into the soft-particle
// buffer: shaded as PSMain shades it, its alpha faded by the scene's depth
// where its pixel is on the screen (shade.hlsli's SoftFade), as
// soft_raster.cpp's SoftPixelFade does
float4 PSSoftParticle(PixelIn i) : SV_Target0 {
    float4 c = MeshColor(i);
    c.a *= SoftFade(SoftSceneDepth(spot.depth_range.y, SceneInvW(ScreenUv(i.pos))), i.depth);
    return FinishMesh(c);
}
