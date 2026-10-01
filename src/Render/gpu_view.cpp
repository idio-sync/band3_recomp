#include "src/Render/gpu_view.h"

#include "src/Render/shaders/mesh_shaders.gen.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#ifdef _WIN32
// for SDL_RegisterApp only; band3 has its own main
#define SDL_MAIN_HANDLED
#define SDL_MAIN_NOIMPL
#include <SDL3/SDL_main.h>
#endif
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// See gpu_view.h.

namespace band3::render {
namespace {

constexpr SDL_GPUTextureFormat kColorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
constexpr SDL_GPUTextureFormat kDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
// soft_raster.cpp's clear colour, 0xff202020
constexpr float kClearGrey = float(0x20) / 255.0f;
// a mesh or texture no frame has drawn for this many frames is let go
constexpr uint64_t kEvictAfter = 120;
// A texture array of a size class starts with layers to about this many
// bytes, and doubles when full. Making a texture costs about half a
// millisecond, so textures share them rather than have one each.
constexpr uint32_t kTextureArrayBytes = 4u << 20;
constexpr uint32_t kMaxTextureLayers = 2048;  // Direct3D 12's limit
// Direct3D 12 copies texture rows from an upload at this pitch, starting at an
// offset aligned to kTextureOffsetAlign; textures are laid out so
constexpr uint32_t kRowPitchAlign = 256;
constexpr uint32_t kTextureOffsetAlign = 512;
// the arena's least size; it's made half again as big as it needs
constexpr uint32_t kMinArenaBytes = 4u << 20;

// mesh.hlsl's cbuffers, as they lie in memory
struct VertexUniforms {
    Mat4 world;
    Mat4 view_proj;
    uint32_t skinned;
    uint32_t bone_base;
    uint32_t bone_count;
    uint32_t pad;
};
static_assert(sizeof(VertexUniforms) == 144);

struct PixelUniforms {
    float color[4];
    uint32_t flags;
    float alpha_threshold;
    uint32_t tex_layer;
    uint32_t pad;
    uint32_t tex_size[2];
    uint32_t pad2[2];
};
static_assert(sizeof(PixelUniforms) == 48);

// mesh.hlsl's flags
enum : uint32_t { kTextured = 1, kPrelit = 2, kLighting = 4, kAlphaCut = 8, kPremultiply = 16 };

// RndMat::Blend, the modes Blend() in soft_raster.cpp draws
enum : int {
    kBlendDest = 0,
    kBlendSrc = 1,
    kBlendAdd = 2,
    kBlendSrcAlpha = 3,
    kBlendSrcAlphaAdd = 4,
    kBlendSubtract = 5,
    kBlendMultiply = 6,
};

int BlendFor(const DrawItem& it, const RasterOptions& o) {
    // Blend() draws any other value as Src
    if (!o.blending || it.blend < kBlendDest || it.blend > kBlendMultiply) return kBlendSrc;
    return it.blend;
}

bool Drawable(const DrawItem& it) {
    return it.geom && !it.geom->verts.empty() && it.geom->indices.size() >= 3;
}

bool Skinned(const DrawItem& it, const RasterOptions& o) {
    return o.skinning && !it.bones.empty();
}

// soft_raster.cpp's depth rules for a draw, which pick its pipeline
struct DepthRules {
    bool test, equal_passes, write;
    int Key() const { return int(test) | int(equal_passes) << 1 | int(write) << 2; }
};

DepthRules RulesFor(const DrawItem& it, const RasterOptions& o) {
    DepthRules r;
    switch (it.z_mode) {
        case 0: r = {false, false, false}; break;
        case 2: r = {true, true, false}; break;
        case 3: r = {false, false, true}; break;
        case 4: r = {true, true, true}; break;
        default: r = {true, false, true}; break;
    }
    if (!o.blending) r.test = r.write = true;
    return r;
}

std::string FormatNames(SDL_GPUShaderFormat f) {
    std::string s;
    auto add = [&](SDL_GPUShaderFormat bit, const char* name) {
        if (!(f & bit)) return;
        if (!s.empty()) s += "/";
        s += name;
    };
    add(SDL_GPU_SHADERFORMAT_PRIVATE, "private");
    add(SDL_GPU_SHADERFORMAT_SPIRV, "SPIR-V");
    add(SDL_GPU_SHADERFORMAT_DXBC, "DXBC");
    add(SDL_GPU_SHADERFORMAT_DXIL, "DXIL");
    add(SDL_GPU_SHADERFORMAT_MSL, "MSL");
    add(SDL_GPU_SHADERFORMAT_METALLIB, "metallib");
    return s.empty() ? "none" : s;
}

uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// index counts are kept even, so every copy of indices is whole 4-byte words
uint32_t IndexSlots(const Geometry& g) { return Align(uint32_t(g.indices.size()), 2); }

uint32_t NextPow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

// A texture's size class, the layer size of the array it goes in: powers of
// two that hold it, at least kMinClassSize and at most 2:1, so a frame's
// textures need few arrays
constexpr uint32_t kMinClassSize = 64;

void SizeClass(uint32_t w, uint32_t h, uint32_t& cw, uint32_t& ch) {
    cw = std::max(kMinClassSize, NextPow2(w));
    ch = std::max(kMinClassSize, NextPow2(h));
    cw = std::max(cw, ch / 2);
    ch = std::max(ch, cw / 2);
}

}  // namespace

struct GpuRenderer::Impl {
    std::mutex mutex;  // held through a whole frame
    // read without the mutex, so the UI thread never waits out a frame
    std::atomic<bool> tried{false};
    std::atomic<bool> ready{false};
    bool video_started = false;
#ifdef _WIN32
    bool app_registered = false;
#endif
    SDL_GPUDevice* device = nullptr;
    SDL_GPUShader* vertex_shader = nullptr;
    SDL_GPUShader* pixel_shader = nullptr;
    // by blend mode and DepthRules::Key, made the first time a draw needs one
    std::unordered_map<int, SDL_GPUGraphicsPipeline*> pipelines;
    SDL_GPUSampler* sampler = nullptr;
    SDL_GPUTexture* white = nullptr;     // bound for untextured draws, which don't read it
    SDL_GPUBuffer* no_bones = nullptr;   // one identity bone, bound when nothing is skinned

    SDL_GPUTexture* color = nullptr;
    SDL_GPUTexture* depth = nullptr;
    SDL_GPUTransferBuffer* readback = nullptr;
    uint32_t width = 0, height = 0;

    // Everything a frame sends goes through this one transfer buffer and one
    // copy pass. It's mapped cycling, so a frame never waits on an earlier one
    // still reading it.
    SDL_GPUTransferBuffer* upload = nullptr;
    uint32_t upload_size = 0;

    struct Buffer {
        SDL_GPUBuffer* buffer = nullptr;
        uint32_t size = 0;
    };
    // Geometry drawn in more than one frame lives in the arena, appended to
    // and, when full, rebuilt from the meshes still drawn. Geometry new this
    // frame goes in the frame's pool: mutable meshes and particles are new
    // every frame, and a buffer each costs far more than copying them. Neither
    // makes a GPU buffer per mesh. The pool alternates between two buffers, so
    // geometry the next frame draws again moves to the arena by a copy on the
    // GPU rather than being sent again.
    Buffer arena_verts, arena_indices;
    uint32_t arena_vert_count = 0, arena_index_count = 0;
    Buffer pool_verts[2], pool_indices[2];  // by frame serial & 1
    Buffer bones;  // this frame's skinned draws' bones, one after another

    struct Mesh {
        std::shared_ptr<const Geometry> keep;  // so the key stays this geometry's
        uint64_t first = 0;                    // the frame that first drew it
        uint64_t used = 0;
        bool in_arena = false;
        // where it starts in the arena, or else in its frame's pool
        uint32_t first_vertex = 0;
        uint32_t first_index = 0;
        // when moving to the arena: where it was in the last frame's pool,
        // or ~0u to send it from the CPU
        uint32_t pool_vertex = ~0u;
        uint32_t pool_index = 0;
    };
    std::unordered_map<const Geometry*, Mesh> meshes;

    // the textures of a size class, a layer each
    struct TexArray {
        SDL_GPUTexture* texture = nullptr;
        uint32_t layers = 0;
        std::vector<uint32_t> free;
        uint64_t empty_since = 0;  // when its last texture went, if none are left
    };
    std::unordered_map<uint64_t, TexArray> tex_arrays;
    struct Tex {
        std::shared_ptr<const Texture> keep;
        TexArray* array = nullptr;  // null if it couldn't have a layer
        uint32_t layer = 0;
        uint64_t first = 0;
        uint64_t used = 0;
    };
    std::unordered_map<const Texture*, Tex> textures;
    uint64_t serial = 0;
    bool texture_failure_logged = false;

    // a frame's work, kept between frames so a frame allocates nothing once
    // they've grown
    std::vector<Mesh*> to_pool, to_arena;
    std::vector<Tex*> new_textures;
    struct ArrayCopy {
        SDL_GPUTexture* from;
        SDL_GPUTexture* to;
        uint32_t w, h, layers;
    };
    std::vector<ArrayCopy> array_copies;  // arrays that grew, old into new
    std::vector<Mat4> frame_bones;
    std::vector<uint32_t> bone_base;  // per draw, where its bones start
    std::vector<uint32_t> cams_seen;

    bool StartVideo(const char* driver);
    void StopVideo();
    bool Create();
    // stop_video: on the UI thread only, as SDL wants
    void Release(bool stop_video);
    SDL_GPUShader* MakeShader(SDL_GPUShaderFormat format, SDL_GPUShaderStage stage);
    SDL_GPUGraphicsPipeline* Pipeline(int blend, const DepthRules& rules);
    bool EnsureTargets(uint32_t w, uint32_t h);
    // grows `b` to hold `bytes`; what it held is lost when it grows
    bool Reserve(Buffer& b, SDL_GPUBufferUsageFlags usage, uint32_t bytes);
    void ReleaseBuffer(Buffer& b);
    // places this frame's new arena meshes, rebuilding the arena if they
    // don't fit; false if it couldn't grow
    bool PlaceInArena();
    // a layer of its size class's array for `tx`, growing the array if full
    bool PlaceTexture(Tex& tx);
    void LetTextureGo(Tex& tx);
    bool Render(const FrameCapture& frame, const RasterOptions& o,
                std::vector<uint32_t>& rgba, GpuStats& stats);
    void Evict();
};

// SDL_CreateGPUDevice wants a video subsystem in band3's SDL copy, which is
// otherwise used for audio and HID only
bool GpuRenderer::Impl::StartVideo(const char* driver) {
    if (driver) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, driver);
    } else {
        SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
#ifdef _WIN32
        // the window class rexruntime's copy registers is SDL_app; this copy
        // takes a name of its own so the two can't collide
        if (!app_registered) app_registered = SDL_RegisterApp("band3_native_view", 0, nullptr);
#endif
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        REXLOG_WARN("native view gpu: SDL's {} video didn't start ({})",
                    driver ? driver : "default", SDL_GetError());
        return false;
    }
    video_started = true;
    return true;
}

void GpuRenderer::Impl::StopVideo() {
    if (video_started) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    video_started = false;
#ifdef _WIN32
    if (app_registered) SDL_UnregisterApp();
    app_registered = false;
#endif
}

SDL_GPUShader* GpuRenderer::Impl::MakeShader(SDL_GPUShaderFormat format,
                                             SDL_GPUShaderStage stage) {
    const bool vs = stage == SDL_GPU_SHADERSTAGE_VERTEX;
    SDL_GPUShaderCreateInfo info{};
    if (format == SDL_GPU_SHADERFORMAT_DXBC) {
        info.code = vs ? shaders::kMeshVertexDxbc : shaders::kMeshPixelDxbc;
        info.code_size = vs ? sizeof(shaders::kMeshVertexDxbc) : sizeof(shaders::kMeshPixelDxbc);
    } else {
        info.code = vs ? shaders::kMeshVertexSpirv : shaders::kMeshPixelSpirv;
        info.code_size =
            vs ? sizeof(shaders::kMeshVertexSpirv) : sizeof(shaders::kMeshPixelSpirv);
    }
    info.entrypoint = vs ? "VSMain" : "PSMain";
    info.format = format;
    info.stage = stage;
    info.num_samplers = vs ? 0 : 1;
    info.num_storage_buffers = vs ? 1 : 0;
    info.num_uniform_buffers = 1;
    SDL_GPUShader* s = SDL_CreateGPUShader(device, &info);
    if (!s) {
        REXLOG_WARN("native view gpu: the {} shader didn't load ({})", vs ? "vertex" : "pixel",
                    SDL_GetError());
    }
    return s;
}

bool GpuRenderer::Impl::Create() {
    // offscreen first: it makes no windows at all; the platform's own driver
    // is the fallback
    for (const char* driver : {"offscreen", static_cast<const char*>(nullptr)}) {
        if (!StartVideo(driver)) continue;
        device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXBC | SDL_GPU_SHADERFORMAT_SPIRV,
                                     false, nullptr);
        if (device) break;
        REXLOG_WARN("native view gpu: no device on SDL's {} video ({})",
                    driver ? driver : "default", SDL_GetError());
        StopVideo();
    }
    if (!device) return false;

    const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(device);
    const SDL_GPUShaderFormat format = (formats & SDL_GPU_SHADERFORMAT_DXBC)
                                           ? SDL_GPU_SHADERFORMAT_DXBC
                                           : (formats & SDL_GPU_SHADERFORMAT_SPIRV)
                                                 ? SDL_GPU_SHADERFORMAT_SPIRV
                                                 : SDL_GPU_SHADERFORMAT_INVALID;
    REXLOG_INFO("native view gpu: {}, {} (takes {})", SDL_GetGPUDeviceDriver(device),
                FormatNames(format), FormatNames(formats));
    if (format == SDL_GPU_SHADERFORMAT_INVALID) {
        REXLOG_WARN("native view gpu: the device takes no shader format band3 has");
        return false;
    }
    vertex_shader = MakeShader(format, SDL_GPU_SHADERSTAGE_VERTEX);
    pixel_shader = MakeShader(format, SDL_GPU_SHADERSTAGE_FRAGMENT);
    if (!vertex_shader || !pixel_shader) return false;

    // nearest and wrapping, as Shade() samples
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler = SDL_CreateGPUSampler(device, &si);

    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    ti.format = kColorFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = ti.height = 1;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    white = SDL_CreateGPUTexture(device, &ti);

    SDL_GPUBufferCreateInfo bi{};
    bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    bi.size = sizeof(Mat4);
    no_bones = SDL_CreateGPUBuffer(device, &bi);

    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = 256;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device, &tbi);
    if (!sampler || !white || !no_bones || !tb) {
        REXLOG_WARN("native view gpu: couldn't make its sampler and buffers ({})",
                    SDL_GetError());
        if (tb) SDL_ReleaseGPUTransferBuffer(device, tb);
        return false;
    }
    auto* p = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device, tb, false));
    const uint32_t white_px = 0xffffffffu;
    Mat4 identity{};
    for (int i = 0; i < 4; i++) identity.m[i][i] = 1.0f;
    if (p) {
        std::memcpy(p, &white_px, 4);
        std::memcpy(p + 16, &identity, sizeof(identity));
        SDL_UnmapGPUTransferBuffer(device, tb);
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    bool ok = p && cmd;
    if (cmd) {
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src{tb, 0, 1, 1};
        SDL_GPUTextureRegion dst{};
        dst.texture = white;
        dst.w = dst.h = dst.d = 1;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
        SDL_GPUTransferBufferLocation bsrc{tb, 16};
        SDL_GPUBufferRegion bdst{no_bones, 0, sizeof(Mat4)};
        SDL_UploadToGPUBuffer(copy, &bsrc, &bdst, false);
        SDL_EndGPUCopyPass(copy);
        ok = SDL_SubmitGPUCommandBuffer(cmd) && ok;
    }
    SDL_ReleaseGPUTransferBuffer(device, tb);
    if (!ok) {
        REXLOG_WARN("native view gpu: couldn't fill its buffers ({})", SDL_GetError());
        return false;
    }
    return true;
}

void GpuRenderer::Impl::Release(bool stop_video) {
    if (device) {
        SDL_WaitForGPUIdle(device);
        for (Buffer* b : {&arena_verts, &arena_indices, &pool_verts[0], &pool_verts[1],
                          &pool_indices[0], &pool_indices[1], &bones})
            ReleaseBuffer(*b);
        for (auto& [k, a] : tex_arrays)
            if (a.texture) SDL_ReleaseGPUTexture(device, a.texture);
        for (auto& [k, p] : pipelines) SDL_ReleaseGPUGraphicsPipeline(device, p);
        if (vertex_shader) SDL_ReleaseGPUShader(device, vertex_shader);
        if (pixel_shader) SDL_ReleaseGPUShader(device, pixel_shader);
        if (sampler) SDL_ReleaseGPUSampler(device, sampler);
        if (white) SDL_ReleaseGPUTexture(device, white);
        if (no_bones) SDL_ReleaseGPUBuffer(device, no_bones);
        if (color) SDL_ReleaseGPUTexture(device, color);
        if (depth) SDL_ReleaseGPUTexture(device, depth);
        if (readback) SDL_ReleaseGPUTransferBuffer(device, readback);
        if (upload) SDL_ReleaseGPUTransferBuffer(device, upload);
        SDL_DestroyGPUDevice(device);
    }
    meshes.clear();
    textures.clear();
    tex_arrays.clear();
    arena_vert_count = arena_index_count = 0;
    pipelines.clear();
    device = nullptr;
    vertex_shader = pixel_shader = nullptr;
    sampler = nullptr;
    white = color = depth = nullptr;
    no_bones = nullptr;
    readback = upload = nullptr;
    width = height = upload_size = 0;
    if (stop_video) StopVideo();
}

SDL_GPUGraphicsPipeline* GpuRenderer::Impl::Pipeline(int blend, const DepthRules& rules) {
    const int key = blend << 3 | rules.Key();
    if (auto it = pipelines.find(key); it != pipelines.end()) return it->second;

    // scene_capture.h's Vertex as it is, 56 bytes
    const SDL_GPUVertexBufferDescription buffer{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX,
                                                0};
    const SDL_GPUVertexAttribute attributes[] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, uint32_t(offsetof(Vertex, pos))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, uint32_t(offsetof(Vertex, nrm))},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, uint32_t(offsetof(Vertex, uv))},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, uint32_t(offsetof(Vertex, color))},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4, uint32_t(offsetof(Vertex, bone))},
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, uint32_t(offsetof(Vertex, weight))},
    };
    SDL_GPUColorTargetDescription target{};
    target.format = kColorFormat;
    // Blend() in soft_raster.cpp. Alpha is never written: it stays the
    // clear's 1, as the CPU's picture has it, so the readback is the picture.
    // The target clamps the colour to 0-1 before blending, which only Multiply
    // above 1 notices; SrcAlpha's scaling happens in the shader (kPremultiply)
    SDL_GPUColorTargetBlendState& bs = target.blend_state;
    bs.enable_color_write_mask = true;
    bs.color_write_mask = blend == kBlendDest ? SDL_GPUColorComponentFlags(0)
                                              : SDL_GPUColorComponentFlags(
                                                    SDL_GPU_COLORCOMPONENT_R |
                                                    SDL_GPU_COLORCOMPONENT_G |
                                                    SDL_GPU_COLORCOMPONENT_B);
    bs.color_blend_op = SDL_GPU_BLENDOP_ADD;
    bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    auto factors = [&](SDL_GPUBlendFactor src, SDL_GPUBlendFactor dst) {
        bs.enable_blend = true;
        bs.src_color_blendfactor = src;
        bs.dst_color_blendfactor = dst;
    };
    switch (blend) {
        case kBlendAdd:
        case kBlendSrcAlphaAdd:  // the shader has scaled it by alpha
            factors(SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ONE);
            break;
        case kBlendSrcAlpha:  // likewise
            factors(SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA);
            break;
        case kBlendSubtract:  // dst - src
            factors(SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ONE);
            bs.color_blend_op = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
            break;
        case kBlendMultiply:
            factors(SDL_GPU_BLENDFACTOR_DST_COLOR, SDL_GPU_BLENDFACTOR_ZERO);
            break;
        default:  // Src, and Dest, which writes no colour
            break;
    }

    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vertex_shader;
    pi.fragment_shader = pixel_shader;
    pi.vertex_input_state.vertex_buffer_descriptions = &buffer;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = attributes;
    pi.vertex_input_state.num_vertex_attributes = uint32_t(std::size(attributes));
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    // the CPU culls nothing; clipping at depth 1 is its near plane (mesh.hlsl)
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.rasterizer_state.enable_depth_clip = true;
    // depth is larger-is-nearer, cleared to 0. A draw that writes without
    // testing tests "always", since a pipeline that doesn't test can't write
    pi.depth_stencil_state.enable_depth_test = rules.test || rules.write;
    pi.depth_stencil_state.enable_depth_write = rules.write;
    pi.depth_stencil_state.compare_op = !rules.test          ? SDL_GPU_COMPAREOP_ALWAYS
                                        : rules.equal_passes ? SDL_GPU_COMPAREOP_GREATER_OR_EQUAL
                                                             : SDL_GPU_COMPAREOP_GREATER;
    pi.target_info.color_target_descriptions = &target;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = kDepthFormat;
    pi.target_info.has_depth_stencil_target = true;
    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(device, &pi);
    if (!p) REXLOG_WARN("native view gpu: no pipeline ({})", SDL_GetError());
    pipelines[key] = p;
    return p;
}

bool GpuRenderer::Impl::Reserve(Buffer& b, SDL_GPUBufferUsageFlags usage, uint32_t bytes) {
    if (b.buffer && b.size >= bytes) return true;
    ReleaseBuffer(b);
    SDL_GPUBufferCreateInfo bi{};
    bi.usage = usage;
    // room to grow, so a frame a little bigger than the last doesn't remake it
    bi.size = std::max(Align(bytes + bytes / 2, 1u << 16), 1u << 16);
    b.buffer = SDL_CreateGPUBuffer(device, &bi);
    if (!b.buffer) {
        REXLOG_WARN("native view gpu: no {} byte buffer ({})", bi.size, SDL_GetError());
        return false;
    }
    b.size = bi.size;
    return true;
}

void GpuRenderer::Impl::ReleaseBuffer(Buffer& b) {
    // SDL lets it go once the frames using it are done
    if (b.buffer) SDL_ReleaseGPUBuffer(device, b.buffer);
    b = Buffer{};
}

bool GpuRenderer::Impl::PlaceInArena() {
    uint32_t verts = 0, indices = 0;
    for (const Mesh* m : to_arena) {
        verts += uint32_t(m->keep->verts.size());
        indices += IndexSlots(*m->keep);
    }
    if (verts == 0) return true;
    if ((arena_vert_count + verts) * sizeof(Vertex) > arena_verts.size ||
        (arena_index_count + indices) * 2 > arena_indices.size) {
        // full: start over with the meshes this frame draws; the rest are let
        // go and come back through the pool if they're drawn again
        for (auto it = meshes.begin(); it != meshes.end();) {
            Mesh& m = it->second;
            if (!m.in_arena) {
                ++it;
                continue;
            }
            m.in_arena = false;
            if (m.used != serial) {
                it = meshes.erase(it);
                continue;
            }
            m.pool_vertex = ~0u;  // sent again from the CPU
            to_arena.push_back(&m);
            verts += uint32_t(m.keep->verts.size());
            indices += IndexSlots(*m.keep);
            ++it;
        }
        // everything in it is sent again this frame, so a bigger buffer can
        // start empty
        arena_vert_count = arena_index_count = 0;
        if (!Reserve(arena_verts, SDL_GPU_BUFFERUSAGE_VERTEX,
                     std::max<uint32_t>(verts * sizeof(Vertex), kMinArenaBytes)) ||
            !Reserve(arena_indices, SDL_GPU_BUFFERUSAGE_INDEX,
                     std::max<uint32_t>(indices * 2, kMinArenaBytes / 4))) {
            return false;
        }
    }
    for (Mesh* m : to_arena) {
        m->in_arena = true;
        m->first_vertex = arena_vert_count;
        m->first_index = arena_index_count;
        arena_vert_count += uint32_t(m->keep->verts.size());
        arena_index_count += IndexSlots(*m->keep);
    }
    return true;
}

bool GpuRenderer::Impl::PlaceTexture(Tex& tx) {
    const Texture& t = *tx.keep;
    uint32_t w, h;
    SizeClass(t.width, t.height, w, h);
    TexArray& a = tex_arrays[uint64_t(w) << 32 | h];
    if (a.free.empty()) {
        const uint32_t layers =
            a.layers ? a.layers * 2 : std::clamp<uint32_t>(kTextureArrayBytes / (w * h * 4), 1, 64);
        SDL_GPUTexture* grown = nullptr;
        if (layers <= kMaxTextureLayers) {
            SDL_GPUTextureCreateInfo ti{};
            ti.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
            ti.format = kColorFormat;
            ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            ti.width = w;
            ti.height = h;
            ti.layer_count_or_depth = layers;
            ti.num_levels = 1;
            grown = SDL_CreateGPUTexture(device, &ti);
        }
        if (!grown) {
            if (!texture_failure_logged) {
                texture_failure_logged = true;
                REXLOG_WARN("native view gpu: no {}x{} texture array of {} ({}); draws untextured",
                            w, h, layers, SDL_GetError());
            }
            return false;
        }
        if (a.texture) {
            // what the old one holds goes over in this frame's copy pass,
            // before anything is sent to the new one
            array_copies.push_back({a.texture, grown, w, h, a.layers});
        }
        for (uint32_t l = layers; l-- > a.layers;) a.free.push_back(l);
        a.texture = grown;
        a.layers = layers;
    }
    tx.array = &a;
    tx.layer = a.free.back();
    a.free.pop_back();
    return true;
}

void GpuRenderer::Impl::LetTextureGo(Tex& tx) {
    if (!tx.array) return;
    tx.array->free.push_back(tx.layer);
    if (tx.array->free.size() == tx.array->layers) tx.array->empty_since = serial;
    tx.array = nullptr;
}

bool GpuRenderer::Impl::EnsureTargets(uint32_t w, uint32_t h) {
    if (color && w == width && h == height) return true;
    if (color) SDL_ReleaseGPUTexture(device, color);
    if (depth) SDL_ReleaseGPUTexture(device, depth);
    if (readback) SDL_ReleaseGPUTransferBuffer(device, readback);
    color = depth = nullptr;
    readback = nullptr;
    width = height = 0;

    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ti.format = kColorFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    color = SDL_CreateGPUTexture(device, &ti);
    ti.format = kDepthFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    depth = SDL_CreateGPUTexture(device, &ti);
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size = w * h * 4;
    readback = SDL_CreateGPUTransferBuffer(device, &tbi);
    if (!color || !depth || !readback) {
        REXLOG_WARN("native view gpu: no {}x{} target ({})", w, h, SDL_GetError());
        return false;
    }
    width = w;
    height = h;
    return true;
}

bool GpuRenderer::Impl::Render(const FrameCapture& frame, const RasterOptions& o,
                               std::vector<uint32_t>& rgba, GpuStats& st) {
    if (!o.width || !o.height || !EnsureTargets(o.width, o.height)) return false;
    serial++;
    Buffer& pool_v = pool_verts[serial & 1];
    Buffer& pool_i = pool_indices[serial & 1];
    const Buffer& last_pool_v = pool_verts[(serial - 1) & 1];
    const Buffer& last_pool_i = pool_indices[(serial - 1) & 1];

    // what this frame draws, and what of it the GPU doesn't have yet
    to_pool.clear();
    to_arena.clear();
    new_textures.clear();
    array_copies.clear();
    frame_bones.clear();
    bone_base.assign(frame.draws.size(), 0);
    uint32_t pool_vert_count = 0, pool_index_count = 0;
    for (size_t d = 0; d < frame.draws.size(); d++) {
        const DrawItem& it = frame.draws[d];
        if (!Drawable(it)) continue;
        Mesh& m = meshes[it.geom.get()];
        if (!m.keep) {
            m.keep = it.geom;
            m.first = serial;
        }
        if (m.used != serial) {
            m.used = serial;
            if (m.in_arena) {
                // already there
            } else if (m.first != serial) {
                // the last frame drew it too (Evict lets go of pool geometry
                // a frame doesn't draw), from its pool: it stays
                m.pool_vertex = m.first_vertex;
                m.pool_index = m.first_index;
                to_arena.push_back(&m);
            } else {
                m.first_vertex = pool_vert_count;
                m.first_index = pool_index_count;
                pool_vert_count += uint32_t(it.geom->verts.size());
                pool_index_count += IndexSlots(*it.geom);
                to_pool.push_back(&m);
            }
        }
        if (Skinned(it, o)) {
            bone_base[d] = uint32_t(frame_bones.size());
            frame_bones.insert(frame_bones.end(), it.bones.begin(), it.bones.end());
        }
        const Texture* t = o.textures ? it.tex.get() : nullptr;
        if (t && t->width && t->height && t->rgba.size() == size_t(t->width) * t->height) {
            Tex& tx = textures[t];
            if (!tx.keep) {
                tx.keep = it.tex;
                tx.first = serial;
                if (PlaceTexture(tx)) new_textures.push_back(&tx);
            }
            tx.used = serial;
        }
    }
    if (!PlaceInArena()) return false;

    // the upload: the pool's vertices and indices, the arena's new meshes that
    // weren't in the last frame's pool, the bones, then the textures, each
    // where a copy may start
    const uint32_t pool_index_at = Align(pool_vert_count * uint32_t(sizeof(Vertex)), 16);
    uint32_t at = Align(pool_index_at + pool_index_count * 2, 16);
    const uint32_t arena_at = at;
    for (const Mesh* m : to_arena) {
        if (m->pool_vertex != ~0u) continue;
        at = Align(at + uint32_t(m->keep->verts.size() * sizeof(Vertex)), 16);
        at = Align(at + IndexSlots(*m->keep) * 2, 16);
    }
    const uint32_t bones_at = at;
    const uint32_t bone_bytes = uint32_t(frame_bones.size() * sizeof(Mat4));
    const uint32_t textures_at = Align(bones_at + bone_bytes, kTextureOffsetAlign);
    uint32_t upload_bytes = textures_at;
    for (const Tex* tx : new_textures) {
        const Texture& t = *tx->keep;
        upload_bytes += Align(Align(t.width * 4, kRowPitchAlign) * t.height, kTextureOffsetAlign);
    }
    if (pool_vert_count &&
        (!Reserve(pool_v, SDL_GPU_BUFFERUSAGE_VERTEX, pool_vert_count * sizeof(Vertex)) ||
         !Reserve(pool_i, SDL_GPU_BUFFERUSAGE_INDEX, pool_index_count * 2))) {
        return false;
    }
    if (bone_bytes &&
        !Reserve(bones, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, bone_bytes)) {
        return false;
    }
    if (upload_bytes > upload_size) {
        if (upload) SDL_ReleaseGPUTransferBuffer(device, upload);
        SDL_GPUTransferBufferCreateInfo tbi{};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size = std::max(Align(upload_bytes + upload_bytes / 4, 1u << 16), upload_size);
        upload = SDL_CreateGPUTransferBuffer(device, &tbi);
        upload_size = upload ? tbi.size : 0;
        if (!upload) {
            REXLOG_WARN("native view gpu: no {} byte upload buffer ({})", tbi.size,
                        SDL_GetError());
            return false;
        }
    }

    if (upload_bytes) {
        auto* base = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device, upload, true));
        if (!base) {
            REXLOG_WARN("native view gpu: couldn't map the upload buffer ({})", SDL_GetError());
            return false;
        }
        auto put_indices = [](uint8_t* to, const Geometry& g) {
            std::memcpy(to, g.indices.data(), g.indices.size() * 2);
            if (g.indices.size() & 1) std::memset(to + g.indices.size() * 2, 0, 2);
        };
        for (const Mesh* m : to_pool) {
            const Geometry& g = *m->keep;
            std::memcpy(base + size_t(m->first_vertex) * sizeof(Vertex), g.verts.data(),
                        g.verts.size() * sizeof(Vertex));
            put_indices(base + pool_index_at + size_t(m->first_index) * 2, g);
        }
        uint32_t mesh_at = arena_at;
        for (const Mesh* m : to_arena) {
            if (m->pool_vertex != ~0u) continue;
            const Geometry& g = *m->keep;
            std::memcpy(base + mesh_at, g.verts.data(), g.verts.size() * sizeof(Vertex));
            mesh_at = Align(mesh_at + uint32_t(g.verts.size() * sizeof(Vertex)), 16);
            put_indices(base + mesh_at, g);
            mesh_at = Align(mesh_at + IndexSlots(g) * 2, 16);
        }
        if (bone_bytes) std::memcpy(base + bones_at, frame_bones.data(), bone_bytes);
        uint32_t tex_at = textures_at;
        for (const Tex* tx : new_textures) {
            const Texture& t = *tx->keep;
            const uint32_t pitch = Align(t.width * 4, kRowPitchAlign);
            for (uint32_t y = 0; y < t.height; y++)
                std::memcpy(base + tex_at + size_t(y) * pitch,
                            t.rgba.data() + size_t(y) * t.width, t.width * 4);
            tex_at += Align(pitch * t.height, kTextureOffsetAlign);
        }
        SDL_UnmapGPUTransferBuffer(device, upload);
    }
    st.uploads = uint32_t(to_pool.size() + to_arena.size() + new_textures.size());

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    if (!cmd) {
        REXLOG_WARN("native view gpu: no command buffer ({})", SDL_GetError());
        return false;
    }
    if (upload_bytes || !to_arena.empty() || !array_copies.empty()) {
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        // texture arrays that grew: the old one's layers into the new one
        // first, then it can go (SDL keeps it until the copy is done)
        for (const ArrayCopy& c : array_copies) {
            for (uint32_t l = 0; l < c.layers; l++) {
                SDL_GPUTextureLocation src{c.from, 0, l, 0, 0, 0};
                SDL_GPUTextureLocation dst{c.to, 0, l, 0, 0, 0};
                SDL_CopyGPUTextureToTexture(copy, &src, &dst, c.w, c.h, 1, false);
            }
            SDL_ReleaseGPUTexture(device, c.from);
        }
        // nothing here cycles: Render waits out each frame, so none is still
        // drawing from these (the pools alternate for the copies to the
        // arena, which read the last frame's)
        auto send = [&](uint32_t from, const Buffer& to, uint32_t offset, uint32_t size) {
            if (!size) return;
            SDL_GPUTransferBufferLocation src{upload, from};
            SDL_GPUBufferRegion dst{to.buffer, offset, size};
            SDL_UploadToGPUBuffer(copy, &src, &dst, false);
        };
        send(0, pool_v, 0, pool_vert_count * uint32_t(sizeof(Vertex)));
        send(pool_index_at, pool_i, 0, pool_index_count * 2);
        uint32_t mesh_at = arena_at;
        for (const Mesh* m : to_arena) {
            const Geometry& g = *m->keep;
            const uint32_t vsize = uint32_t(g.verts.size() * sizeof(Vertex));
            const uint32_t isize = IndexSlots(g) * 2;
            const uint32_t vto = m->first_vertex * uint32_t(sizeof(Vertex));
            const uint32_t ito = m->first_index * 2;
            if (m->pool_vertex != ~0u) {
                const uint32_t vfrom = m->pool_vertex * uint32_t(sizeof(Vertex));
                SDL_GPUBufferLocation src{last_pool_v.buffer, vfrom};
                SDL_GPUBufferLocation dst{arena_verts.buffer, vto};
                SDL_CopyGPUBufferToBuffer(copy, &src, &dst, vsize, false);
                src = {last_pool_i.buffer, m->pool_index * 2};
                dst = {arena_indices.buffer, ito};
                SDL_CopyGPUBufferToBuffer(copy, &src, &dst, isize, false);
                continue;
            }
            send(mesh_at, arena_verts, vto, vsize);
            mesh_at = Align(mesh_at + vsize, 16);
            send(mesh_at, arena_indices, ito, isize);
            mesh_at = Align(mesh_at + isize, 16);
        }
        send(bones_at, bones, 0, bone_bytes);
        uint32_t tex_at = textures_at;
        for (const Tex* tx : new_textures) {
            const Texture& t = *tx->keep;
            const uint32_t pitch = Align(t.width * 4, kRowPitchAlign);
            SDL_GPUTextureTransferInfo src{upload, tex_at, pitch / 4, t.height};
            SDL_GPUTextureRegion dst{};
            dst.texture = tx->array->texture;
            dst.layer = tx->layer;
            dst.w = t.width;
            dst.h = t.height;
            dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, false);
            tex_at += Align(pitch * t.height, kTextureOffsetAlign);
        }
        SDL_EndGPUCopyPass(copy);
    }

    // a pass per stretch of draws between depth clears, as Rasterize() clears
    // depth when a camera it hasn't seen yet starts drawing
    SDL_GPUColorTargetInfo ct{};
    ct.texture = color;
    ct.clear_color = {kClearGrey, kClearGrey, kClearGrey, 1.0f};
    ct.load_op = SDL_GPU_LOADOP_CLEAR;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo dt{};
    dt.texture = depth;
    dt.clear_depth = 0.0f;
    dt.load_op = SDL_GPU_LOADOP_CLEAR;
    dt.store_op = SDL_GPU_STOREOP_STORE;
    dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPUBuffer* bone_buffer = bone_bytes ? bones.buffer : no_bones;
    SDL_GPURenderPass* pass = nullptr;
    bool pass_drew = false;
    // what's bound in the pass, so a draw binds only what changes
    SDL_GPUGraphicsPipeline* bound = nullptr;
    SDL_GPUBuffer* bound_verts = nullptr;
    SDL_GPUTexture* bound_tex = nullptr;
    auto begin_pass = [&] {
        pass = SDL_BeginGPURenderPass(cmd, &ct, 1, &dt);
        SDL_BindGPUVertexStorageBuffers(pass, 0, &bone_buffer, 1);
        pass_drew = false;
        bound = nullptr;
        bound_verts = nullptr;
        bound_tex = nullptr;
    };
    begin_pass();
    cams_seen.clear();
    uint32_t last_cam = 0;
    for (size_t d = 0; d < frame.draws.size(); d++) {
        const DrawItem& it = frame.draws[d];
        if (o.clear_depth_per_camera && it.cam != last_cam &&
            std::find(cams_seen.begin(), cams_seen.end(), it.cam) == cams_seen.end()) {
            cams_seen.push_back(it.cam);
            if (pass_drew) {
                SDL_EndGPURenderPass(pass);
                ct.load_op = SDL_GPU_LOADOP_LOAD;
                begin_pass();
            }
        }
        last_cam = it.cam;
        if (!Drawable(it)) {
            st.skipped++;
            continue;
        }
        const Mesh& m = meshes[it.geom.get()];
        SDL_GPUGraphicsPipeline* pipeline = Pipeline(BlendFor(it, o), RulesFor(it, o));
        if (!pipeline) {
            st.skipped++;
            continue;
        }
        if (pipeline != bound) {
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            bound = pipeline;
        }
        SDL_GPUBuffer* verts = m.in_arena ? arena_verts.buffer : pool_v.buffer;
        if (verts != bound_verts) {
            const SDL_GPUBufferBinding vb{verts, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vb, 1);
            const SDL_GPUBufferBinding ib{m.in_arena ? arena_indices.buffer : pool_i.buffer, 0};
            SDL_BindGPUIndexBuffer(pass, &ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);
            bound_verts = verts;
        }

        const Tex* tex = nullptr;
        if (o.textures && it.tex) {
            auto t = textures.find(it.tex.get());
            if (t != textures.end() && t->second.array) tex = &t->second;
        }
        SDL_GPUTexture* sampled = tex ? tex->array->texture : white;
        if (sampled != bound_tex) {
            const SDL_GPUTextureSamplerBinding ts{sampled, sampler};
            SDL_BindGPUFragmentSamplers(pass, 0, &ts, 1);
            bound_tex = sampled;
        }

        VertexUniforms vu{};
        vu.world = it.world;
        vu.view_proj = it.view_proj;
        if (Skinned(it, o)) {
            vu.skinned = 1;
            vu.bone_base = bone_base[d];
            vu.bone_count = uint32_t(it.bones.size());
        }
        SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));
        const int blend = BlendFor(it, o);
        PixelUniforms pu{};
        std::copy(std::begin(it.color), std::end(it.color), pu.color);
        pu.flags = (tex ? kTextured : 0) | (it.prelit ? kPrelit : 0) |
                   (o.lighting ? kLighting : 0) | (it.alpha_cut ? kAlphaCut : 0) |
                   (blend == kBlendSrcAlpha || blend == kBlendSrcAlphaAdd ? kPremultiply : 0);
        pu.alpha_threshold = float(it.alpha_threshold);
        if (tex) {
            pu.tex_layer = tex->layer;
            pu.tex_size[0] = tex->keep->width;
            pu.tex_size[1] = tex->keep->height;
        }
        SDL_PushGPUFragmentUniformData(cmd, 0, &pu, sizeof(pu));

        SDL_DrawGPUIndexedPrimitives(pass, uint32_t(it.geom->indices.size() / 3 * 3), 1,
                                     m.first_index, int32_t(m.first_vertex), 0);
        pass_drew = true;
        st.draws++;
    }
    SDL_EndGPURenderPass(pass);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{};
    src.texture = color;
    src.w = width;
    src.h = height;
    src.d = 1;
    SDL_GPUTextureTransferInfo dst{readback, 0, width, height};
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);
    const auto submitted = std::chrono::steady_clock::now();
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) {
        REXLOG_WARN("native view gpu: the frame didn't submit ({})", SDL_GetError());
        return false;
    }
    const bool done = SDL_WaitForGPUFences(device, true, &fence, 1);
    SDL_ReleaseGPUFence(device, fence);
    if (!done) {
        REXLOG_WARN("native view gpu: the frame didn't finish ({})", SDL_GetError());
        return false;
    }
    const auto* px = static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(device, readback, false));
    if (!px) {
        REXLOG_WARN("native view gpu: couldn't read the frame back ({})", SDL_GetError());
        return false;
    }
    // alpha is the clear's 0xff throughout: no pipeline writes it
    rgba.resize(size_t(width) * height);
    std::memcpy(rgba.data(), px, rgba.size() * sizeof(uint32_t));
    SDL_UnmapGPUTransferBuffer(device, readback);
    st.wait_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                           submitted).count();
    Evict();
    return true;
}

void GpuRenderer::Impl::Evict() {
    // geometry only this frame drew stays until the next, which moves it to
    // the arena if it draws it too; the arena's space comes back when it's
    // rebuilt
    for (auto it = meshes.begin(); it != meshes.end();) {
        const Mesh& m = it->second;
        if (m.used == serial || (m.in_arena && m.used + kEvictAfter >= serial)) {
            ++it;
            continue;
        }
        it = meshes.erase(it);
    }
    // likewise a texture drawn in one frame only, such as one RB3 rendered
    // that frame, goes once a frame hasn't drawn it
    for (auto it = textures.begin(); it != textures.end();) {
        const Tex& t = it->second;
        if (t.used == serial || (t.used != t.first && t.used + kEvictAfter >= serial)) {
            ++it;
            continue;
        }
        LetTextureGo(it->second);
        it = textures.erase(it);
    }
    // an array whose textures have all gone goes after a while
    for (auto it = tex_arrays.begin(); it != tex_arrays.end();) {
        const TexArray& a = it->second;
        if (a.free.size() < a.layers || a.empty_since + kEvictAfter >= serial) {
            ++it;
            continue;
        }
        if (a.texture) SDL_ReleaseGPUTexture(device, a.texture);
        it = tex_arrays.erase(it);
    }
}

GpuRenderer& GpuRenderer::Get() {
    // never destroyed: Shutdown lets the device go while SDL is still there
    static GpuRenderer* r = new GpuRenderer;
    return *r;
}

GpuRenderer::GpuRenderer() : impl_(std::make_unique<Impl>()) {}
GpuRenderer::~GpuRenderer() = default;

bool GpuRenderer::Init() {
    if (impl_->tried) return impl_->ready;
    std::lock_guard lock(impl_->mutex);
    if (impl_->tried.exchange(true)) return impl_->ready;
    if (impl_->Create()) {
        impl_->ready = true;
        return true;
    }
    impl_->Release(true);
    REXLOG_WARN("native view gpu: not available, the native view draws on the CPU");
    return false;
}

bool GpuRenderer::Ready() { return impl_->ready; }

bool GpuRenderer::RenderFrame(const FrameCapture& frame, const RasterOptions& options,
                              std::vector<uint32_t>& rgba, GpuStats& stats) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->device) return false;
    const auto start = std::chrono::steady_clock::now();
    stats = GpuStats{};
    if (!impl_->Render(frame, options, rgba, stats)) {
        // whatever went wrong would go wrong every frame; the CPU takes over
        REXLOG_WARN("native view gpu: giving up for this session, the native view draws on "
                    "the CPU");
        impl_->ready = false;
        impl_->Release(false);
        return false;
    }
    stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                   .count();
    return true;
}

void GpuRenderer::Shutdown() {
    std::lock_guard lock(impl_->mutex);
    impl_->ready = false;
    impl_->Release(true);
}

}  // namespace band3::render
