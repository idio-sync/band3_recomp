// Experimental: the native view's mesh shader, for the GPU backend
// (gpu_view.cpp). It draws what soft_raster.cpp draws, the same way: Shade()
// there is the reference for the pixel shader.
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

// Milo's matrices are row vectors (v' = v * M), and the uniforms arrive as
// they are in memory, so mul(v, M) with row_major needs no transpose.
VK_BINDING(0, 1) cbuffer VertexUniforms : register(b0, space1) {
    row_major float4x4 world;
    row_major float4x4 view_proj;
    uint skinned;     // bones[bone_base..] place the vertex instead of world
    uint bone_base;
    uint bone_count;
    uint vertex_pad;
};

// the frame's bone matrices, four float4 rows each; SDL binds storage buffers
// as raw views, so a ByteAddressBuffer reads them as they are
VK_BINDING(0, 0) ByteAddressBuffer bones : register(t0, space0);

// Textures share arrays by size class, a texture to a layer, in its corner
// (gpu_view.cpp). The sampler is SDL_gpu's pairing; the shader reads texels
// itself, as Shade() does
VK_SAMPLER VK_BINDING(0, 2) Texture2DArray<float4> tex : register(t0, space2);
VK_SAMPLER VK_BINDING(0, 2) SamplerState tex_sampler : register(s0, space2);

static const uint kTextured = 1;
static const uint kPrelit = 2;
static const uint kLighting = 4;
static const uint kAlphaCut = 8;
// SrcAlpha and SrcAlphaAdd: the colour leaves already scaled by its alpha,
// which the blend can't clamp first (gpu_view.cpp's pipelines)
static const uint kPremultiply = 16;

VK_BINDING(0, 3) cbuffer PixelUniforms : register(b0, space3) {
    float4 mat_color;
    uint flags;
    float alpha_threshold;  // 0-255, as RndMat keeps it
    uint tex_layer;
    uint pixel_pad;
    uint2 tex_size;  // the texture's own; its layer may be bigger
    uint2 pixel_pad2;
};

// soft_raster.cpp's near plane: w below it is clipped
static const float kNearW = 1e-3;
static const float3 kLight = float3(0.39, -0.59, 0.71);  // Milo is z up

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
};

float4x4 Bone(uint i) {
    const uint at = (bone_base + i) * 64;
    return float4x4(asfloat(bones.Load4(at)), asfloat(bones.Load4(at + 16)),
                    asfloat(bones.Load4(at + 32)), asfloat(bones.Load4(at + 48)));
}

PixelIn VSMain(VertexIn v) {
    float3 wp = 0, wn = 0;
    if (skinned != 0) {
        float total = 0;
        [unroll] for (int k = 0; k < 4; k++) {
            const float w = v.weight[k];
            if (w <= 0) continue;
            const float4x4 b = Bone(v.bone[k] < bone_count ? v.bone[k] : 0);
            wp += mul(float4(v.pos, 1), b).xyz * w;
            wn += mul(float4(v.nrm, 0), b).xyz * w;
            total += w;
        }
        if (total <= 0) {
            const float4x4 b = Bone(0);
            wp = mul(float4(v.pos, 1), b).xyz;
            wn = mul(float4(v.nrm, 0), b).xyz;
        }
    } else {
        wp = mul(float4(v.pos, 1), world).xyz;
        wn = mul(float4(v.nrm, 0), world).xyz;
    }
    const float4 clip = mul(float4(wp, 1), view_proj);
    PixelIn o;
    // depth is kNearW / w, the CPU's 1/w scaled: z/w interpolates as 1/w does,
    // larger is nearer, the near plane is w = kNearW and there's no far plane,
    // whatever depth range the game's projection has
    o.pos = float4(clip.xy, kNearW, clip.w);
    o.uv = v.uv;
    o.nrm = wn;
    o.color = v.color;
    return o;
}

float4 PSMain(PixelIn i) : SV_Target0 {
    float4 c = mat_color;
    if (flags & kTextured) {
        // Shade()'s nearest texel, wrapping, by the same arithmetic
        const float2 f = i.uv - floor(i.uv);
        const uint2 t = min(uint2(f * float2(tex_size)), tex_size - 1);
        c *= tex.Load(int4(t, tex_layer, 0));
    }
    if (flags & kPrelit) {
        c *= i.color;
    } else if (flags & kLighting) {
        const float len = length(i.nrm);
        const float d = len > 1e-6 ? dot(i.nrm, kLight) / len : 0;
        c.rgb *= 0.4 + 0.6 * max(0, d);
    }
    if ((flags & kAlphaCut) && c.a * 255 < alpha_threshold) discard;
    // Blend() in soft_raster.cpp clamps alpha, never the colour, before
    // scaling by it; a UNORM target clamps what reaches the blender, so the
    // scaling happens here, where colour above 1 still counts
    if (flags & kPremultiply) c.rgb *= saturate(c.a);
    return c;
}
