#include "src/Render/gpu_view.h"

#include "src/Render/deferred_decode.h"
#include "src/Render/frame_compose.h"
#include "src/Render/gamma_ramp.h"
#include "src/Render/guest_formats.h"
#include "src/Render/post_model.h"
#include "src/Render/sample_model.h"
#include "src/Render/shade_model.h"
#include "src/Render/spot_model.h"
#include "src/Render/target_premake.h"
#include "src/Render/shaders/gamma_shaders.gen.h"
#include "src/Render/shaders/mesh_shaders.gen.h"
#include "src/Render/shaders/mips_shaders.gen.h"
#include "src/Render/shaders/post_shaders.gen.h"
#include "src/Render/shaders/velocity_shaders.gen.h"
#include "src/stall_watch.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_version.h>
#ifdef _WIN32
// for SDL_RegisterApp only; band3 has its own main
#define SDL_MAIN_HANDLED
#define SDL_MAIN_NOIMPL
#include <SDL3/SDL_main.h>
// the Direct3D 12 texture behind an output, for the presenter (CheckZeroCopy)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#endif
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// See gpu_view.h.

namespace band3::render {
namespace {

constexpr SDL_GPUTextureFormat kColorFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
constexpr SDL_GPUTextureFormat kDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
// a shadow map's target: its depth, clip z/w, as soft_raster.cpp keeps it
constexpr SDL_GPUTextureFormat kShadowFormat = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
// frames unused (and kKeepSeconds) before a mesh or texture is let go; a
// render target's picture is forgotten at the frames alone
constexpr uint64_t kEvictAfter = 120;
// seconds unused before a forgotten target's textures go, and what ClockKeep
// keeps in a song; past kMaxRts, targets go LRU first (gpu_view.h's residency)
constexpr double kKeepSeconds = 30;
constexpr size_t kMaxRts = 128;
// over this an arena rebuild keeps less than the clock would
// (ArenaRebuildKeep); a song's arena is 24 to 34 MB
constexpr uint64_t kMaxArenaBytes = 256u << 20;
// A size class's texture array starts at about this many bytes and doubles
// when full. Textures share arrays: making a texture costs ~0.5 ms.
constexpr uint32_t kTextureArrayBytes = 4u << 20;
constexpr uint32_t kMaxTextureLayers = 2048;  // Direct3D 12's limit
// Direct3D 12 copies texture rows from an upload at this pitch, starting at an
// offset aligned to kTextureOffsetAlign; textures are laid out so
constexpr uint32_t kRowPitchAlign = 256;
constexpr uint32_t kTextureOffsetAlign = 512;
// the arena's least size; it's made twice as big as it needs
constexpr uint32_t kMinArenaBytes = 4u << 20;
// holds a song's usual frame: growing it later costs a frame several ms
constexpr uint32_t kInitialUploadBytes = 32u << 20;

// mesh.hlsl's cbuffers, as they lie in memory
struct VertexUniforms {
    Mat4 world;
    Mat4 view_proj;
    uint32_t skinned;
    uint32_t bone_base;
    uint32_t bone_count;
    uint32_t shadow_depth;  // into a shadow map: the clip z is the depth
    float clip_offset[4];  // ClipOffset's
    // the depth's DepthMap (soft_raster.h): p, q, r, then 0; 0 1 0 is 1/w
    float depth_map[4];
    // RasterOptions::overlay_edge for a whole-picture overlay draw; 1 1 otherwise
    float overlay_edge[4] = {1.0f, 1.0f, 0.0f, 0.0f};
    shade::ShadeParams shade;
};
static_assert(sizeof(VertexUniforms) == 192 + sizeof(shade::ShadeParams));

void SetDepthMap(const DepthMap& d, float out[4]) {
    out[0] = d.p;
    out[1] = d.q;
    out[2] = d.r;
    out[3] = 0;
}

// What mesh.hlsl adds to a draw's clip x, y (times w) in a viewport vw x vh:
// half a pixel right and down, so SDL_gpu's centre x + .5 sees what D3D9's
// pixel centre x did; none for DrawRect's quads, which RB3 draws on D3D10's
// (soft_raster.cpp's PixelCentre)
void ClipOffset(const DrawItem& it, float vw, float vh, float out[4]) {
    const bool rect = it.rect_shader >= 0;
    out[0] = rect ? 0.0f : 1.0f / vw;
    out[1] = rect ? 0.0f : -1.0f / vh;
    out[2] = out[3] = 0.0f;
}

struct PixelUniforms {
    shade::ShadeParams shade;
    // diffuse, specular map, glow map, projected light, gobo, normal map,
    // detail map
    uint32_t tex_layer[8];
    // width and height: the five before the normal map, then the shadow
    // map's, the normal map's and the detail map's
    uint32_t tex_size[8][4];
    // x: kPremultiply; y: bit per map (tex_layer's order) for DXN kept as BC5;
    // z, w: R8 maps' expand codes, a byte each, 0 for the rest (mesh.hlsl's
    // MapTexel)
    uint32_t flags[4];
    // sample_model.h's PackSampler, in tex_layer's order
    uint32_t tex_sampler[8][4];
};
static_assert(sizeof(PixelUniforms) == sizeof(shade::ShadeParams) + 304);

// mesh.hlsl's pixel_flags.x
enum : uint32_t { kPremultiply = 1 };

// mesh.hlsl's SpotUniforms, a spotlight's cone's second buffer
struct SpotUniforms {
    spot::SpotParams spot;
    float viewport[4];  // the pass's: x, y, 1/width, 1/height
    uint32_t sizes[4];  // the scene depth's width and height
};
static_assert(sizeof(SpotUniforms) == sizeof(spot::SpotParams) + 32);

// the textures a draw samples, in mesh.hlsl's sampler order: the maps, the
// picture behind (kShadeRefract), the shadow map (kShadeShadow), the normal
// and detail maps (kShadeNormalMap, kShadeDetailMap); a spotlight's cone reads
// two more, the scene's depth and the density map
enum {
    kSlotDiffuse,
    kSlotSpecular,
    kSlotGlow,
    kSlotProjected,
    kSlotGobo,
    kSlotBehind,
    kSlotShadow,
    kSlotNormal,
    kSlotDetail,
    kNumSlots
};
enum { kSlotSceneDepth = kNumSlots, kSlotDensity, kNumSpotSlots };
static_assert(kSlotBehind == 5 && kSlotNormal == 7,
              "PixelUniforms has tex_size for the five maps, the shadow's, then the normal's");

// RndMat::Blend, the modes Blend() in soft_raster.cpp draws (Screen, Lighten
// and Darken, which NgMat sets no state for, as Src, as it does)
enum : int {
    kBlendDest = 0,
    kBlendSrc = 1,
    kBlendAdd = 2,
    kBlendSrcAlpha = 3,
    kBlendSrcAlphaAdd = 4,
    kBlendSubtract = 5,
    kBlendMultiply = 6,
    kBlendPreMultAlpha = 7,
};
static_assert(kBlendPreMultAlpha < 8, "Pipeline()'s key keeps the mode in 3 bits");

// What a draw's pipeline does with its target's alpha: leaves it, blends it
// by the colour's factors (into a texture), or as RB3's back buffer does
// (WritesSceneAlpha: ONE ONE MAX where it blends)
enum class AlphaMode { kNone, kTexture, kScene };
constexpr int kNumAlphaModes = 3;

// mesh.hlsl's PSMain, PSSpotCone, PSSoftParticle, PSShadowDepth (R32_FLOAT
// target, LESS against depth cleared to the pass's clear_z, no blend)
enum class PixelKind { kMesh, kSpot, kSoft, kShadowDepth };

// a target's sample count (1, 2 or 4) as SDL has it, and in Pipeline()'s key
SDL_GPUSampleCount SampleCount(uint32_t samples) {
    return samples == 4   ? SDL_GPU_SAMPLECOUNT_4
           : samples == 2 ? SDL_GPU_SAMPLECOUNT_2
                          : SDL_GPU_SAMPLECOUNT_1;
}
int SampleBits(uint32_t samples) { return samples == 4 ? 2 : samples == 2 ? 1 : 0; }

// the triangles a draw's pipeline culls, by their winding on the screen
// (DrawItem::cull): what soft_raster.cpp's RasterTri drops
enum class CullWinding { kNone, kClockwise, kCounterClockwise, kAll };

CullWinding CullFor(const DrawItem& it, const RasterOptions& o) {
    if (!o.culling || !it.cull) return CullWinding::kNone;
    const bool cw = Culls(it.cull, true), ccw = Culls(it.cull, false);
    return cw && ccw ? CullWinding::kAll
           : cw      ? CullWinding::kClockwise
           : ccw     ? CullWinding::kCounterClockwise
                     : CullWinding::kNone;
}

int BlendFor(const DrawItem& it, const RasterOptions& o) {
    // Blend() draws any other value as Src
    if (!o.blending || it.blend < kBlendDest || it.blend > kBlendPreMultAlpha) return kBlendSrc;
    return it.blend;
}

bool Drawable(const DrawItem& it) {
    return it.geom && !it.geom->verts.empty() && it.geom->indices.size() >= 3;
}

bool Skinned(const DrawItem& it, const RasterOptions& o) {
    return o.skinning && !it.bones.empty();
}

// whether a draw of `run` is drawn (soft_raster.h's PassRun)
bool DrawnIn(const PassRun& run, const FrameCapture& frame, const DrawItem& it,
             const RasterOptions& o) {
    return (run.pass ? DrawnInTexturePass(it) : DrawnToBackBuffer(it)) && Drawable(it) &&
           DrawnByOptions(frame, it, o);
}

// soft_raster.cpp's depth rules for a draw, which pick its pipeline
struct DepthRules {
    bool test, equal_passes, write;
    int Key() const { return int(test) | int(equal_passes) << 1 | int(write) << 2; }
};

// `no_z`: into a texture without a depth buffer, where nothing tests or
// writes depth
DepthRules RulesFor(const DrawItem& it, const RasterOptions& o, bool no_z) {
    if (no_z) return {false, false, false};
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

// level 0 and the following mips that have their proper size; of
// Texture::blocks with `blocks`
uint32_t LevelsOf(const Texture& t, bool blocks = false) {
    uint32_t n = 1;
    if (blocks) {
        for (const auto& level : t.blocks->mips) {
            if (level.size() != guest_format::LevelBlockBytes(t.blocks->format,
                                                              std::max(t.width >> n, 1u),
                                                              std::max(t.height >> n, 1u)))
                break;
            n++;
        }
        return n;
    }
    for (const auto& level : t.mips) {
        const size_t texels =
            size_t(std::max(t.width >> n, 1u)) * std::max(t.height >> n, 1u);
        if (level.size() != texels) break;
        n++;
    }
    return n;
}

// The GPU format for a Xenos block format (BlockPixels::format) kept as blocks
// (RasterOptions::bc_textures), else INVALID. Loads match DecodeBlock but for
// BC5's (x, y, 0, 1) vs (x, y, y, y), which mesh.hlsl's MapTexel fixes.
SDL_GPUTextureFormat BcFormat(uint32_t xenos_format) {
    switch (xenos_format) {
        case 18: return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
        case 19: return SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
        case 20: return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
        case 49: return SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM;
        default: return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
}
bool IsBc(SDL_GPUTextureFormat f) {
    return f == SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM || f == SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM ||
           f == SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM || f == SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM;
}
// The GPU's format for a texture kept as blocks or bytes (Texture::blocks):
// BcFormat's, or for k_8's bytes (RasterOptions::r8_textures) R8, which loads
// as (byte, 0, 0, 1) where DecodeLevel8 swizzles the byte by the fetch's
// swizzle: mesh.hlsl's MapTexel makes the same texel from it
// (PixelUniforms::flags[2] and [3])
SDL_GPUTextureFormat KeptFormat(uint32_t xenos_format) {
    return xenos_format == 2 ? SDL_GPU_TEXTUREFORMAT_R8_UNORM : BcFormat(xenos_format);
}
// the bytes of one of a format's 4x4 blocks (BC), or of a texel (R8, RGBA8)
uint32_t BlockBytes(SDL_GPUTextureFormat f) {
    return f == SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM ? 8
           : IsBc(f)                                ? 16
           : f == SDL_GPU_TEXTUREFORMAT_R8_UNORM    ? 1
                                                    : 4;
}
// the texels along a side of one of its blocks
uint32_t BlockSide(SDL_GPUTextureFormat f) { return IsBc(f) ? 4 : 1; }
// the bits a texel of a texture array takes: what its megabytes count
uint32_t TexelBits(SDL_GPUTextureFormat f) {
    return BlockBytes(f) * 8 / (BlockSide(f) * BlockSide(f));
}

// index counts are kept even, so every copy of indices is whole 4-byte words
uint32_t IndexSlots(const Geometry& g) { return Align(uint32_t(g.indices.size()), 2); }

uint32_t NextPow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

// A texture's size class (its array's layer size): powers of two, at least
// kMinClassSize, at most 2:1, so few arrays are needed. An array has its
// class's full chain; a texture's levels sit in each level's corner.
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
    // mesh.hlsl's PSSpotCone, PSSoftParticle and PSShadowDepth
    SDL_GPUShader* spot_shader = nullptr;
    SDL_GPUShader* soft_shader = nullptr;
    SDL_GPUShader* shadow_shader = nullptr;
    // whether the device draws into and samples R32_FLOAT, the shadow maps'
    // format; without, the characters are drawn without their self-shadows
    bool shadow_maps = false;
    // whether the device samples BC1/2/3/5 texture arrays (checked at
    // Create), so RasterOptions::bc_textures can keep blocks; bc_now is this
    // frame's choice, by which UseTexture places new textures
    bool bc_formats = false;
    bool bc_now = false;
    // likewise R8 texture arrays, for k_8's bytes (RasterOptions::r8_textures)
    bool r8_format = false;
    bool r8_now = false;
    // Direct3D 12 copies a BC texture's regions in whole blocks, even a level
    // under 4 texels on a side; Vulkan's and Metal's end at the level's edge
    // (BcExtent)
    bool bc_whole_blocks = false;
    // post.hlsl's: the full-screen triangle, the resolve and post-processing
    SDL_GPUShader* fullscreen_shader = nullptr;
    SDL_GPUShader* resolve_shader = nullptr;
    // copies the picture and its depth into every sample of the overlay's
    // multisampled target (PSOverlayStart)
    SDL_GPUShader* overlay_start_shader = nullptr;
    SDL_GPUShader* downsample_shader = nullptr;
    SDL_GPUShader* blur_shader = nullptr;
    SDL_GPUShader* glare_shader = nullptr;
    SDL_GPUShader* composite_shader = nullptr;
    // also keeps the post buffer the trails read (PSCompositeHistory: two
    // targets)
    SDL_GPUShader* composite_history_shader = nullptr;
    // camera motion blur's velocity pass (PSVelocity) and object pass
    // (velocity.hlsl), with a pipeline per CullWinding but kAll
    SDL_GPUShader* velocity_shader = nullptr;
    SDL_GPUShader* velocity_object_vs = nullptr;
    SDL_GPUShader* velocity_object_ps = nullptr;
    SDL_GPUGraphicsPipeline* velocity_object_pipelines[3] = {};
    SDL_GPUGraphicsPipeline* resolve_pipeline = nullptr;
    SDL_GPUGraphicsPipeline* downsample_pipeline = nullptr;
    SDL_GPUGraphicsPipeline* blur_pipeline = nullptr;
    SDL_GPUGraphicsPipeline* glare_pipeline = nullptr;
    SDL_GPUGraphicsPipeline* composite_pipeline = nullptr;
    SDL_GPUGraphicsPipeline* composite_history_pipeline = nullptr;
    SDL_GPUGraphicsPipeline* velocity_pipeline = nullptr;
    // gamma.hlsl's: the display gamma ramp over the finished picture
    SDL_GPUShader* gamma_shader = nullptr;
    SDL_GPUGraphicsPipeline* gamma_pipeline = nullptr;
    // mips.hlsl's: texture pass mips drawn inline (RasterOptions::inline_mips)
    // as SDL's GenerateMipmaps does on Direct3D 12 (inline_mips_ok only).
    // mip_sampler has no LOD clamp; linear_sampler's pins level 0.
    SDL_GPUShader* mip_vertex_shader = nullptr;
    SDL_GPUShader* mip_shader = nullptr;
    SDL_GPUGraphicsPipeline* mip_pipeline = nullptr;
    SDL_GPUSampler* mip_sampler = nullptr;
    bool inline_mips_ok = false;
    // by blend mode, DepthRules::Key, AlphaMode, CullWinding, PixelKind and
    // sample count, all made before the first frame; and the overlay start's,
    // by sample count (OverlayStartPipeline)
    std::unordered_map<int, SDL_GPUGraphicsPipeline*> pipelines;
    // Prewarm has started (read without the mutex) / finished; a pipeline made
    // after it is one it missed, which a frame waited for (logged)
    std::atomic<bool> warm{false};
    bool warmed_up = false;
    // RasterOptions::gpu_labels: the last frame submitted and its indexed
    // draws in order, a list per command buffer (DescribeIndexedDraw)
    std::mutex draw_log_mutex;
    std::string draw_log_frame;
    std::vector<std::vector<std::string>> draw_log;
    SDL_GPUSampler* sampler = nullptr;
    // linear and clamping, as RB3 samples its post-processing levels
    SDL_GPUSampler* linear_sampler = nullptr;
    SDL_GPUTexture* white = nullptr;     // bound for untextured draws, which don't read it
    SDL_GPUTexture* black = nullptr;     // transparent: a render target nothing has drawn
    SDL_GPUBuffer* no_bones = nullptr;   // one identity bone, bound when nothing is skinned

    // The world draws to `scene` (alpha: bloom weight), resolved into
    // `color`; the overlay draws on top. They share `depth`, sampled by the
    // resolve if the device can (depth_sampled), else no_depth (0), no DOF.
    SDL_GPUTexture* scene = nullptr;
    SDL_GPUTexture* color = nullptr;
    SDL_GPUTexture* depth = nullptr;
    // The overlay's multisampled targets (soft_raster.h's OverlaySamples):
    // colour, resolved into `color` as each pass ends (the samples' mean, as
    // RB3's EndTiling), and its own depth, leaving the world's intact. SDL
    // can't sample them. Made at the picture's size on a frame's first overlay.
    SDL_GPUTexture* color_ms = nullptr;
    SDL_GPUTexture* depth_ms = nullptr;
    uint32_t ms_samples = 1, ms_w = 0, ms_h = 0;
    // whether the device draws kColorFormat and kDepthFormat at [0] 2 and [1]
    // 4 samples; the unsupported counts and failures already logged
    bool ms_supported[2] = {};
    uint32_t ms_fallback_logged = 0;
    bool ms_failure_logged = false;
    // `color` as the resolve left it, for the overlay's REFRACT_WORLD draws
    // (RefractsWorld), made on frames that have one
    SDL_GPUTexture* behind = nullptr;
    // RenderFrame's output, read back: the picture through the frame's gamma
    // ramp (or the identity)
    SDL_GPUTexture* graded = nullptr;
    bool depth_sampled = false;
    SDL_GPUTexture* no_depth = nullptr;
    SDL_GPUTransferBuffer* readback = nullptr;
    uint32_t width = 0, height = 0;
    // post-processing's levels (post_model.h), RGBA8 as the 360's: DOF at a
    // quarter of the picture's size, bloom at a quarter, a sixteenth and a
    // sixty-fourth, a blur's first direction at each; velocity at half size
    SDL_GPUTexture* post_dof = nullptr;
    SDL_GPUTexture* post_velocity = nullptr;
    uint32_t velocity_w = 0, velocity_h = 0;
    SDL_GPUTexture* post_velocity_depth = nullptr;
    // where each velocity object's two palettes (this frame's, then the
    // last's) start in the frame's bones
    std::vector<uint32_t> velocity_bone_base;
    SDL_GPUTexture* post_bloom[3] = {};
    SDL_GPUTexture* post_tmp[3] = {};
    uint32_t post_w[3] = {}, post_h[3] = {};
    // a texture pass's target copied to a plain 2D RGBA8 texture for a blur
    // to read: the depth volume before a blur (the game blurs it in place,
    // through a resolve), NgLight's shadow likewise, the soft-particle
    // surface; and the scene a world pass left (soft_raster.h's
    // kPreBufferPasses) for the next one's REFRACT_WORLD draws
    struct Scratch {
        SDL_GPUTexture* texture = nullptr;
        uint32_t w = 0, h = 0;
    };
    Scratch spot_scratch, light_scratch, soft_scratch, pre_scratch;
    // The trails' post buffer (RasterOptions::trails): the last post frame's
    // composite in tex[cur] (-1 none); the next writes the other. Like the
    // buffers below, apart from the frame's targets so captures at another
    // size leave it be.
    struct History {
        SDL_GPUTexture* tex[2] = {};
        uint32_t w = 0, h = 0;
        int cur = -1;
        uint64_t game_frame = 0;
    };
    History history;
    // RasterOptions::post_buffer: the last post frame's picture before its
    // overlay (game_frame 0: none), shown under later frames' overlays
    struct PostBuffer {
        SDL_GPUTexture* tex = nullptr;
        uint32_t w = 0, h = 0;
        uint64_t game_frame = 0;
    };
    PostBuffer post_buffer;
    // RasterOptions::pre_buffer: the last refracting world frame's scene
    // before post-processing, for the next one's REFRACT_WORLD draws
    PostBuffer pre_buffer;

    // The presenter's outputs (RenderFrameToOutput). SDL leaves them in
    // ALL_SHADER_RESOURCE, so the SDK samples them barrier-free. Never cycled,
    // so the texture stays the one checked. `fence`: the last frame drawn
    // into it, until OutputDone sees it signal.
    struct Output {
        SDL_GPUTexture* texture = nullptr;
        uint32_t w = 0, h = 0;
        uint64_t generation = 0;
        void* resource = nullptr;  // its ID3D12Resource, if it can be sampled in place
        SDL_GPUFence* fence = nullptr;
    };
    Output outputs[kOutputs];
    uint64_t output_generations = 0;
    // DownloadOutput's, grown to the biggest output read back
    SDL_GPUTransferBuffer* output_readback = nullptr;
    uint32_t output_readback_size = 0;
    // CheckZeroCopy: the SDK's ID3D12Device, and whether outputs can be
    // sampled in place (read without `mutex`; zero_copy_why under its own)
    void* present_device = nullptr;
    std::atomic<bool> zero_copy_checked{false};
    std::atomic<bool> zero_copy{false};
    std::mutex zero_copy_mutex;
    std::string zero_copy_why = "not checked yet";
    // RefuseDevice's reason; keeps Init from making the device (under
    // zero_copy_mutex)
    std::string refused;

    // GPU timings (RasterOptions::gpu_timestamps; gpu_timing_model.h). Heap
    // regions of kTimingSlots: each output's, RenderFrame's (kOutputs), and
    // the aside's (kAsideRegion) for fenceless pre passes and world-ahead work
    // until the next frame resolves it. The readback has two regions per frame
    // (its own, the aside's), never overwritten before read: outputs wait for
    // their fence (native_view.cpp's PresentSlots), RenderFrame reads at once.
    static constexpr uint32_t kTimingSlots = 256;
    static constexpr int kFrameRegions = kOutputs + 1;
    static constexpr int kAsideRegion = kOutputs + 1;
    uint64_t timestamp_frequency = 0;
    bool timing_checked = false, timing_ok = false;
#ifdef _WIN32
    ID3D12QueryHeap* query_heap = nullptr;
    ID3D12Resource* query_readback = nullptr;
#endif
    // the recording frame's ladder and the aside's; its region (-1 untimed),
    // and whether it marks the aside
    gpu_timing::Ladder ladder{kTimingSlots}, aside_ladder{kTimingSlots};
    int timing_region = -1;
    bool timing_aside = false;
    // a frame region's ladders as resolved, until its frame's times are read
    struct Timed {
        std::vector<uint8_t> labels, aside;
        uint32_t dropped = 0;
        bool pending = false;
    };
    Timed timed[kFrameRegions];

    // Everything a frame sends goes through this one transfer buffer and one
    // copy pass, mapped cycling so a frame never waits on an earlier one.
    SDL_GPUTransferBuffer* upload = nullptr;
    uint32_t upload_size = 0;

    struct Buffer {
        SDL_GPUBuffer* buffer = nullptr;
        uint32_t size = 0;
    };
    // Geometry drawn in more than one frame lives in the arena (appended;
    // rebuilt on the GPU when full: PlaceInArena); new geometry in the frame's
    // pool, no buffer per mesh. The pool alternates two buffers, so geometry
    // drawn again next frame moves to the arena by a GPU copy.
    Buffer arena_verts, arena_indices;
    uint32_t arena_vert_count = 0, arena_index_count = 0;
    // the arena a rebuild this frame replaced; the copy pass copies out of it
    // and then releases it (the handle mustn't be used after release)
    Buffer old_arena_verts, old_arena_indices;
    Buffer pool_verts[2], pool_indices[2];  // by frame serial & 1
    Buffer bones;  // this frame's skinned draws' bones, one after another

    struct Mesh {
        std::shared_ptr<const Geometry> keep;  // so the key stays this geometry's
        uint64_t first = 0;                    // the frame that first drew it
        // the frame that last drew it, and when (frame_now)
        uint64_t used = 0;
        std::chrono::steady_clock::time_point used_at;
        // for ClockKeep: the first drawing frame's world (frame_world),
        // whether another world's frames drew it, whether last drawn in a song
        uint64_t first_world = 0;
        bool across_worlds = false;
        bool drawn_in_song = false;
        bool in_arena = false;
        // where it starts in the arena, or else in its frame's pool
        uint32_t first_vertex = 0;
        uint32_t first_index = 0;
        // when moving to the arena: where it was in the last frame's pool or
        // the old arena (kept by a rebuild); with neither (~0u) it's sent
        // from the CPU
        uint32_t pool_vertex = ~0u;
        uint32_t pool_index = 0;
        uint32_t arena_vertex = ~0u;
        uint32_t arena_index = 0;
        bool FromCpu() const { return pool_vertex == ~0u && arena_vertex == ~0u; }
    };
    std::unordered_map<const Geometry*, Mesh> meshes;

    // the textures of a size class and format, a layer each: RGBA8, or a BC
    // format for those kept as blocks (BcFormat)
    struct TexArray {
        SDL_GPUTexture* texture = nullptr;
        uint32_t w = 0, h = 0;  // its class's size
        SDL_GPUTextureFormat format = kColorFormat;
        uint32_t layers = 0;
        uint32_t levels = 1;  // its class's whole chain
        std::vector<uint32_t> free;
        uint64_t empty_since = 0;  // when its last texture went, if none are left
        // made ahead (Premake) and not yet used: kept empty until then
        // (kArrayKeepSeconds), then as any empty one
        std::chrono::steady_clock::time_point ahead_until{};
    };
    std::unordered_map<uint64_t, TexArray> tex_arrays;
    static uint64_t ArrayKey(uint32_t w, uint32_t h, SDL_GPUTextureFormat format) {
        return uint64_t(w) << 40 | uint64_t(h) << 16 | uint32_t(format);
    }
    // a size class's first array's layers: about kTextureArrayBytes of level 0
    static uint32_t FirstArrayLayers(uint32_t w, uint32_t h, SDL_GPUTextureFormat format) {
        const uint64_t layer_bytes = uint64_t(w) * h * TexelBits(format) / 8;
        return uint32_t(std::clamp<uint64_t>(kTextureArrayBytes / layer_bytes, 1, 64));
    }
    SDL_GPUTexture* NewArrayTexture(uint32_t w, uint32_t h, SDL_GPUTextureFormat format,
                                    uint32_t layers);
    struct Tex {
        std::shared_ptr<const Texture> keep;
        TexArray* array = nullptr;  // null if it couldn't have a layer
        uint32_t layer = 0;
        uint32_t levels = 1;  // of the texture's, in it (LevelsOf)
        uint64_t first = 0;
        // as a Mesh's
        uint64_t used = 0;
        std::chrono::steady_clock::time_point used_at;
        uint64_t first_world = 0;
        bool across_worlds = false;
        bool drawn_in_song = false;
    };
    std::unordered_map<const Texture*, Tex> textures;
    uint64_t serial = 0;
    bool texture_failure_logged = false;

    // A texture pass's target, by DxTex: a one-layer RGBA8 2D array with mips
    // (bound like any texture) plus depth; a shadow map's is plain R32_FLOAT
    // 2D (mesh.hlsl's shadow_tex). Kept between frames (Evict); remade when
    // its size changes.
    struct Rt {
        SDL_GPUTexture* color = nullptr;
        SDL_GPUTexture* depth = nullptr;
        uint32_t w = 0, h = 0, levels = 1;
        // the pass's size in the game; w x h unless drawn bigger
        // (soft_raster.h's PassTargetSize)
        uint32_t game_w = 0, game_h = 0;
        bool shadow = false;    // a shadow map's
        // its depth is its size's shared one (target_premake.h's SharesDepth)
        bool depth_shared = false;
        // made ahead (Idle) and not yet found by a pass's TargetFor
        bool premade = false;
        bool drawn = false;     // by a pass, this frame or before
        uint64_t drawn_in = 0;  // the frame a pass last drew it
        uint32_t version = 0;   // the version that pass made
        // the frame that last drew or sampled it, and when (frame_now)
        uint64_t used = 0;
        std::chrono::steady_clock::time_point used_at;
    };
    std::unordered_map<uint32_t, Rt> rts;
    bool rt_failure_logged = false;
    // the depth textures targets without depth share, by size (w << 32 | h),
    // and how many targets hold each; let go with the last
    struct SharedDepth {
        SDL_GPUTexture* texture = nullptr;
        uint32_t refs = 0;
    };
    std::unordered_map<uint64_t, SharedDepth> shared_depths;
    // targets announced and not yet made ahead (target_premake.h), and what
    // Idle made since the last frame (GpuStats::targets_premade)
    AnnounceQueue premake_pending;
    TextureAnnounceQueue premake_textures;
    uint32_t premade_since = 0, arrays_premade_since = 0;
    double premade_ms_since = 0;
    // DxTexes with a target made since the device started, for
    // GpuStats::targets_returning
    std::unordered_set<uint32_t> rts_seen;
    // the frame's time, taken as its walk starts: the used_at it stamps, and
    // what Evict measures idle seconds from
    std::chrono::steady_clock::time_point frame_now;
    // its world's game frame (FrameCapture::world_frame), and whether the game
    // is in a song (RasterOptions::clock_keep; ClockKeep)
    uint64_t frame_world = 0;
    bool frame_in_song = false;
    // seconds a Mesh or Tex is kept after this frame
    template <typename T>
    double KeepSecondsOf(const T& m) const {
        return ClockKeep(frame_in_song, m.drawn_in_song, m.across_worlds, kKeepSeconds);
    }
    // seconds since `used_at`, at frame_now
    double IdleSeconds(std::chrono::steady_clock::time_point used_at) const {
        return std::chrono::duration<double>(frame_now - used_at).count();
    }
    // Evict's meshes and textures kept by the clock alone
    // (GpuStats::meshes_by_time, textures_by_time)
    uint32_t meshes_by_time = 0, textures_by_time = 0;
    // Evict's forgotten targets (used, DxTex) for kMaxRts; kept to avoid
    // allocating
    std::vector<std::pair<uint64_t, uint32_t>> rts_forgotten;

    // a frame's work, kept between frames to avoid allocating
    std::vector<Mesh*> to_pool, to_arena;
    std::vector<Tex*> new_textures;
    // An array that grew: only the old layers holding textures sent in an
    // earlier frame are copied, with their written levels (Tex::levels); a
    // layer placed this frame is sent to the new array directly.
    struct ArrayCopy {
        SDL_GPUTexture* from;
        SDL_GPUTexture* to;
        uint32_t w, h;
        bool bc;  // a BC array's, copied in whole blocks (BcExtent)
        std::vector<std::pair<uint32_t, uint32_t>> layers;  // layer, levels
    };
    std::vector<ArrayCopy> array_copies;  // arrays that grew, old into new
    std::vector<Mat4> frame_bones;
    std::vector<uint32_t> bone_base;  // per draw, where its bones start
    std::vector<shade::ShadeParams> shades;  // per draw
    std::vector<uint32_t> cams_seen;
    // per draw, what its diffuse texture and its projected light's s5 sample
    // (kSourceNone: the capture's map, or none)
    enum Source : uint8_t { kSourceNone, kSourceTexture, kSourceRt, kSourceBlack };
    std::vector<uint8_t> diffuse_source;
    std::vector<uint8_t> proj_source;
    // the normal and detail maps' (a head's normal map is a target's:
    // MapTargetOf)
    std::vector<uint8_t> normal_source[2];
    // per draw: a spotlight cone (PSSpotCone), one skipped (no scene depth),
    // a depth volume blur into itself, a soft particle (PSSoftParticle), or a
    // blur of one soft-particle surface into the other
    enum SpotDraw : uint8_t {
        kSpotNone,
        kSpotCone,
        kSpotConeSkipped,
        kSpotBlur,
        kSoftParticle,
        kSoftBlur
    };
    std::vector<uint8_t> spot_draw;
    // per texture pass run: drawn (has a target), and what starts cleared
    enum : uint8_t { kRunDrawn = 1, kRunClearColor = 2, kRunClearDepth = 4 };
    std::vector<uint8_t> run_clear;
    // made and let go since the device started; Draw turns these into a
    // frame's (GpuStats::pipelines_made and the rest)
    struct Counts {
        uint64_t pipelines = 0, buffers = 0, textures = 0, arena_rebuilds = 0;
        uint64_t evicted_meshes = 0, evicted_textures = 0, evicted_rts = 0, rts_released = 0;
        uint64_t textures_pressured = 0, meshes_pressured = 0;
    };
    Counts counts;
    // the stats of the frame whose walk is placing (Render), where TargetFor
    // and PlaceTexture count what they make; null outside it
    GpuStats* walk_stats = nullptr;

    bool StartVideo(const char* driver);
    void StopVideo();
    bool Create();
    // stop_video: on the UI thread only, as SDL wants
    void Release(bool stop_video);
    // SDL's Direct3D 12 backend copies a pipeline's fragment samplers into
    // the command buffer's 2048-entry sampler heap as a batch on each rebind,
    // checking room only before the batch (SDL 3.4 and main as of 2026-10:
    // "FIXME: need to error on overflow"). A batch straddling the heap's end
    // writes outside it (INVALID_DESCRIPTOR_HANDLE) and AMD GPUs hang on the
    // next draw (DEVICE_HUNG). So every sampling fragment shader declares
    // kSamplerBatch samplers (MakeShader) and every pass binds them all
    // (BeginPass): batches then end exactly on the heap's end.
    static constexpr uint32_t kSamplerBatch = 16;
    static_assert(2048 % kSamplerBatch == 0, "a batch divides SDL's sampler heap");
    // a render pass, its kSamplerBatch fragment samplers bound to white first
    SDL_GPURenderPass* BeginPass(SDL_GPUCommandBuffer* cmd, const SDL_GPUColorTargetInfo* ct,
                                 uint32_t targets, const SDL_GPUDepthStencilTargetInfo* dt);
    // `texture`'s (w x h, one-layer RGBA8 array) levels 1 to levels - 1, as
    // SDL's GenerateMipmaps blits them on Direct3D 12 (inline_mips_ok)
    void DrawMips(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* texture, uint32_t w, uint32_t h,
                  uint32_t levels);
    // one of the generated shaders, in `format` (DXBC or SPIR-V)
    SDL_GPUShader* MakeShader(SDL_GPUShaderFormat format, SDL_GPUShaderStage stage,
                              const unsigned char* dxbc, size_t dxbc_size,
                              const unsigned char* spirv, size_t spirv_size, const char* entry,
                              uint32_t samplers, uint32_t storage_buffers, uint32_t uniforms);
    // `pixel`: PSMain's, or PSSpotCone's or PSSoftParticle's in its place;
    // `samples` the target's (the overlay's multisampled one, or 1)
    SDL_GPUGraphicsPipeline* Pipeline(int blend, const DepthRules& rules, AlphaMode alpha,
                                      CullWinding cull, PixelKind pixel = PixelKind::kMesh,
                                      uint32_t samples = 1);
    // PSOverlayStart's, into the overlay's targets at `samples`: depth
    // written, not tested
    SDL_GPUGraphicsPipeline* OverlayStartPipeline(uint32_t samples);
    // the overlay's samples for `want` (OverlaySamples): `want` if the device
    // can, else the other of 2 and 4, else 1 (logged once)
    uint32_t DeviceSamples(uint32_t want);
    // the overlay's targets at the picture's size; false if they couldn't be
    // made (logged)
    bool EnsureOverlayTargets(uint32_t samples);
    SDL_GPUGraphicsPipeline* ShadowDepthPipeline(CullWinding cull) {
        return Pipeline(kBlendSrc, {true, false, true}, AlphaMode::kNone, cull,
                        PixelKind::kShadowDepth);
    }
    // post.hlsl's triangle (or `vertex`) and `pixel`, into RGBA8
    SDL_GPUGraphicsPipeline* MakeFullscreenPipeline(SDL_GPUShader* pixel, const char* name,
                                                    uint32_t targets = 1,
                                                    SDL_GPUShader* vertex = nullptr);
    // velocity.hlsl's object pass: SrcAlpha (alpha 1 or 0), depth LEQUAL
    // tested and written
    SDL_GPUGraphicsPipeline* MakeVelocityObjectPipeline(CullWinding cull);
    bool EnsureHistory(uint32_t w, uint32_t h);
    void ReleaseHistory();
    // emptied (game_frame 0) if made again
    bool EnsureKept(PostBuffer& b, uint32_t w, uint32_t h);
    void ReleaseKept(PostBuffer& b);
    void ReleaseTargets();
    // makes every pipeline a frame can ask for (the overlay's at
    // DeviceSamples(overlay_samples)) and the usual upload buffer, so no
    // frame stalls on them
    void Prewarm(uint32_t overlay_samples);
    // SDL_gpu's Direct3D 12 pools, grown only when a frame finds them empty,
    // and never shrunk: command buffers (a command list and allocator, ~7 ms
    // each on the R9700 in its VM), their fences (made at submit, ~6.5 ms),
    // 32 KB uniform buffers (~0.45 ms; a draw pushes 2 KB) and descriptor
    // heap pairs (taken at a command buffer's first draw, ~1.7 ms). The
    // first frame needing more than any before made them as it recorded: a
    // song's first world 11 ms, its first 1402-draw frame 28 ms and the post
    // frames after it 7 to 20 ms each. Fills them to kWarmCommandBuffers,
    // kWarmUniformBuffers and kWarmHeapPairs once, with work that draws
    // nothing a frame sees. Once per device (GpuRenderer::Idle); logged.
    void WarmPools();
    static constexpr uint32_t kWarmCommandBuffers = 16;
    static constexpr uint32_t kWarmUniformBuffers = 128;
    static constexpr uint32_t kWarmHeapPairs = 24;
    std::atomic<bool> pools_warm{false};
    bool EnsureTargets(uint32_t w, uint32_t h);
    bool EnsureScratch(Scratch& s, uint32_t w, uint32_t h);
    // grows `b` to hold `bytes`, losing its contents
    bool Reserve(Buffer& b, SDL_GPUBufferUsageFlags usage, uint32_t bytes);
    void ReleaseBuffer(Buffer& b);
    // places this frame's new arena meshes, rebuilding the arena if they
    // don't fit (adding what it keeps to to_arena); false if it couldn't grow
    bool PlaceInArena();
    // marks `t` drawn this frame; a new one gets a layer and an upload
    void UseTexture(const std::shared_ptr<const Texture>& t);
    // null if `t` has no layer
    const Tex* TextureFor(const Texture* t);
    // a layer in its size class's `format` array, growing the array if full
    // and none of its textures can go
    bool PlaceTexture(Tex& tx, SDL_GPUTextureFormat format);
    // the texels a copy of a `texels`-long level into a BC array's
    // `level_size`-long level spans: whole blocks, which on Vulkan stop at the
    // level's edge
    uint32_t BcExtent(uint32_t texels, uint32_t level_size) const {
        const uint32_t whole = guest_format::AlignUp(texels, 4);
        return bc_whole_blocks ? whole : std::min(whole, level_size);
    }
    void LetTextureGo(Tex& tx);
    // mips counted as a third more (GpuStats::texture_array_mb)
    double TextureArrayMb() const;
    // the target for texture pass `p`, remade if not w x h (PassTargetSize)
    // or holding a shared depth `p` mustn't have; a new one without depth
    // takes its size's shared depth if `share` (RasterOptions::premake_targets).
    // Null on failure.
    Rt* TargetFor(const Pass& p, uint32_t w, uint32_t h, bool share);
    // makes `rt`'s textures (it has none); false on failure (logged once)
    bool MakeRt(Rt& rt, uint32_t w, uint32_t h, uint32_t levels, bool shadow, bool share);
    SDL_GPUTexture* TakeSharedDepth(uint32_t w, uint32_t h);
    void DropSharedDepth(uint32_t w, uint32_t h);
    // between frames (GpuRenderer::Idle): announced targets and texture
    // arrays made ahead, within PremakeBudget; whether any are left
    bool Premake(const RasterOptions& o);
    // marks `rt` drawn or sampled by this frame, for Evict
    void UseRt(Rt& rt) {
        rt.used = serial;
        rt.used_at = frame_now;
    }
    void ReleaseRt(Rt& rt);
    // remade (a new generation) if not w x h; false on failure
    bool EnsureOutput(int slot, uint32_t w, uint32_t h);
    void ReleaseOutputs();
    // signalled or not: SDL's own reference keeps it until the submission ends
    void ReleaseFence(Output& out);
    // the ID3D12Resource behind `texture` if the SDK's presenter can sample
    // it in place; else null, with why
    void* SdkResource(SDL_GPUTexture* texture, const SDL_GPUTextureCreateInfo& info,
                      std::string& why);
    // CheckZeroCopy's first check, with a texture of its own
    void CheckZeroCopyOnce();
    void SetZeroCopy(bool ok, const std::string& why);
    // SDL's ID3D12GraphicsCommandList behind `cmd` if SDL 3.4.14's command
    // buffer layout checks out (no calls made through it), else null
    void* TimingList(SDL_GPUCommandBuffer* cmd);
    // whether GPU timings can be taken in `cmd`: the first time, full checks
    // (the list's and allocator's interfaces, type, device) and making the
    // query heap and readback; after, those results and `cmd`'s pointers
    bool CheckTimingOnce(SDL_GPUCommandBuffer* cmd);
    // A Render's timing, its command buffer just acquired: a pre pass or the
    // world ahead goes on the aside's ladder as one part; a frame on its own
    // region's (`slot`'s or RenderFrame's), from kUpload.
    void StartTiming(SDL_GPUCommandBuffer* cmd, const RasterOptions& o, int slot, int pre_pass,
                     bool ahead_pass);
    // GPU time from here goes to `part` (gpu_timing::Ladder::Mark); the
    // aside's ladder marks only its start and end (kNone)
    void MarkTime(SDL_GPUCommandBuffer* cmd, uint8_t part);
    // in a frame's last command buffer before submit: ends its ladder and
    // the aside's, resolving both into the frame's readback region
    void ResolveTimes(SDL_GPUCommandBuffer* cmd);
    // once the GPU has finished `region`'s frame: its unread times into `st`
    // (GpuStats::gpu_ms)
    void ReadTimes(int region, GpuStats& st);
    void ReleaseTiming();
    // Draws `frame` into output `slot`, or with -1 into `graded`, read back
    // into rgba. `pre_pass` (from 1): that world pass before a refracting
    // frame (soft_raster.h's kPreBufferPasses), world alone, scene left in
    // pre_scratch, its REFRACT_WORLD draws reading the last pass's there (the
    // first black). `ahead_pass` (RenderWorldAhead): the world alone, left in
    // the scene target and submitted unwaited.
    bool Render(const FrameCapture& frame, const RasterOptions& o, int slot,
                std::vector<uint32_t>* rgba, GpuStats& stats, int pre_pass = 0,
                bool ahead_pass = false);
    // The world RenderWorldAhead left in the scene target and depth, drawn as
    // frame `serial` (0 none). Every Render forgets it as it starts, so only
    // the very next one can post-process it: another might draw over it.
    struct Ahead {
        uint64_t serial = 0, world_frame = 0;
        uint32_t w = 0, h = 0;
    };
    Ahead ahead;
    // false if the GPU failed (logged)
    bool Download(SDL_GPUTexture* texture, uint32_t w, uint32_t h, std::vector<uint32_t>& rgba);
    // after each frame: lets go of what's gone unused long enough
    // (gpu_view.h's residency)
    void Evict();
    // frames that one frame's geometry and textures are kept
    // (ResidencyKeepFrames, from RasterOptions::world_period)
    uint64_t keep_frames = 0;
};

// SDL_CreateGPUDevice wants a video subsystem in band3's SDL copy, which is
// otherwise used for audio and HID only
bool GpuRenderer::Impl::StartVideo(const char* driver) {
    if (driver) {
        // over an SDL_VIDEO_DRIVER in the environment, which could open windows
        SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, driver, SDL_HINT_OVERRIDE);
    } else {
        SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
#ifdef _WIN32
        // rexruntime's SDL copy registers SDL_app; a name of our own avoids a
        // collision
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
                                             SDL_GPUShaderStage stage,
                                             const unsigned char* dxbc, size_t dxbc_size,
                                             const unsigned char* spirv, size_t spirv_size,
                                             const char* entry, uint32_t samplers,
                                             uint32_t storage_buffers, uint32_t uniforms) {
    SDL_GPUShaderCreateInfo info{};
    const bool is_dxbc = format == SDL_GPU_SHADERFORMAT_DXBC;
    info.code = is_dxbc ? dxbc : spirv;
    info.code_size = is_dxbc ? dxbc_size : spirv_size;
    info.entrypoint = entry;
    info.format = format;
    info.stage = stage;
    // a fragment shader's samplers, padded (kSamplerBatch)
    if (stage == SDL_GPU_SHADERSTAGE_FRAGMENT && samplers) {
        if (samplers > kSamplerBatch)
            REXLOG_WARN("native view gpu: the shader {} samples {} textures, more than {}", entry,
                        samplers, kSamplerBatch);
        samplers = std::max(samplers, kSamplerBatch);
    }
    info.num_samplers = samplers;
    info.num_storage_buffers = storage_buffers;
    info.num_uniform_buffers = uniforms;
    SDL_GPUShader* s = SDL_CreateGPUShader(device, &info);
    if (!s) REXLOG_WARN("native view gpu: the shader {} didn't load ({})", entry, SDL_GetError());
    return s;
}

SDL_GPURenderPass* GpuRenderer::Impl::BeginPass(SDL_GPUCommandBuffer* cmd,
                                                const SDL_GPUColorTargetInfo* ct, uint32_t targets,
                                                const SDL_GPUDepthStencilTargetInfo* dt) {
    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, ct, targets, dt);
    if (rp) {
        SDL_GPUTextureSamplerBinding padding[kSamplerBatch];
        for (SDL_GPUTextureSamplerBinding& b : padding) b = {white, sampler};
        SDL_BindGPUFragmentSamplers(rp, 0, padding, kSamplerBatch);
    }
    return rp;
}

void GpuRenderer::Impl::DrawMips(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* texture, uint32_t w,
                                 uint32_t h, uint32_t levels) {
    // SDL_GenerateMipmapsForGPUTexture's blits, call for call: a pass per
    // level, load DONT_CARE, the blit's viewport (the level's size, at least
    // 1), no scissor. Byte-identical to SDL's, odd sizes too. A level whose
    // side halves to 0 (1x1 under 256x512) is drawn by neither and left as is.
    for (uint32_t l = 1; l < levels; l++) {
        SDL_GPUColorTargetInfo ct{};
        ct.texture = texture;
        ct.mip_level = l;
        ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* rp = BeginPass(cmd, &ct, 1, nullptr);
        const SDL_GPUViewport v{0.0f, 0.0f, float(std::max(w >> l, 1u)),
                                float(std::max(h >> l, 1u)), 0.0f, 1.0f};
        SDL_SetGPUViewport(rp, &v);
        SDL_BindGPUGraphicsPipeline(rp, mip_pipeline);
        const SDL_GPUTextureSamplerBinding tb{texture, mip_sampler};
        SDL_BindGPUFragmentSamplers(rp, 0, &tb, 1);
        const uint32_t level[4] = {l - 1, 0, 0, 0};
        SDL_PushGPUFragmentUniformData(cmd, 0, level, sizeof(level));
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        SDL_EndGPURenderPass(rp);
    }
}

SDL_GPUGraphicsPipeline* GpuRenderer::Impl::MakeFullscreenPipeline(SDL_GPUShader* pixel,
                                                                    const char* name,
                                                                    uint32_t targets,
                                                                    SDL_GPUShader* vertex) {
    SDL_GPUColorTargetDescription target[2]{};
    target[0].format = target[1].format = kColorFormat;
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vertex ? vertex : fullscreen_shader;
    pi.fragment_shader = pixel;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.target_info.color_target_descriptions = target;
    pi.target_info.num_color_targets = targets;
    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(device, &pi);
    if (!p) REXLOG_WARN("native view gpu: no {} pipeline ({})", name, SDL_GetError());
    return p;
}

SDL_GPUGraphicsPipeline* GpuRenderer::Impl::MakeVelocityObjectPipeline(CullWinding cull) {
    const SDL_GPUVertexBufferDescription buffer{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX,
                                                0};
    const SDL_GPUVertexAttribute attributes[] = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, uint32_t(offsetof(Vertex, pos))},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, uint32_t(offsetof(Vertex, nrm))},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, uint32_t(offsetof(Vertex, uv))},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, uint32_t(offsetof(Vertex, color))},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4, uint32_t(offsetof(Vertex, bone))},
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, uint32_t(offsetof(Vertex, weight))},
        {6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, uint32_t(offsetof(Vertex, tan))},
    };
    SDL_GPUColorTargetDescription target{};
    target.format = kColorFormat;
    SDL_GPUColorTargetBlendState& bs = target.blend_state;
    bs.enable_blend = true;
    bs.src_color_blendfactor = bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    bs.dst_color_blendfactor = bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    bs.color_blend_op = bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = velocity_object_vs;
    pi.fragment_shader = velocity_object_ps;
    pi.vertex_input_state.vertex_buffer_descriptions = &buffer;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = attributes;
    pi.vertex_input_state.num_vertex_attributes = uint32_t(std::size(attributes));
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pi.rasterizer_state.cull_mode = cull == CullWinding::kClockwise ? SDL_GPU_CULLMODE_BACK
                                    : cull == CullWinding::kCounterClockwise
                                        ? SDL_GPU_CULLMODE_FRONT
                                        : SDL_GPU_CULLMODE_NONE;
    pi.rasterizer_state.enable_depth_clip = true;
    pi.depth_stencil_state.enable_depth_test = true;
    pi.depth_stencil_state.enable_depth_write = true;
    pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    pi.target_info.color_target_descriptions = &target;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = kDepthFormat;
    pi.target_info.has_depth_stencil_target = true;
    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(device, &pi);
    if (!p) REXLOG_WARN("native view gpu: no velocity object pipeline ({})", SDL_GetError());
    return p;
}

bool GpuRenderer::Impl::Create() {
    // offscreen first (no windows), then the platform's own driver
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
    using namespace shaders;
    vertex_shader = MakeShader(format, SDL_GPU_SHADERSTAGE_VERTEX, kMeshVertexDxbc,
                               sizeof(kMeshVertexDxbc), kMeshVertexSpirv,
                               sizeof(kMeshVertexSpirv), "VSMain", 0, 1, 1);
    pixel_shader = MakeShader(format, SDL_GPU_SHADERSTAGE_FRAGMENT, kMeshPixelDxbc,
                              sizeof(kMeshPixelDxbc), kMeshPixelSpirv, sizeof(kMeshPixelSpirv),
                              "PSMain", kNumSlots, 0, 1);
    spot_shader = MakeShader(format, SDL_GPU_SHADERSTAGE_FRAGMENT, kSpotPixelDxbc,
                             sizeof(kSpotPixelDxbc), kSpotPixelSpirv, sizeof(kSpotPixelSpirv),
                             "PSSpotCone", kNumSpotSlots, 0, 2);
    // the mesh's slots and the scene's depth
    soft_shader = MakeShader(format, SDL_GPU_SHADERSTAGE_FRAGMENT, kSoftPixelDxbc,
                             sizeof(kSoftPixelDxbc), kSoftPixelSpirv, sizeof(kSoftPixelSpirv),
                             "PSSoftParticle", kSlotSceneDepth + 1, 0, 2);
    // reads nothing: the depth is its position's
    shadow_shader = MakeShader(format, SDL_GPU_SHADERSTAGE_FRAGMENT, kShadowDepthPixelDxbc,
                               sizeof(kShadowDepthPixelDxbc), kShadowDepthPixelSpirv,
                               sizeof(kShadowDepthPixelSpirv), "PSShadowDepth", 0, 0, 0);
    fullscreen_shader = MakeShader(format, SDL_GPU_SHADERSTAGE_VERTEX, kFullscreenVertexDxbc,
                                   sizeof(kFullscreenVertexDxbc), kFullscreenVertexSpirv,
                                   sizeof(kFullscreenVertexSpirv), "VSFullscreen", 0, 0, 0);
    resolve_shader = MakeShader(format, SDL_GPU_SHADERSTAGE_FRAGMENT, kResolvePixelDxbc,
                                sizeof(kResolvePixelDxbc), kResolvePixelSpirv,
                                sizeof(kResolvePixelSpirv), "PSResolve", 2, 0, 1);
    overlay_start_shader = MakeShader(
        format, SDL_GPU_SHADERSTAGE_FRAGMENT, kOverlayStartPixelDxbc,
        sizeof(kOverlayStartPixelDxbc), kOverlayStartPixelSpirv,
        sizeof(kOverlayStartPixelSpirv), "PSOverlayStart", 2, 0, 1);
    downsample_shader = MakeShader(format, SDL_GPU_SHADERSTAGE_FRAGMENT, kDownsamplePixelDxbc,
                                   sizeof(kDownsamplePixelDxbc), kDownsamplePixelSpirv,
                                   sizeof(kDownsamplePixelSpirv), "PSDownsample", 1, 0, 1);
    blur_shader = MakeShader(format, SDL_GPU_SHADERSTAGE_FRAGMENT, kBlurPixelDxbc,
                             sizeof(kBlurPixelDxbc), kBlurPixelSpirv, sizeof(kBlurPixelSpirv),
                             "PSBlur", 1, 0, 1);
    glare_shader = MakeShader(format, SDL_GPU_SHADERSTAGE_FRAGMENT, kGlarePixelDxbc,
                              sizeof(kGlarePixelDxbc), kGlarePixelSpirv, sizeof(kGlarePixelSpirv),
                              "PSGlare", 1, 0, 1);
    composite_shader = MakeShader(format, SDL_GPU_SHADERSTAGE_FRAGMENT, kCompositePixelDxbc,
                                  sizeof(kCompositePixelDxbc), kCompositePixelSpirv,
                                  sizeof(kCompositePixelSpirv), "PSComposite", 11, 0, 1);
    composite_history_shader = MakeShader(
        format, SDL_GPU_SHADERSTAGE_FRAGMENT, kCompositeHistoryPixelDxbc,
        sizeof(kCompositeHistoryPixelDxbc), kCompositeHistoryPixelSpirv,
        sizeof(kCompositeHistoryPixelSpirv), "PSCompositeHistory", 12, 0, 1);
    // the scene's colour (not read) and depth, as the resolve's
    velocity_shader = MakeShader(format, SDL_GPU_SHADERSTAGE_FRAGMENT, kVelocityPixelDxbc,
                                 sizeof(kVelocityPixelDxbc), kVelocityPixelSpirv,
                                 sizeof(kVelocityPixelSpirv), "PSVelocity", 2, 0, 1);
    // the object pass: the bones, and the scene's depth
    velocity_object_vs = MakeShader(
        format, SDL_GPU_SHADERSTAGE_VERTEX, kVelocityObjectVertexDxbc,
        sizeof(kVelocityObjectVertexDxbc), kVelocityObjectVertexSpirv,
        sizeof(kVelocityObjectVertexSpirv), "VSVelocityObject", 0, 1, 1);
    velocity_object_ps = MakeShader(
        format, SDL_GPU_SHADERSTAGE_FRAGMENT, kVelocityObjectPixelDxbc,
        sizeof(kVelocityObjectPixelDxbc), kVelocityObjectPixelSpirv,
        sizeof(kVelocityObjectPixelSpirv), "PSVelocityObject", 1, 0, 1);
    gamma_shader = MakeShader(format, SDL_GPU_SHADERSTAGE_FRAGMENT, kGammaPixelDxbc,
                              sizeof(kGammaPixelDxbc), kGammaPixelSpirv, sizeof(kGammaPixelSpirv),
                              "PSGamma", 1, 0, 1);
    if (!vertex_shader || !pixel_shader || !spot_shader || !soft_shader || !shadow_shader ||
        !fullscreen_shader || !resolve_shader || !overlay_start_shader || !downsample_shader ||
        !blur_shader ||
        !glare_shader || !composite_shader || !composite_history_shader || !velocity_shader ||
        !velocity_object_vs || !velocity_object_ps || !gamma_shader)
        return false;
    resolve_pipeline = MakeFullscreenPipeline(resolve_shader, "resolve");
    downsample_pipeline = MakeFullscreenPipeline(downsample_shader, "downsample");
    blur_pipeline = MakeFullscreenPipeline(blur_shader, "blur");
    glare_pipeline = MakeFullscreenPipeline(glare_shader, "glare");
    composite_pipeline = MakeFullscreenPipeline(composite_shader, "composite");
    composite_history_pipeline =
        MakeFullscreenPipeline(composite_history_shader, "composite with history", 2);
    velocity_pipeline = MakeFullscreenPipeline(velocity_shader, "velocity");
    for (int c = 0; c < 3; c++)
        velocity_object_pipelines[c] = MakeVelocityObjectPipeline(CullWinding(c));
    gamma_pipeline = MakeFullscreenPipeline(gamma_shader, "gamma");
    if (!resolve_pipeline || !downsample_pipeline || !blur_pipeline || !glare_pipeline ||
        !composite_pipeline || !composite_history_pipeline || !velocity_pipeline ||
        !velocity_object_pipelines[0] || !velocity_object_pipelines[1] ||
        !velocity_object_pipelines[2] || !gamma_pipeline)
        return false;
    // Direct3D 12 and Vulkan both should
    depth_sampled = SDL_GPUTextureSupportsFormat(
        device, kDepthFormat, SDL_GPU_TEXTURETYPE_2D,
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!depth_sampled)
        REXLOG_WARN("native view gpu: the device can't sample a D32 depth buffer; the scene's "
                    "depth reads as 0, post-processing has no depth of field, the "
                    "spotlights' cones aren't drawn and the soft particles don't fade");
    // shadow depth is drawn as a colour and read by Load (Direct3D 12 and
    // Vulkan both should support it)
    shadow_maps = SDL_GPUTextureSupportsFormat(
        device, kShadowFormat, SDL_GPU_TEXTURETYPE_2D,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!shadow_maps)
        REXLOG_WARN("native view gpu: the device can't draw into an R32_FLOAT texture; the "
                    "characters are drawn without their self-shadows");
    // all four BcFormat formats, or every texture is RGBA8
    bc_formats = true;
    for (uint32_t xenos : {18u, 19u, 20u, 49u})
        bc_formats = bc_formats && SDL_GPUTextureSupportsFormat(device, BcFormat(xenos),
                                                                SDL_GPU_TEXTURETYPE_2D_ARRAY,
                                                                SDL_GPU_TEXTUREUSAGE_SAMPLER);
    bc_whole_blocks = std::strcmp(SDL_GetGPUDeviceDriver(device), "direct3d12") == 0;
    if (!bc_formats)
        REXLOG_WARN("native view gpu: the device can't sample BC1, BC2, BC3 or BC5 texture "
                    "arrays; compressed textures are sent as RGBA (native_bc_textures)");
    // and k_8's (KeptFormat), or they're RGBA8
    r8_format = SDL_GPUTextureSupportsFormat(device, SDL_GPU_TEXTUREFORMAT_R8_UNORM,
                                             SDL_GPU_TEXTURETYPE_2D_ARRAY,
                                             SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!r8_format)
        REXLOG_WARN("native view gpu: the device can't sample R8 texture arrays; one-channel "
                    "textures (movie planes) are sent as RGBA (native_r8_textures)");
    // Direct3D 12 and Vulkan both require 4 samples; every desktop GPU has 2
    for (int k = 0; k < 2; k++) {
        const SDL_GPUSampleCount count = k ? SDL_GPU_SAMPLECOUNT_4 : SDL_GPU_SAMPLECOUNT_2;
        ms_supported[k] = SDL_GPUTextureSupportsSampleCount(device, kColorFormat, count) &&
                          SDL_GPUTextureSupportsSampleCount(device, kDepthFormat, count);
    }

    // nearest and wrapping, as Shade() samples
    SDL_GPUSamplerCreateInfo si{};
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
    si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler = SDL_CreateGPUSampler(device, &si);
    si.min_filter = si.mag_filter = SDL_GPU_FILTER_LINEAR;
    si.address_mode_u = si.address_mode_v = si.address_mode_w =
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    linear_sampler = SDL_CreateGPUSampler(device, &si);
    // Inline mips on Direct3D 12 only, whose GenerateMipmaps they copy
    // (Vulkan's differs, untested); otherwise SDL's in a command buffer of
    // their own. The sampler is SDL's blit one: every level reachable.
    if (std::strcmp(SDL_GetGPUDeviceDriver(device), "direct3d12") == 0) {
        mip_vertex_shader = MakeShader(format, SDL_GPU_SHADERSTAGE_VERTEX, kMipVertexDxbc,
                                       sizeof(kMipVertexDxbc), kMipVertexSpirv,
                                       sizeof(kMipVertexSpirv), "VSMip", 0, 0, 0);
        mip_shader = MakeShader(format, SDL_GPU_SHADERSTAGE_FRAGMENT, kMipPixelDxbc,
                                sizeof(kMipPixelDxbc), kMipPixelSpirv, sizeof(kMipPixelSpirv),
                                "PSMip", 1, 0, 1);
        if (mip_vertex_shader && mip_shader)
            mip_pipeline = MakeFullscreenPipeline(mip_shader, "mips", 1, mip_vertex_shader);
        si.max_lod = 1000.0f;
        mip_sampler = SDL_CreateGPUSampler(device, &si);
        inline_mips_ok = mip_pipeline && mip_sampler;
        if (!inline_mips_ok)
            REXLOG_WARN("native view gpu: no pipeline for the texture passes' mips ({}); SDL "
                        "makes them, in command buffers of their own",
                        SDL_GetError());
    }

    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    ti.format = kColorFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = ti.height = 1;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    white = SDL_CreateGPUTexture(device, &ti);
    black = SDL_CreateGPUTexture(device, &ti);
    // plain 2D, as the resolve's depth binding is: a depth of 0
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    no_depth = SDL_CreateGPUTexture(device, &ti);

    SDL_GPUBufferCreateInfo bi{};
    bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    bi.size = sizeof(Mat4);
    no_bones = SDL_CreateGPUBuffer(device, &bi);

    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = kTextureOffsetAlign * 2;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(device, &tbi);
    if (!sampler || !linear_sampler || !white || !black || !no_depth || !no_bones || !tb) {
        REXLOG_WARN("native view gpu: couldn't make its sampler and buffers ({})",
                    SDL_GetError());
        if (tb) SDL_ReleaseGPUTransferBuffer(device, tb);
        return false;
    }
    auto* p = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device, tb, false));
    const uint32_t white_px = 0xffffffffu, black_px = 0;
    Mat4 identity{};
    for (int i = 0; i < 4; i++) identity.m[i][i] = 1.0f;
    if (p) {
        std::memcpy(p, &white_px, 4);
        std::memcpy(p + kTextureOffsetAlign, &black_px, 4);
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
        // a texture's upload starts kTextureOffsetAlign into the buffer
        src.offset = kTextureOffsetAlign;
        dst.texture = black;
        SDL_UploadToGPUTexture(copy, &src, &dst, false);
        dst.texture = no_depth;
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
        for (Buffer* b : {&arena_verts, &arena_indices, &old_arena_verts, &old_arena_indices,
                          &pool_verts[0], &pool_verts[1], &pool_indices[0], &pool_indices[1],
                          &bones})
            ReleaseBuffer(*b);
        for (auto& [k, a] : tex_arrays)
            if (a.texture) SDL_ReleaseGPUTexture(device, a.texture);
        for (auto& [k, rt] : rts) ReleaseRt(rt);
        for (auto& [k, p] : pipelines) SDL_ReleaseGPUGraphicsPipeline(device, p);
        for (SDL_GPUGraphicsPipeline* p :
             {resolve_pipeline, downsample_pipeline, blur_pipeline, glare_pipeline,
              composite_pipeline, composite_history_pipeline, velocity_pipeline, gamma_pipeline,
              velocity_object_pipelines[0], velocity_object_pipelines[1],
              velocity_object_pipelines[2], mip_pipeline})
            if (p) SDL_ReleaseGPUGraphicsPipeline(device, p);
        for (SDL_GPUShader* sh : {vertex_shader, pixel_shader, spot_shader, soft_shader,
                                  shadow_shader, fullscreen_shader, resolve_shader,
                                  overlay_start_shader,
                                  downsample_shader, blur_shader, glare_shader, composite_shader,
                                  composite_history_shader, velocity_shader, velocity_object_vs,
                                  velocity_object_ps, gamma_shader, mip_vertex_shader,
                                  mip_shader})
            if (sh) SDL_ReleaseGPUShader(device, sh);
        if (sampler) SDL_ReleaseGPUSampler(device, sampler);
        if (linear_sampler) SDL_ReleaseGPUSampler(device, linear_sampler);
        if (mip_sampler) SDL_ReleaseGPUSampler(device, mip_sampler);
        if (white) SDL_ReleaseGPUTexture(device, white);
        if (black) SDL_ReleaseGPUTexture(device, black);
        if (no_depth) SDL_ReleaseGPUTexture(device, no_depth);
        if (no_bones) SDL_ReleaseGPUBuffer(device, no_bones);
        ReleaseTargets();
        ReleaseHistory();
        ReleaseKept(post_buffer);
        ReleaseKept(pre_buffer);
        ReleaseOutputs();
        if (upload) SDL_ReleaseGPUTransferBuffer(device, upload);
        // (the GPU idle: no command list still writes the heap)
        ReleaseTiming();
        SDL_DestroyGPUDevice(device);
    }
    meshes.clear();
    textures.clear();
    tex_arrays.clear();
    rts.clear();
    rts_seen.clear();
    rts_forgotten.clear();
    // (the targets let go of theirs)
    shared_depths.clear();
    premake_pending.Clear();
    premake_textures.Clear();
    premade_since = arrays_premade_since = 0;
    premade_ms_since = 0;
    arena_vert_count = arena_index_count = 0;
    pipelines.clear();
    warm = false;
    warmed_up = false;
    pools_warm = false;
    device = nullptr;
    vertex_shader = pixel_shader = spot_shader = soft_shader = shadow_shader = nullptr;
    fullscreen_shader = nullptr;
    shadow_maps = false;
    bc_formats = bc_now = bc_whole_blocks = false;
    r8_format = r8_now = false;
    // (the CPU draws from now on: textures decoded as RGBA)
    SetKeepBlocks(false);
    SetKeepR8(false);
    resolve_shader = overlay_start_shader = nullptr;
    ms_supported[0] = ms_supported[1] = false;
    ms_fallback_logged = 0;
    ms_failure_logged = false;
    downsample_shader = blur_shader = glare_shader = composite_shader = gamma_shader = nullptr;
    composite_history_shader = velocity_shader = velocity_object_vs = velocity_object_ps =
        nullptr;
    for (auto& p : velocity_object_pipelines) p = nullptr;
    resolve_pipeline = downsample_pipeline = blur_pipeline = glare_pipeline = nullptr;
    composite_pipeline = composite_history_pipeline = velocity_pipeline = nullptr;
    gamma_pipeline = nullptr;
    mip_vertex_shader = mip_shader = nullptr;
    mip_pipeline = nullptr;
    mip_sampler = nullptr;
    inline_mips_ok = false;
    sampler = linear_sampler = nullptr;
    white = black = no_depth = nullptr;
    ReleaseTargets();  // released above: forgets them
    ReleaseHistory();
    ReleaseKept(post_buffer);
    ReleaseKept(pre_buffer);
    ReleaseOutputs();
    ahead = Ahead{};
    depth_sampled = false;
    no_bones = nullptr;
    upload = nullptr;
    upload_size = 0;
    if (stop_video) StopVideo();
}

SDL_GPUGraphicsPipeline* GpuRenderer::Impl::Pipeline(int blend, const DepthRules& rules,
                                                     AlphaMode alpha, CullWinding cull,
                                                     PixelKind pixel, uint32_t samples) {
    const int key = SampleBits(samples) << 12 | int(pixel) << 10 | int(cull) << 8 |
                    int(alpha) << 6 | blend << 3 | rules.Key();
    if (auto it = pipelines.find(key); it != pipelines.end()) return it->second;
    const auto start = std::chrono::steady_clock::now();

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
        {6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, uint32_t(offsetof(Vertex, tan))},
    };
    SDL_GPUColorTargetDescription target{};
    target.format = kColorFormat;
    // Blend() in soft_raster.cpp. The picture's alpha stays the resolve's 1,
    // as on the CPU. The target clamps colour to 0-1 before blending, as
    // Blend() does; SrcAlpha's scaling is done in the shader (kPremultiply),
    // to colour only, after its clamp
    SDL_GPUColorTargetBlendState& bs = target.blend_state;
    bs.enable_color_write_mask = true;
    SDL_GPUColorComponentFlags mask =
        SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B;
    if (blend == kBlendDest) mask = 0;
    if (alpha == AlphaMode::kTexture && blend != kBlendDest) mask |= SDL_GPU_COLORCOMPONENT_A;
    if (alpha == AlphaMode::kScene) mask |= SDL_GPU_COLORCOMPONENT_A;
    bs.color_write_mask = mask;
    bs.color_blend_op = SDL_GPU_BLENDOP_ADD;
    bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    bs.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    // colour factors, then alpha's
    auto factors = [&](SDL_GPUBlendFactor src, SDL_GPUBlendFactor dst, SDL_GPUBlendFactor src_a,
                       SDL_GPUBlendFactor dst_a) {
        bs.enable_blend = true;
        bs.src_color_blendfactor = src;
        bs.dst_color_blendfactor = dst;
        bs.src_alpha_blendfactor = src_a;
        bs.dst_alpha_blendfactor = dst_a;
    };
    switch (blend) {
        case kBlendAdd:
            factors(SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ONE,
                    SDL_GPU_BLENDFACTOR_ONE);
            break;
        case kBlendSrcAlphaAdd:  // the shader has scaled the colour by alpha
            factors(SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ONE,
                    SDL_GPU_BLENDFACTOR_SRC_ALPHA, SDL_GPU_BLENDFACTOR_ONE);
            break;
        case kBlendSrcAlpha:  // likewise
            factors(SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                    SDL_GPU_BLENDFACTOR_SRC_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA);
            break;
        case kBlendSubtract:  // dst - src
            factors(SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ONE,
                    SDL_GPU_BLENDFACTOR_ONE);
            bs.color_blend_op = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
            bs.alpha_blend_op = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
            break;
        case kBlendMultiply:
            factors(SDL_GPU_BLENDFACTOR_DST_COLOR, SDL_GPU_BLENDFACTOR_ZERO,
                    SDL_GPU_BLENDFACTOR_DST_ALPHA, SDL_GPU_BLENDFACTOR_ZERO);
            break;
        case kBlendPreMultAlpha:  // ONE INVSRCALPHA: the colour comes scaled by alpha
            factors(SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
                    SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA);
            break;
        default:  // Src, and Dest, which writes no colour
            break;
    }
    // the scene's alpha: MAX wherever RB3 blends (any mode but Src)
    if (alpha == AlphaMode::kScene && blend != kBlendSrc) {
        if (blend == kBlendDest) {
            bs.enable_blend = true;
            bs.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            bs.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        }
        bs.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        bs.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        bs.alpha_blend_op = SDL_GPU_BLENDOP_MAX;
    }
    // soft_raster.cpp's Target::zw
    const bool shadow = pixel == PixelKind::kShadowDepth;
    if (shadow) {
        target.format = kShadowFormat;
        bs = SDL_GPUColorTargetBlendState{};
    }

    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = vertex_shader;
    pi.fragment_shader = pixel == PixelKind::kSpot   ? spot_shader
                         : pixel == PixelKind::kSoft ? soft_shader
                         : shadow                    ? shadow_shader
                                                     : pixel_shader;
    pi.vertex_input_state.vertex_buffer_descriptions = &buffer;
    pi.vertex_input_state.num_vertex_buffers = 1;
    pi.vertex_input_state.vertex_attributes = attributes;
    pi.vertex_input_state.num_vertex_attributes = uint32_t(std::size(attributes));
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    // RasterTri's culling: SDL's winding is the screen's, as D3D's is;
    // clipping at depth 1 is the CPU's near plane (mesh.hlsl)
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pi.rasterizer_state.cull_mode = cull == CullWinding::kClockwise ? SDL_GPU_CULLMODE_BACK
                                    : cull == CullWinding::kCounterClockwise
                                        ? SDL_GPU_CULLMODE_FRONT
                                        : SDL_GPU_CULLMODE_NONE;
    pi.rasterizer_state.enable_depth_clip = true;
    // depth is larger-is-nearer, cleared to 0. Write-without-test uses
    // ALWAYS: a pipeline that doesn't test can't write
    pi.depth_stencil_state.enable_depth_test = rules.test || rules.write;
    pi.depth_stencil_state.enable_depth_write = rules.write;
    pi.depth_stencil_state.compare_op = !rules.test          ? SDL_GPU_COMPAREOP_ALWAYS
                                        : rules.equal_passes ? SDL_GPU_COMPAREOP_GREATER_OR_EQUAL
                                                             : SDL_GPU_COMPAREOP_GREATER;
    // a shadow map's is clip z/w, smaller nearer, cleared to the pass's
    // clear_z, LESS as RndShadowMap's ZFunc
    if (shadow) {
        pi.depth_stencil_state.enable_depth_test = true;
        pi.depth_stencil_state.enable_depth_write = true;
        pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;
    }
    pi.target_info.color_target_descriptions = &target;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = kDepthFormat;
    pi.target_info.has_depth_stencil_target = true;
    pi.multisample_state.sample_count = SampleCount(samples);
    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(device, &pi);
    if (!p) REXLOG_WARN("native view gpu: no pipeline ({})", SDL_GetError());
    pipelines[key] = p;
    counts.pipelines++;
    // Prewarm missed it and a frame waited (add it there)
    if (warmed_up) {
        REXLOG_INFO("native view gpu: pipeline made after warm-up: {:#x} (pixel {}, cull {}, "
                    "alpha {}, blend {}, depth rules {}, samples {}) ({:.1f} ms)",
                    key, int(pixel), int(cull), int(alpha), blend, rules.Key(), samples,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              start).count());
    }
    return p;
}

SDL_GPUGraphicsPipeline* GpuRenderer::Impl::OverlayStartPipeline(uint32_t samples) {
    const int key = 1 << 16 | SampleBits(samples);
    if (auto it = pipelines.find(key); it != pipelines.end()) return it->second;
    const auto start = std::chrono::steady_clock::now();
    SDL_GPUColorTargetDescription target{};
    target.format = kColorFormat;
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = fullscreen_shader;
    pi.fragment_shader = overlay_start_shader;
    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    // its depth everywhere, whatever is there
    pi.depth_stencil_state.enable_depth_test = true;
    pi.depth_stencil_state.enable_depth_write = true;
    pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    pi.multisample_state.sample_count = SampleCount(samples);
    pi.target_info.color_target_descriptions = &target;
    pi.target_info.num_color_targets = 1;
    pi.target_info.depth_stencil_format = kDepthFormat;
    pi.target_info.has_depth_stencil_target = true;
    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(device, &pi);
    if (!p) REXLOG_WARN("native view gpu: no overlay start pipeline ({})", SDL_GetError());
    pipelines[key] = p;
    counts.pipelines++;
    if (warmed_up) {
        REXLOG_INFO("native view gpu: pipeline made after warm-up: the overlay's start at {} "
                    "samples ({:.1f} ms)",
                    samples,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                              start).count());
    }
    return p;
}

uint32_t GpuRenderer::Impl::DeviceSamples(uint32_t want) {
    if (want != 2 && want != 4) return 1;
    const bool two = ms_supported[0], four = ms_supported[1];
    const uint32_t got = want == 2 ? (two ? 2 : four ? 4 : 1) : (four ? 4 : two ? 2 : 1);
    if (got != want && !(ms_fallback_logged & want)) {
        ms_fallback_logged |= want;
        REXLOG_WARN("native view gpu: the device can't draw {} samples a pixel; the overlay "
                    "is drawn with {}",
                    want, got);
    }
    return got;
}

bool GpuRenderer::Impl::EnsureOverlayTargets(uint32_t samples) {
    if (color_ms && ms_samples == samples && ms_w == width && ms_h == height) return true;
    if (color_ms) SDL_ReleaseGPUTexture(device, color_ms);
    if (depth_ms) SDL_ReleaseGPUTexture(device, depth_ms);
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.width = width;
    ti.height = height;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ti.sample_count = SampleCount(samples);
    ti.format = kColorFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    color_ms = SDL_CreateGPUTexture(device, &ti);
    ti.format = kDepthFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    depth_ms = SDL_CreateGPUTexture(device, &ti);
    if (!color_ms || !depth_ms) {
        if (!ms_failure_logged)
            REXLOG_WARN("native view gpu: no {}x{} target at {} samples ({}); the overlay is "
                        "drawn single-sampled",
                        width, height, samples, SDL_GetError());
        ms_failure_logged = true;
        if (color_ms) SDL_ReleaseGPUTexture(device, color_ms);
        if (depth_ms) SDL_ReleaseGPUTexture(device, depth_ms);
        color_ms = depth_ms = nullptr;
        ms_w = ms_h = 0;
        return false;
    }
    ms_samples = samples;
    ms_w = width;
    ms_h = height;
    return true;
}

void GpuRenderer::Impl::Prewarm(uint32_t overlay_samples) {
    warm = true;
    const auto start = std::chrono::steady_clock::now();
    // RulesFor's, blending on and off; culling none, D3DCULL_CW (RndMat's
    // cull flag) or D3DCULL_CCW (reflections, with several blends and alphas)
    constexpr DepthRules kRules[] = {{false, false, false}, {true, true, false},
                                     {false, false, true},  {true, true, true},
                                     {true, false, true}};
    for (CullWinding cull :
         {CullWinding::kNone, CullWinding::kClockwise, CullWinding::kCounterClockwise})
        for (int alpha = 0; alpha < kNumAlphaModes; alpha++)
            for (int blend = kBlendDest; blend <= kBlendPreMultAlpha; blend++)
                for (const DepthRules& r : kRules) Pipeline(blend, r, AlphaMode(alpha), cull);
    // spotlight cones: Add into the depthless depth volume (RenderConeDefs
    // sets D3DCULL_CCW)
    for (CullWinding cull :
         {CullWinding::kNone, CullWinding::kClockwise, CullWinding::kCounterClockwise})
        Pipeline(kBlendAdd, {false, false, false}, AlphaMode::kTexture, cull, PixelKind::kSpot);
    // soft particles: into the depthless soft-particle buffer, never culled
    for (int blend = kBlendDest; blend <= kBlendPreMultAlpha; blend++)
        Pipeline(blend, {false, false, false}, AlphaMode::kTexture, CullWinding::kNone,
                 PixelKind::kSoft);
    // shadow maps (PrepShadow sets D3DCULL_CCW)
    if (shadow_maps)
        for (CullWinding cull :
             {CullWinding::kNone, CullWinding::kClockwise, CullWinding::kCounterClockwise})
            ShadowDepthPipeline(cull);
    // the multisampled overlay's (AlphaMode::kNone): at the game's 2 samples
    // whatever the setting, so turning it back on doesn't stall, and at the
    // setting's
    for (uint32_t samples : {DeviceSamples(2), DeviceSamples(overlay_samples)}) {
        if (samples <= 1) continue;
        OverlayStartPipeline(samples);
        for (CullWinding cull :
             {CullWinding::kNone, CullWinding::kClockwise, CullWinding::kCounterClockwise})
            for (int blend = kBlendDest; blend <= kBlendPreMultAlpha; blend++)
                for (const DepthRules& r : kRules)
                    Pipeline(blend, r, AlphaMode::kNone, cull, PixelKind::kMesh, samples);
    }
    if (upload_size < kInitialUploadBytes) {
        if (upload) SDL_ReleaseGPUTransferBuffer(device, upload);
        SDL_GPUTransferBufferCreateInfo tbi{};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size = kInitialUploadBytes;
        upload = SDL_CreateGPUTransferBuffer(device, &tbi);
        upload_size = upload ? tbi.size : 0;
    }
    REXLOG_INFO("native view gpu: {} pipelines and a {} MB upload buffer in {:.1f} ms",
                pipelines.size(), upload_size >> 20,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                          start).count());
    warmed_up = true;
}

void GpuRenderer::Impl::WarmPools() {
    if (pools_warm || !device || !gamma_pipeline) return;
    pools_warm = true;
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    auto ms = [](Clock::time_point from, Clock::time_point to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    };
    // what the draws draw into, let go after
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = kColorFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    ti.width = ti.height = 4;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(device, &ti);
    if (!target) {
        REXLOG_WARN("native view gpu: SDL's pools not warmed ({})", SDL_GetError());
        return;
    }
    // All held at once, so each pool has to grow to hold them
    SDL_GPUCommandBuffer* cmds[kWarmCommandBuffers] = {};
    uint32_t got = 0;
    while (got < kWarmCommandBuffers && (cmds[got] = SDL_AcquireGPUCommandBuffer(device))) got++;
    const auto acquired = Clock::now();
    // A push that doesn't fit its uniform buffer takes another: 16 KB blocks
    // take one each. Never read (the vertex stage of what's drawn below has
    // no uniforms).
    static const uint8_t block[16384] = {};
    if (got) {
        for (uint32_t u = 0; u < kWarmUniformBuffers; u++)
            SDL_PushGPUVertexUniformData(cmds[u % got], 0, block, sizeof(block));
    }
    const auto pushed = Clock::now();
    // Draws in each, taking its descriptor heaps: the gamma pass's, through
    // BeginPass as every pass (kSamplerBatch). A draw after a rebind writes
    // kSamplerBatch samplers, so the sampler heap (2048) is full after 128
    // and the next takes a new pair, as in a frame's big command buffers
    // (about 20 pairs for 1400 draws): the first `extra` buffers draw 128
    // more for each pair over one, rebinding slot 1 (which the gamma pass
    // doesn't read) between white and black
    const uint32_t extra = got ? kWarmHeapPairs - std::min(kWarmHeapPairs, got) : 0;
    uint32_t lut[256] = {};
    for (uint32_t i = 0; i < got; i++) {
        SDL_GPUColorTargetInfo ct{};
        ct.texture = target;
        ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* rp = BeginPass(cmds[i], &ct, 1, nullptr);
        if (!rp) continue;
        SDL_BindGPUGraphicsPipeline(rp, gamma_pipeline);
        const SDL_GPUTextureSamplerBinding tb{no_depth, sampler};
        SDL_BindGPUFragmentSamplers(rp, 0, &tb, 1);
        SDL_PushGPUFragmentUniformData(cmds[i], 0, lut, sizeof(lut));
        const uint32_t pairs = 1 + extra / got + (i < extra % got ? 1 : 0);
        const uint32_t draws = 1 + (pairs - 1) * (2048 / kSamplerBatch);
        for (uint32_t d = 0; d < draws; d++) {
            if (d) {
                const SDL_GPUTextureSamplerBinding pad{d & 1 ? black : white, sampler};
                SDL_BindGPUFragmentSamplers(rp, 1, &pad, 1);
            }
            SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        }
        SDL_EndGPURenderPass(rp);
    }
    const auto drawn = Clock::now();
    // submitted together, so each takes a fence of its own; the last one's,
    // on SDL's one queue, covers all, and its wait returns them to the pools
    bool ok = got > 0;
    for (uint32_t i = 0; i + 1 < got; i++) ok &= SDL_SubmitGPUCommandBuffer(cmds[i]);
    SDL_GPUFence* fence = got ? SDL_SubmitGPUCommandBufferAndAcquireFence(cmds[got - 1]) : nullptr;
    const auto submitted = Clock::now();
    ok = ok && fence && SDL_WaitForGPUFences(device, true, &fence, 1);
    if (fence) SDL_ReleaseGPUFence(device, fence);
    SDL_ReleaseGPUTexture(device, target);
    const auto end = Clock::now();
    if (!ok) {
        REXLOG_WARN("native view gpu: warming SDL's pools failed ({}); frames grow them as before",
                    SDL_GetError());
        return;
    }
    REXLOG_INFO("native view gpu: SDL's pools warmed with {} command buffers and fences, {} "
                "uniform buffers ({} MB) and {} descriptor heap pairs (about 2 MB each), in "
                "{:.1f} ms: acquiring {:.1f}, pushing {:.1f}, drawing {:.1f}, submitting {:.1f}, "
                "waiting {:.1f}",
                got, kWarmUniformBuffers, kWarmUniformBuffers * 64 / 1024, got + extra,
                ms(start, end),
                ms(start, acquired), ms(acquired, pushed), ms(pushed, drawn), ms(drawn, submitted),
                ms(submitted, end));
}

bool GpuRenderer::Impl::Reserve(Buffer& b, SDL_GPUBufferUsageFlags usage, uint32_t bytes) {
    if (b.buffer && b.size >= bytes) return true;
    ReleaseBuffer(b);
    SDL_GPUBufferCreateInfo bi{};
    bi.usage = usage;
    // room to grow, so a frame a little bigger than the last doesn't remake it
    bi.size = std::max(Align(bytes + bytes / 2, 1u << 16), 1u << 16);
    b.buffer = SDL_CreateGPUBuffer(device, &bi);
    counts.buffers++;
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
        // Full: start over with the meshes still kept plus this frame's new
        // ones, within kMaxArenaBytes (ArenaRebuildKeep). Kept meshes are
        // copied from the old buffers on the GPU (Mesh::arena_vertex); the
        // rest come back through the pool if drawn again.
        counts.arena_rebuilds++;
        auto bytes_of = [](const Geometry& g) {
            return uint64_t(g.verts.size()) * sizeof(Vertex) + uint64_t(IndexSlots(g)) * 2;
        };
        const uint64_t new_bytes = uint64_t(verts) * sizeof(Vertex) + uint64_t(indices) * 2;
        uint64_t keep_bytes = new_bytes, frames_bytes = new_bytes;
        for (const auto& [geom, m] : meshes) {
            if (!m.in_arena) continue;
            const double idle = IdleSeconds(m.used_at);
            const double keep_seconds = KeepSecondsOf(m);
            if (KeptByRebuild(ArenaKeep::kKeep, m.used, serial, kEvictAfter, idle, keep_seconds))
                keep_bytes += bytes_of(*m.keep);
            if (KeptByRebuild(ArenaKeep::kFrames, m.used, serial, kEvictAfter, idle, keep_seconds))
                frames_bytes += bytes_of(*m.keep);
        }
        const ArenaKeep what = ArenaRebuildKeep(keep_bytes, frames_bytes, kMaxArenaBytes);
        for (auto it = meshes.begin(); it != meshes.end();) {
            Mesh& m = it->second;
            if (!m.in_arena) {
                ++it;
                continue;
            }
            m.in_arena = false;
            const double idle = IdleSeconds(m.used_at);
            const double keep_seconds = KeepSecondsOf(m);
            if (!KeptByRebuild(what, m.used, serial, kEvictAfter, idle, keep_seconds)) {
                // past its keep, or within it but let go for room
                (KeptByRebuild(ArenaKeep::kKeep, m.used, serial, kEvictAfter, idle, keep_seconds)
                     ? counts.meshes_pressured
                     : counts.evicted_meshes)++;
                it = meshes.erase(it);
                continue;
            }
            m.pool_vertex = ~0u;
            m.arena_vertex = m.first_vertex;
            m.arena_index = m.first_index;
            to_arena.push_back(&m);
            verts += uint32_t(m.keep->verts.size());
            indices += IndexSlots(*m.keep);
            ++it;
        }
        // The old buffers live until this frame's copy pass copies out of
        // them and releases them. Ones a failed frame left go first.
        ReleaseBuffer(old_arena_verts);
        ReleaseBuffer(old_arena_indices);
        old_arena_verts = std::exchange(arena_verts, Buffer{});
        old_arena_indices = std::exchange(arena_indices, Buffer{});
        arena_vert_count = arena_index_count = 0;
        // twice what goes in, with Reserve's extra half, so rebuilds are rare
        auto twice = [](uint64_t bytes, uint32_t least) {
            return uint32_t(std::clamp<uint64_t>(bytes + bytes / 3, least, 1u << 30));
        };
        if (!Reserve(arena_verts, SDL_GPU_BUFFERUSAGE_VERTEX,
                     twice(uint64_t(verts) * sizeof(Vertex), kMinArenaBytes)) ||
            !Reserve(arena_indices, SDL_GPU_BUFFERUSAGE_INDEX,
                     twice(uint64_t(indices) * 2, kMinArenaBytes / 4))) {
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

bool GpuRenderer::Impl::PlaceTexture(Tex& tx, SDL_GPUTextureFormat format) {
    const Texture& t = *tx.keep;
    uint32_t w, h;
    SizeClass(t.width, t.height, w, h);
    TexArray& a = tex_arrays[ArrayKey(w, h, format)];
    a.w = w;
    a.h = h;
    a.format = format;
    // level 0's
    const uint64_t layer_bytes = uint64_t(w) * h * TexelBits(format) / 8;
    if (a.free.empty() && a.texture) {
        // Full: before growing, textures kept by the clock alone (unused for
        // kEvictAfter frames) make room. Their layers are safe to reuse: no
        // frame in flight samples them, and uploads come after this frame's
        // ArrayCopy. One drawn later this frame is placed and sent again.
        for (auto it = textures.begin(); it != textures.end();) {
            Tex& held = it->second;
            if (&held == &tx || held.array != &a ||
                WithinKeep(held.used, serial, kEvictAfter, 0, 0)) {
                ++it;
                continue;
            }
            LetTextureGo(held);
            it = textures.erase(it);
            counts.textures_pressured++;
        }
    }
    if (a.free.empty()) {
        const auto start = std::chrono::steady_clock::now();
        const uint32_t layers = a.layers ? a.layers * 2 : FirstArrayLayers(w, h, format);
        SDL_GPUTexture* grown =
            layers <= kMaxTextureLayers ? NewArrayTexture(w, h, format, layers) : nullptr;
        if (!grown) {
            if (!texture_failure_logged) {
                texture_failure_logged = true;
                REXLOG_WARN("native view gpu: no {}x{} texture array of {} in format {} ({}); "
                            "draws untextured",
                            w, h, layers, int(format), SDL_GetError());
            }
            return false;
        }
        if (a.texture) {
            // copied in this frame's copy pass before any upload (ArrayCopy)
            ArrayCopy c{a.texture, grown, w, h, IsBc(format), {}};
            for (const auto& [key, held] : textures)
                if (held.array == &a && held.first < serial)
                    c.layers.emplace_back(held.layer, held.levels);
            array_copies.push_back(std::move(c));
        }
        for (uint32_t l = layers; l-- > a.layers;) a.free.push_back(l);
        a.texture = grown;
        a.layers = layers;
        a.levels = FullMipChain(w, h);
        if (walk_stats) {
            walk_stats->arrays_grown++;
            walk_stats->arrays_mb +=
                double(layer_bytes) * layers * (a.levels > 1 ? 4.0 / 3 : 1) / 1048576;
            walk_stats->arrays_ms += std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - start)
                                         .count();
        }
    }
    if (a.ahead_until != std::chrono::steady_clock::time_point{}) {
        a.ahead_until = {};
        if (walk_stats) walk_stats->arrays_premade_used++;
    }
    tx.array = &a;
    tx.levels = std::min(LevelsOf(t, format != kColorFormat), a.levels);
    tx.layer = a.free.back();
    a.free.pop_back();
    return true;
}

SDL_GPUTexture* GpuRenderer::Impl::NewArrayTexture(uint32_t w, uint32_t h,
                                                   SDL_GPUTextureFormat format, uint32_t layers) {
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    ti.format = format;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = layers;
    ti.num_levels = FullMipChain(w, h);
    counts.textures++;
    return SDL_CreateGPUTexture(device, &ti);
}

void GpuRenderer::Impl::LetTextureGo(Tex& tx) {
    if (!tx.array) return;
    tx.array->free.push_back(tx.layer);
    if (tx.array->free.size() == tx.array->layers) tx.array->empty_since = serial;
    tx.array = nullptr;
}

void GpuRenderer::Impl::UseTexture(const std::shared_ptr<const Texture>& t) {
    if (!t || !t->width || !t->height) return;
    auto it = textures.find(t.get());
    if (it == textures.end()) {
        // New: as blocks (or k_8 bytes) if it has them and this frame keeps
        // them (RasterOptions::bc_textures, r8_textures), else RGBA
        // (EnsureRgba); fixed until it's let go. DecodeDeferred waits if
        // another thread is decoding it.
        if (t->deferred) DecodeDeferred(*t);
        SDL_GPUTextureFormat format = kColorFormat;
        const SDL_GPUTextureFormat kept =
            t->blocks ? KeptFormat(t->blocks->format) : SDL_GPU_TEXTUREFORMAT_INVALID;
        if (kept != SDL_GPU_TEXTUREFORMAT_INVALID &&
            (kept == SDL_GPU_TEXTUREFORMAT_R8_UNORM ? r8_now : bc_now) &&
            t->blocks->level0.size() ==
                guest_format::LevelBlockBytes(t->blocks->format, t->width, t->height)) {
            format = kept;
        } else {
            EnsureRgba(*t);
            if (t->rgba.size() != size_t(t->width) * t->height) return;
        }
        Tex& tx = textures[t.get()];
        tx.keep = t;
        tx.first = serial;
        tx.first_world = frame_world;
        if (walk_stats) walk_stats->textures_first++;
        if (PlaceTexture(tx, format)) new_textures.push_back(&tx);
        it = textures.find(t.get());
    }
    Tex& tx = it->second;
    tx.used = serial;
    tx.used_at = frame_now;
    tx.drawn_in_song = frame_in_song;
    if (frame_world != tx.first_world) tx.across_worlds = true;
}

double GpuRenderer::Impl::TextureArrayMb() const {
    double mb = 0;
    for (const auto& [key, a] : tex_arrays) {
        const double layer = double(a.w) * a.h * TexelBits(a.format) / 8;
        mb += layer * a.layers * (a.levels > 1 ? 4.0 / 3 : 1) / 1048576;
    }
    return mb;
}

const GpuRenderer::Impl::Tex* GpuRenderer::Impl::TextureFor(const Texture* t) {
    if (!t) return nullptr;
    auto it = textures.find(t);
    return it != textures.end() && it->second.array ? &it->second : nullptr;
}

GpuRenderer::Impl::Rt* GpuRenderer::Impl::TargetFor(const Pass& p, uint32_t w, uint32_t h,
                                                    bool share) {
    Rt& rt = rts[p.tex_obj];
    UseRt(rt);
    rt.game_w = p.width;
    rt.game_h = p.height;
    // read by Load: no mips
    const bool shadow = p.tex_type == kTexTypeShadowMap;
    const uint32_t levels = TargetLevels(p.num_mips, w, h, shadow);
    const bool may_share = SharesDepth(p.tex_type, shadow);
    if (rt.color && rt.w == w && rt.h == h && rt.levels == levels && rt.shadow == shadow &&
        (may_share || !rt.depth_shared)) {
        if (rt.premade && walk_stats) walk_stats->premade_used++;
        rt.premade = false;
        return &rt;
    }
    const auto start = std::chrono::steady_clock::now();
    const bool resized = rt.color != nullptr;
    ReleaseRt(rt);
    if (!MakeRt(rt, w, h, levels, shadow, share && may_share)) {
        rts.erase(p.tex_obj);
        return nullptr;
    }
    const bool returning = !rts_seen.insert(p.tex_obj).second && !resized;
    if (walk_stats) {
        walk_stats->targets_made++;
        (resized ? walk_stats->targets_resized : walk_stats->targets_new)++;
        if (returning) walk_stats->targets_returning++;
        walk_stats->targets_ms += std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - start)
                                      .count();
    }
    return &rt;
}

bool GpuRenderer::Impl::MakeRt(Rt& rt, uint32_t w, uint32_t h, uint32_t levels, bool shadow,
                               bool share) {
    SDL_GPUTextureCreateInfo ti{};
    ti.type = shadow ? SDL_GPU_TEXTURETYPE_2D : SDL_GPU_TEXTURETYPE_2D_ARRAY;
    ti.format = shadow ? kShadowFormat : kColorFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = levels;
    rt.color = SDL_CreateGPUTexture(device, &ti);
    counts.textures++;
    if (share) {
        rt.depth = TakeSharedDepth(w, h);
        rt.depth_shared = rt.depth != nullptr;
    } else {
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = kDepthFormat;
        ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        ti.num_levels = 1;
        rt.depth = SDL_CreateGPUTexture(device, &ti);
    }
    // (set first: the failure's ReleaseRt gives a shared depth back by size)
    rt.w = w;
    rt.h = h;
    if (!rt.color || !rt.depth) {
        if (!rt_failure_logged) {
            rt_failure_logged = true;
            REXLOG_WARN("native view gpu: no {}x{} render target ({}); what samples it draws "
                        "transparent black",
                        w, h, SDL_GetError());
        }
        ReleaseRt(rt);
        return false;
    }
    rt.levels = levels;
    rt.shadow = shadow;
    return true;
}

SDL_GPUTexture* GpuRenderer::Impl::TakeSharedDepth(uint32_t w, uint32_t h) {
    SharedDepth& d = shared_depths[uint64_t(w) << 32 | h];
    if (!d.texture) {
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = kDepthFormat;
        ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        ti.width = w;
        ti.height = h;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        d.texture = SDL_CreateGPUTexture(device, &ti);
        if (!d.texture) {
            shared_depths.erase(uint64_t(w) << 32 | h);
            return nullptr;
        }
    }
    d.refs++;
    return d.texture;
}

void GpuRenderer::Impl::DropSharedDepth(uint32_t w, uint32_t h) {
    const auto f = shared_depths.find(uint64_t(w) << 32 | h);
    if (f == shared_depths.end() || --f->second.refs) return;
    SDL_ReleaseGPUTexture(device, f->second.texture);
    shared_depths.erase(f);
}

void GpuRenderer::Impl::ReleaseRt(Rt& rt) {
    // SDL lets them go once the frames using them are done
    if (rt.color) SDL_ReleaseGPUTexture(device, rt.color);
    if (rt.depth && rt.depth_shared) DropSharedDepth(rt.w, rt.h);
    else if (rt.depth) SDL_ReleaseGPUTexture(device, rt.depth);
    rt.color = rt.depth = nullptr;
    rt.w = rt.h = 0;
    rt.levels = 1;
    rt.shadow = false;
    rt.depth_shared = false;
    rt.premade = false;
    rt.drawn = false;
}

bool GpuRenderer::Impl::Premake(const RasterOptions& o) {
    TakeAnnouncedTargets(premake_pending);
    TakeAnnouncedTextures(premake_textures);
    if (!o.premake_targets) premake_pending.Clear();
    if (!o.premake_arrays) premake_textures.Clear();
    if (premake_pending.Empty() && premake_textures.Empty()) return false;
    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
    };
    uint32_t unused = 0;
    for (const auto& [obj, rt] : rts) unused += rt.premade ? 1 : 0;
    auto room = [&] { return unused < PremakeBudget::kMaxUnused && rts.size() < kMaxRts; };
    uint32_t made = 0, arrays = 0;
    // Arrays first: a song's textures are announced 0.1 to 0.2 s before its
    // first frame, its targets seconds before. A class's first array as
    // PlaceTexture makes it, in the format UseTexture would keep the texture
    // in (as blocks or bytes where this frame's options keep them)
    while (!premake_textures.Empty() && PremakeMore(made, elapsed(), 0)) {
        const AnnouncedTexture t = premake_textures.Front();
        premake_textures.PopFront();
        SDL_GPUTextureFormat format = kColorFormat;
        const SDL_GPUTextureFormat kept = KeptFormat(t.xenos_format);
        if (kept != SDL_GPU_TEXTUREFORMAT_INVALID &&
            (kept == SDL_GPU_TEXTUREFORMAT_R8_UNORM ? r8_format && o.r8_textures
                                                    : bc_formats && o.bc_textures))
            format = kept;
        uint32_t w, h;
        SizeClass(t.width, t.height, w, h);
        const uint64_t key = ArrayKey(w, h, format);
        if (tex_arrays.count(key)) continue;
        const uint32_t layers = FirstArrayLayers(w, h, format);
        SDL_GPUTexture* texture = NewArrayTexture(w, h, format, layers);
        if (!texture) break;
        TexArray& a = tex_arrays[key];
        a.texture = texture;
        a.w = w;
        a.h = h;
        a.format = format;
        a.layers = layers;
        a.levels = FullMipChain(w, h);
        for (uint32_t l = layers; l-- > 0;) a.free.push_back(l);
        a.empty_since = serial;
        a.ahead_until = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(kArrayKeepSeconds));
        made++;
        arrays++;
    }
    // sized as TargetFor would without the frame (target_premake.h)
    static const FrameCapture kNoFrame;
    while (!premake_pending.Empty() && PremakeMore(made, elapsed(), unused) && room()) {
        const AnnouncedTarget a = premake_pending.Front();
        premake_pending.PopFront();
        // never one a pass has: remaking it would forget what it drew
        if (const auto f = rts.find(a.tex_obj); f != rts.end() && f->second.color) continue;
        Pass p;
        p.tex_obj = a.tex_obj;
        p.width = a.width;
        p.height = a.height;
        p.tex_type = a.tex_type;
        p.num_mips = a.num_mips;
        uint32_t w, h;
        PassTargetSize(kNoFrame, p, o, w, h);
        const bool shadow = p.tex_type == kTexTypeShadowMap;
        Rt& rt = rts[a.tex_obj];
        if (!MakeRt(rt, w, h, TargetLevels(p.num_mips, w, h, shadow), shadow,
                    SharesDepth(p.tex_type, shadow))) {
            rts.erase(a.tex_obj);
            break;
        }
        rt.premade = true;
        rt.game_w = a.width;
        rt.game_h = a.height;
        UseRt(rt);
        rt.used_at = std::chrono::steady_clock::now();
        made++;
        unused++;
    }
    premade_since += made - arrays;
    arrays_premade_since += arrays;
    premade_ms_since += elapsed();
    // more to do now (not targets held back by the cap)
    return !premake_textures.Empty() || (!premake_pending.Empty() && room());
}

// the frame's targets, at the picture's size; with no device, only forgets them
void GpuRenderer::Impl::ReleaseTargets() {
    if (device) {
        for (SDL_GPUTexture* t : {scene, color, depth, behind, graded, post_dof, post_velocity,
                                  post_velocity_depth, post_bloom[0],
                                  post_bloom[1], post_bloom[2], post_tmp[0], post_tmp[1],
                                  post_tmp[2], spot_scratch.texture, light_scratch.texture,
                                  soft_scratch.texture, pre_scratch.texture, color_ms,
                                  depth_ms})
            if (t) SDL_ReleaseGPUTexture(device, t);
        if (readback) SDL_ReleaseGPUTransferBuffer(device, readback);
    }
    scene = color = depth = behind = graded = post_dof = post_velocity = nullptr;
    post_velocity_depth = nullptr;
    velocity_w = velocity_h = 0;
    color_ms = depth_ms = nullptr;
    ms_samples = 1;
    ms_w = ms_h = 0;
    spot_scratch = light_scratch = soft_scratch = pre_scratch = Scratch{};
    for (int k = 0; k < 3; k++) post_bloom[k] = post_tmp[k] = nullptr;
    readback = nullptr;
    width = height = 0;
}

void GpuRenderer::Impl::ReleaseHistory() {
    if (device)
        for (SDL_GPUTexture* t : history.tex)
            if (t) SDL_ReleaseGPUTexture(device, t);
    history = History{};
}

bool GpuRenderer::Impl::EnsureHistory(uint32_t w, uint32_t h) {
    if (history.tex[0] && history.tex[1] && history.w == w && history.h == h) return true;
    ReleaseHistory();
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ti.format = kColorFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    for (SDL_GPUTexture*& t : history.tex) t = SDL_CreateGPUTexture(device, &ti);
    if (!history.tex[0] || !history.tex[1]) {
        ReleaseHistory();
        return false;
    }
    history.w = w;
    history.h = h;
    return true;
}

void GpuRenderer::Impl::ReleaseKept(PostBuffer& b) {
    if (device && b.tex) SDL_ReleaseGPUTexture(device, b.tex);
    b = PostBuffer{};
}

bool GpuRenderer::Impl::EnsureKept(PostBuffer& b, uint32_t w, uint32_t h) {
    if (b.tex && b.w == w && b.h == h) return true;
    ReleaseKept(b);
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ti.format = kColorFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    b.tex = SDL_CreateGPUTexture(device, &ti);
    counts.textures++;
    if (!b.tex) return false;
    b.w = w;
    b.h = h;
    return true;
}

bool GpuRenderer::Impl::EnsureTargets(uint32_t w, uint32_t h) {
    if (color && w == width && h == height) return true;
    ReleaseTargets();
    counts.textures++;

    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    ti.format = kColorFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    graded = SDL_CreateGPUTexture(device, &ti);
    // the gamma ramp's pass reads the picture
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    color = SDL_CreateGPUTexture(device, &ti);
    scene = SDL_CreateGPUTexture(device, &ti);
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    behind = SDL_CreateGPUTexture(device, &ti);
    ti.format = kDepthFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    if (depth_sampled) ti.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
    depth = SDL_CreateGPUTexture(device, &ti);
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size = w * h * 4;
    readback = SDL_CreateGPUTransferBuffer(device, &tbi);
    // post-processing's levels, each a quarter of the last
    ti.format = kColorFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    bool levels = true;
    for (int k = 0; k < 3; k++) {
        post_w[k] = post::Quarter(k ? post_w[k - 1] : w);
        post_h[k] = post::Quarter(k ? post_h[k - 1] : h);
        ti.width = post_w[k];
        ti.height = post_h[k];
        post_bloom[k] = SDL_CreateGPUTexture(device, &ti);
        post_tmp[k] = SDL_CreateGPUTexture(device, &ti);
        levels &= post_bloom[k] && post_tmp[k];
        if (k == 0) {
            post_dof = SDL_CreateGPUTexture(device, &ti);
            levels &= post_dof != nullptr;
        }
    }
    velocity_w = std::max(w / 2, 1u);
    velocity_h = std::max(h / 2, 1u);
    ti.width = velocity_w;
    ti.height = velocity_h;
    post_velocity = SDL_CreateGPUTexture(device, &ti);
    levels &= post_velocity != nullptr;
    ti.format = kDepthFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    post_velocity_depth = SDL_CreateGPUTexture(device, &ti);
    levels &= post_velocity_depth != nullptr;
    if (!scene || !color || !depth || !behind || !graded || !readback || !levels) {
        REXLOG_WARN("native view gpu: no {}x{} target ({})", w, h, SDL_GetError());
        return false;
    }
    width = w;
    height = h;
    return true;
}

bool GpuRenderer::Impl::EnsureScratch(Scratch& s, uint32_t w, uint32_t h) {
    if (s.texture && s.w == w && s.h == h) return true;
    if (s.texture) SDL_ReleaseGPUTexture(device, s.texture);
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = kColorFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    s.texture = SDL_CreateGPUTexture(device, &ti);
    counts.textures++;
    s.w = s.texture ? w : 0;
    s.h = s.texture ? h : 0;
    if (!s.texture)
        REXLOG_WARN("native view gpu: no {}x{} texture for a blur ({})", w, h, SDL_GetError());
    return s.texture != nullptr;
}

namespace {

SDL_GPUTextureCreateInfo OutputInfo(uint32_t w, uint32_t h) {
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = kColorFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = w;
    ti.height = h;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    return ti;
}

#ifdef _WIN32
// SDL 3.4.14's private Direct3D 12 texture (SDL_gpu_d3d12.c's
// D3D12TextureContainer and D3D12Texture, SDL_sysgpu.h's TextureCommonHeader).
// Read only, and untrusted until SdkResource's checks pass.
struct SdlD3D12Texture;
struct SdlD3D12TextureContainer {
    SDL_GPUTextureCreateInfo info;
    SdlD3D12Texture* active_texture;
    SdlD3D12Texture** textures;
    Uint32 texture_capacity;
    Uint32 texture_count;
    bool can_be_cycled;
    char* debug_name;
};
struct SdlD3D12Texture {
    SdlD3D12TextureContainer* container;
    Uint32 container_index;
    void* subresources;
    Uint32 subresource_count;
    ID3D12Resource* resource;
    // the SRV's staging descriptor and the reference count follow
};

// SDL 3.4.14's private command buffer (SDL_sysgpu.h's
// CommandBufferCommonHeader and passes, SDL_gpu_d3d12.c's D3D12CommandBuffer;
// no #ifdefs). Read only, for TimingList's command list; untrusted until the
// checks pass.
struct SdlPass {
    SDL_GPUCommandBuffer* command_buffer;
    bool in_progress;
};
struct SdlComputePass {
    SDL_GPUCommandBuffer* command_buffer;
    bool in_progress;
    SDL_GPUComputePipeline* compute_pipeline;
    // MAX_TEXTURE_SAMPLERS_PER_STAGE, MAX_STORAGE_TEXTURES_PER_STAGE,
    // MAX_STORAGE_BUFFERS_PER_STAGE, MAX_COMPUTE_WRITE_TEXTURES and _BUFFERS
    bool sampler_bound[16];
    bool read_only_storage_texture_bound[8];
    bool read_only_storage_buffer_bound[8];
    bool read_write_storage_texture_bound[8];
    bool read_write_storage_buffer_bound[8];
};
struct SdlRenderPass {
    SDL_GPUCommandBuffer* command_buffer;
    bool in_progress;
    SDL_GPUTexture* color_targets[8];  // MAX_COLOR_TARGET_BINDINGS
    Uint32 num_color_targets;
    SDL_GPUTexture* depth_stencil_target;
    SDL_GPUGraphicsPipeline* graphics_pipeline;
    bool vertex_sampler_bound[16];
    bool vertex_storage_texture_bound[8];
    bool vertex_storage_buffer_bound[8];
    bool fragment_sampler_bound[16];
    bool fragment_storage_texture_bound[8];
    bool fragment_storage_buffer_bound[8];
};
struct SdlCommandBufferCommonHeader {
    SDL_GPUDevice* device;
    SdlRenderPass render_pass;
    SdlComputePass compute_pass;
    SdlPass copy_pass;
    bool swapchain_texture_acquired;
    bool submitted;
    bool ignore_render_pass_texture_validation;
};
struct SdlD3D12CommandBuffer {
    SdlCommandBufferCommonHeader common;
    void* renderer;  // D3D12Renderer*, whose layout has #ifdefs: never walked
    ID3D12CommandAllocator* command_allocator;
    ID3D12GraphicsCommandList* graphics_command_list;
    // the in-flight fence and the rest follow
};

// COM identity: the IUnknown pointer is the same however it's reached
IUnknown* Identity(IUnknown* object) {
    IUnknown* unknown = nullptr;
    if (!object || FAILED(object->QueryInterface(IID_PPV_ARGS(&unknown)))) return nullptr;
    unknown->Release();  // the object holds it still
    return unknown;
}
#endif

}  // namespace

void* GpuRenderer::Impl::SdkResource(SDL_GPUTexture* texture, const SDL_GPUTextureCreateInfo& ti,
                                     std::string& why) {
#ifdef _WIN32
    if (!present_device) {
        why = "the SDK's presenter isn't Direct3D 12";
        return nullptr;
    }
    if (std::strcmp(SDL_GetGPUDeviceDriver(device), "direct3d12") != 0) {
        why = std::string("SDL_gpu's device is ") + SDL_GetGPUDeviceDriver(device) +
              ", not Direct3D 12";
        return nullptr;
    }
    // the N2 kill test's checks (out/research/n2_design.md) on SDL 3.4.14's
    // layout; a newer SDL that moved anything fails one before any call
    const auto* c = reinterpret_cast<const SdlD3D12TextureContainer*>(texture);
    // (a) the create info, but props: SDL gives the container its own
    if (c->info.type != ti.type || c->info.format != ti.format || c->info.usage != ti.usage ||
        c->info.width != ti.width || c->info.height != ti.height ||
        c->info.layer_count_or_depth != ti.layer_count_or_depth ||
        c->info.num_levels != ti.num_levels || c->info.sample_count != ti.sample_count) {
        why = fmt::format("SDL {}.{}.{}'s texture container doesn't start with its create info",
                          SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);
        return nullptr;
    }
    // (b) its texture points back at it
    const SdlD3D12Texture* t = c->active_texture;
    if (!t || t->container != c) {
        why = "SDL's active texture doesn't point back at its container";
        return nullptr;
    }
    // (c) the one texture the container has
    if (t->container_index != 0 || c->texture_count != 1 || !c->textures ||
        c->textures[0] != t || !t->resource) {
        why = fmt::format("SDL's texture container has {} textures, or its first isn't the "
                          "active one",
                          c->texture_count);
        return nullptr;
    }
    // (d) on the SDK's device, the same object however reached
    ID3D12Resource* resource = t->resource;
    ID3D12Device* sdl_device = nullptr;
    if (FAILED(resource->GetDevice(IID_PPV_ARGS(&sdl_device))) || !sdl_device) {
        why = "SDL's resource has no device";
        return nullptr;
    }
    const bool same = Identity(sdl_device) &&
                      Identity(sdl_device) == Identity(static_cast<ID3D12Device*>(present_device));
    sdl_device->Release();
    if (!same) {
        why = "SDL_gpu's Direct3D 12 device isn't the SDK's (another adapter?)";
        return nullptr;
    }
    // (e) the texture asked for
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.Width != ti.width ||
        desc.Height != ti.height || desc.DepthOrArraySize != 1 || desc.MipLevels != 1) {
        why = fmt::format("SDL's resource is format {} {}x{}, not R8G8B8A8_UNORM {}x{}",
                          int(desc.Format), uint64_t(desc.Width), desc.Height, ti.width,
                          ti.height);
        return nullptr;
    }
    return resource;
#else
    (void)texture;
    (void)ti;
    why = "zero-copy presentation is Direct3D 12 only";
    return nullptr;
#endif
}

void GpuRenderer::Impl::SetZeroCopy(bool ok, const std::string& why) {
    std::lock_guard lock(zero_copy_mutex);
    zero_copy_why = ok ? std::string() : why;
    zero_copy = ok;
}

void GpuRenderer::Impl::CheckZeroCopyOnce() {
    if (zero_copy_checked) return;
    zero_copy_checked = true;
    const SDL_GPUTextureCreateInfo ti = OutputInfo(16, 16);
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device, &ti);
    if (!texture) {
        SetZeroCopy(false, std::string("no test texture (") + SDL_GetError() + ")");
        return;
    }
    std::string why;
    const bool ok = SdkResource(texture, ti, why) != nullptr;
    SDL_ReleaseGPUTexture(device, texture);
    SetZeroCopy(ok, why);
}

void* GpuRenderer::Impl::TimingList(SDL_GPUCommandBuffer* cmd) {
#ifdef _WIN32
    if (!cmd) return nullptr;
    const auto* c = reinterpret_cast<const SdlD3D12CommandBuffer*>(cmd);
    // (a) SDL_AcquireGPUCommandBuffer sets the header's device and each
    // pass's command buffer every time; (b) the fields after it are set.
    // Pointers compared only, nothing called.
    if (c->common.device != device || c->common.render_pass.command_buffer != cmd ||
        c->common.compute_pass.command_buffer != cmd || c->common.copy_pass.command_buffer != cmd)
        return nullptr;
    if (!c->renderer || !c->command_allocator || !c->graphics_command_list) return nullptr;
    return c->graphics_command_list;
#else
    (void)cmd;
    return nullptr;
#endif
}

bool GpuRenderer::Impl::CheckTimingOnce(SDL_GPUCommandBuffer* cmd) {
    if (timing_checked) return timing_ok && TimingList(cmd);
    timing_checked = true;
    std::string why;
#ifdef _WIN32
    auto check = [&]() -> bool {
        if (!present_device) {
            why = "the SDK's presenter isn't Direct3D 12";
            return false;
        }
        if (std::strcmp(SDL_GetGPUDeviceDriver(device), "direct3d12") != 0) {
            why = std::string("SDL_gpu's device is ") + SDL_GetGPUDeviceDriver(device);
            return false;
        }
        if (!timestamp_frequency) {
            why = "the SDK's direct queue has no timestamp frequency";
            return false;
        }
        // a newer SDL that moved anything fails here before any call
        auto* list = static_cast<ID3D12GraphicsCommandList*>(TimingList(cmd));
        if (!list) {
            why = fmt::format("SDL {}.{}.{}'s command buffer doesn't have the layout of 3.4.14's",
                              SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_MICRO_VERSION);
            return false;
        }
        const auto* c = reinterpret_cast<const SdlD3D12CommandBuffer*>(cmd);
        // (c) a graphics command list, direct, on the SDK's device, and a
        // command allocator, however reached
        ID3D12GraphicsCommandList* as_list = nullptr;
        if (FAILED(list->QueryInterface(IID_PPV_ARGS(&as_list))) || !as_list) {
            why = "SDL's command list isn't an ID3D12GraphicsCommandList";
            return false;
        }
        const bool direct = as_list->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12Device* list_device = nullptr;
        const bool has_device = SUCCEEDED(as_list->GetDevice(IID_PPV_ARGS(&list_device))) &&
                                list_device;
        as_list->Release();
        const bool same = has_device && Identity(list_device) &&
                          Identity(list_device) ==
                              Identity(static_cast<ID3D12Device*>(present_device));
        if (list_device) list_device->Release();
        if (!direct || !same) {
            why = !direct ? "SDL's command list isn't a direct one"
                          : "SDL's command list isn't on the SDK's device";
            return false;
        }
        ID3D12CommandAllocator* allocator = nullptr;
        if (FAILED(c->command_allocator->QueryInterface(IID_PPV_ARGS(&allocator))) ||
            !allocator) {
            why = "SDL's command allocator isn't an ID3D12CommandAllocator";
            return false;
        }
        allocator->Release();
        // a heap region per frame in flight plus the aside's; readback, two
        // regions per frame
        auto* d3d = static_cast<ID3D12Device*>(present_device);
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = (kAsideRegion + 1) * kTimingSlots;
        if (FAILED(d3d->CreateQueryHeap(&qd, IID_PPV_ARGS(&query_heap)))) {
            query_heap = nullptr;
            why = "no timestamp query heap";
            return false;
        }
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = uint64_t(kFrameRegions) * 2 * kTimingSlots * sizeof(uint64_t);
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(d3d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&query_readback)))) {
            query_readback = nullptr;
            why = "no readback buffer for the timestamps";
            return false;
        }
        return true;
    };
    timing_ok = check();
    if (!timing_ok) ReleaseTiming();
#else
    why = "they're Direct3D 12 only";
#endif
    if (timing_ok)
        REXLOG_INFO("native view gpu: GPU timings on ({} timestamp ticks a second)",
                    timestamp_frequency);
    else
        REXLOG_INFO("native view gpu: GPU timings off: {}", why);
    return timing_ok;
}

void GpuRenderer::Impl::StartTiming(SDL_GPUCommandBuffer* cmd, const RasterOptions& o, int slot,
                                    int pre_pass, bool ahead_pass) {
    timing_region = -1;
    const bool aside = pre_pass || ahead_pass;
    if (!o.gpu_timestamps || !CheckTimingOnce(cmd)) {
        // so a later frame isn't charged the aside's
        if (!aside) aside_ladder.Reset();
        return;
    }
    timing_aside = aside;
    timing_region = aside ? kAsideRegion : slot >= 0 ? slot : kOutputs;
    if (!aside) {
        ladder.Reset();
        timed[timing_region].pending = false;
    }
    MarkTime(cmd, ahead_pass ? gpu_timing::kWorldAhead
                  : pre_pass ? gpu_timing::kPreWorld
                             : gpu_timing::kUpload);
}

void GpuRenderer::Impl::MarkTime(SDL_GPUCommandBuffer* cmd, uint8_t part) {
#ifdef _WIN32
    if (timing_region < 0) return;
    auto* list = static_cast<ID3D12GraphicsCommandList*>(TimingList(cmd));
    if (!list) {
        // fails the checks: untimed, and the aside's ladder starts over
        if (timing_aside) aside_ladder.Reset();
        timing_region = -1;
        return;
    }
    gpu_timing::Ladder& l = timing_aside ? aside_ladder : ladder;
    // the aside's are one part each, from start to end
    if (timing_aside && part != gpu_timing::kNone && l.Open()) return;
    const int at = l.Mark(part);
    if (at < 0) return;
    list->EndQuery(query_heap, D3D12_QUERY_TYPE_TIMESTAMP,
                   uint32_t(timing_region) * kTimingSlots + uint32_t(at));
#else
    (void)cmd;
    (void)part;
#endif
}

void GpuRenderer::Impl::ResolveTimes(SDL_GPUCommandBuffer* cmd) {
#ifdef _WIN32
    if (timing_region < 0 || timing_aside) return;
    MarkTime(cmd, gpu_timing::kNone);
    if (timing_region < 0) return;
    auto* list = static_cast<ID3D12GraphicsCommandList*>(TimingList(cmd));
    const int r = timing_region;
    const uint64_t at = uint64_t(r) * 2 * kTimingSlots * sizeof(uint64_t);
    list->ResolveQueryData(query_heap, D3D12_QUERY_TYPE_TIMESTAMP, uint32_t(r) * kTimingSlots,
                           ladder.Count(), query_readback, at);
    Timed& t = timed[r];
    t.labels = ladder.Labels();
    t.dropped = ladder.Dropped();
    t.aside.clear();
    // the aside's, if ended (one left open was cut short: not counted)
    if (aside_ladder.Count() && !aside_ladder.Open()) {
        list->ResolveQueryData(query_heap, D3D12_QUERY_TYPE_TIMESTAMP,
                               uint32_t(kAsideRegion) * kTimingSlots, aside_ladder.Count(),
                               query_readback, at + kTimingSlots * sizeof(uint64_t));
        t.aside = aside_ladder.Labels();
        t.dropped += aside_ladder.Dropped();
    }
    aside_ladder.Reset();
    t.pending = true;
    timing_region = -1;
#else
    (void)cmd;
#endif
}

void GpuRenderer::Impl::ReadTimes(int region, GpuStats& st) {
#ifdef _WIN32
    if (region < 0 || region >= kFrameRegions || !query_readback) return;
    Timed& t = timed[region];
    if (!t.pending) return;
    t.pending = false;
    const size_t at = size_t(region) * 2 * kTimingSlots * sizeof(uint64_t);
    const D3D12_RANGE range{at, at + 2 * kTimingSlots * sizeof(uint64_t)};
    void* data = nullptr;
    // fails if the device was removed
    if (FAILED(query_readback->Map(0, &range, &data)) || !data) return;
    const auto* ticks = reinterpret_cast<const uint64_t*>(static_cast<const char*>(data) + at);
    gpu_timing::Times times;
    gpu_timing::Accumulate(t.labels.data(), ticks, t.labels.size(), timestamp_frequency, times);
    gpu_timing::Accumulate(t.aside.data(), ticks + kTimingSlots, t.aside.size(),
                           timestamp_frequency, times);
    const D3D12_RANGE none{0, 0};
    query_readback->Unmap(0, &none);
    st.gpu_timed = true;
    std::copy(std::begin(times.ms), std::end(times.ms), st.gpu_ms);
    st.gpu_total_ms = times.total_ms;
    st.gpu_marks_dropped = t.dropped;
    st.gpu_bad_spans = times.bad;
#else
    (void)region;
    (void)st;
#endif
}

void GpuRenderer::Impl::ReleaseTiming() {
#ifdef _WIN32
    if (query_heap) query_heap->Release();
    if (query_readback) query_readback->Release();
    query_heap = nullptr;
    query_readback = nullptr;
#endif
    timing_ok = false;
    timing_region = -1;
    ladder.Reset();
    aside_ladder.Reset();
    for (Timed& t : timed) t = Timed{};
}

bool GpuRenderer::Impl::EnsureOutput(int slot, uint32_t w, uint32_t h) {
    Output& out = outputs[slot];
    if (out.texture && out.w == w && out.h == h) return true;
    // SDL frees it after its own work; the presenter holds the Direct3D 12
    // texture as long as it needs it (native_view.cpp redraws a slot only
    // once it's done)
    if (out.texture) SDL_ReleaseGPUTexture(device, out.texture);
    ReleaseFence(out);
    out = Output{};
    const SDL_GPUTextureCreateInfo ti = OutputInfo(w, h);
    out.texture = SDL_CreateGPUTexture(device, &ti);
    counts.textures++;
    if (!out.texture) {
        REXLOG_WARN("native view gpu: no {}x{} output ({})", w, h, SDL_GetError());
        return false;
    }
    out.w = w;
    out.h = h;
    out.generation = ++output_generations;
    // each checked as the test texture was
    if (zero_copy) {
        std::string why;
        out.resource = SdkResource(out.texture, ti, why);
        if (!out.resource) SetZeroCopy(false, "an output failed the checks: " + why);
    }
    return true;
}

void GpuRenderer::Impl::ReleaseFence(Output& out) {
    if (device && out.fence) SDL_ReleaseGPUFence(device, out.fence);
    out.fence = nullptr;
}

void GpuRenderer::Impl::ReleaseOutputs() {
    if (device) {
        for (Output& out : outputs) {
            ReleaseFence(out);
            if (out.texture) SDL_ReleaseGPUTexture(device, out.texture);
        }
        if (output_readback) SDL_ReleaseGPUTransferBuffer(device, output_readback);
    }
    for (Output& out : outputs) out = Output{};
    output_readback = nullptr;
    output_readback_size = 0;
}

bool GpuRenderer::Impl::Download(SDL_GPUTexture* texture, uint32_t w, uint32_t h,
                                 std::vector<uint32_t>& rgba) {
    const uint32_t bytes = w * h * 4;
    if (!output_readback || output_readback_size < bytes) {
        if (output_readback) SDL_ReleaseGPUTransferBuffer(device, output_readback);
        SDL_GPUTransferBufferCreateInfo tbi{};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        tbi.size = bytes;
        output_readback = SDL_CreateGPUTransferBuffer(device, &tbi);
        output_readback_size = output_readback ? bytes : 0;
        if (!output_readback) {
            REXLOG_WARN("native view gpu: no buffer to read an output back ({})", SDL_GetError());
            return false;
        }
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    if (!cmd) {
        REXLOG_WARN("native view gpu: no command buffer ({})", SDL_GetError());
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{};
    src.texture = texture;
    src.w = w;
    src.h = h;
    src.d = 1;
    SDL_GPUTextureTransferInfo dst{output_readback, 0, w, h};
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    const bool done = fence && SDL_WaitForGPUFences(device, true, &fence, 1);
    if (fence) SDL_ReleaseGPUFence(device, fence);
    const auto* px =
        done ? static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(device, output_readback, false))
             : nullptr;
    if (!px) {
        REXLOG_WARN("native view gpu: couldn't read an output back ({})", SDL_GetError());
        return false;
    }
    rgba.resize(size_t(w) * h);
    std::memcpy(rgba.data(), px, rgba.size() * sizeof(uint32_t));
    SDL_UnmapGPUTransferBuffer(device, output_readback);
    return true;
}

bool GpuRenderer::Impl::Render(const FrameCapture& frame, const RasterOptions& o, int slot,
                               std::vector<uint32_t>* rgba, GpuStats& st, int pre_pass,
                               bool ahead_pass) {
    const Ahead ahead_was = std::exchange(ahead, Ahead{});
    // for GpuStats::plan_ms and the rest
    using Clock = std::chrono::steady_clock;
    const auto render_start = Clock::now();
    auto ms_since = [](Clock::time_point t) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    };
    if (!o.width || !o.height || !EnsureTargets(o.width, o.height)) return false;
    if (slot >= 0 && !EnsureOutput(slot, o.width, o.height)) return false;
    keep_frames = ResidencyKeepFrames(o.world_period);
    // first-placed textures kept as blocks or k_8 bytes (UseTexture)
    bc_now = bc_formats && o.bc_textures;
    r8_now = r8_format && o.r8_textures;
    // where the last pass, the gamma ramp's, puts the finished frame
    SDL_GPUTexture* const output = slot >= 0 ? outputs[slot].texture : graded;
    // RasterOptions::post_buffer: frames that post-process nothing show the
    // kept picture instead of drawing their world
    const bool kept_buffer = o.post_buffer && o.view == RasterView::kFinal;
    const bool shows_kept = kept_buffer && ShowsPostBuffer(frame) && post_buffer.tex &&
                            post_buffer.w == width && post_buffer.h == height &&
                            PostBufferFor(frame, post_buffer.game_frame);
    const bool keeps = kept_buffer && ProcKnown(frame) && (frame.proc_cmds & kProcPost) &&
                       EnsureKept(post_buffer, width, height);
    st.shows_kept = shows_kept;
    // RasterOptions::world_ahead: the previous Render (serial is still its)
    // drew this composed post frame's world into the scene target, so draws
    // [0, composed_world_end) are skipped and it goes on from the resolve
    const bool uses_ahead = !ahead_pass && !pre_pass && o.world_ahead && frame.composed &&
                            frame.composed_world_end && !shows_kept &&
                            o.view == RasterView::kFinal && ahead_was.serial &&
                            ahead_was.serial == serial &&
                            ahead_was.world_frame == frame.world_frame &&
                            ahead_was.w == width && ahead_was.h == height;
    const uint32_t ahead_end = uses_ahead ? frame.composed_world_end : 0;
    st.ahead_used = uses_ahead ? 1 : 0;
    // The world's REFRACT_WORLD draws (RefractsWorld) read the pre-process
    // buffer (RasterOptions::pre_buffer) kept from earlier world frames, or
    // else the world drawn kPreBufferPasses times first, each its own Render,
    // as Rasterize() does; black (no_depth) where neither. This frame's world
    // is kept in turn.
    const bool world_refracts = !shows_kept && !uses_ahead && WorldRefracts(frame);
    const bool keeps_pre =
        world_refracts && !pre_pass && o.pre_buffer && o.view == RasterView::kFinal;
    SDL_GPUTexture* world_behind = no_depth;
    if (world_refracts) {
        if (pre_pass) {
            if (pre_pass > 1) world_behind = pre_scratch.texture;
        } else if (keeps_pre && pre_buffer.tex && pre_buffer.w == width &&
                   pre_buffer.h == height && PreBufferFor(frame, pre_buffer.game_frame)) {
            world_behind = pre_buffer.tex;
        } else if (o.view == RasterView::kFinal && EnsureScratch(pre_scratch, width, height)) {
            // the world alone: no post-processing, history or MSAA
            RasterOptions po = o;
            po.post = po.trails = po.post_buffer = po.pre_buffer = false;
            po.msaa = 1;
            const auto pre_start = Clock::now();
            for (int k = 1; k <= kPreBufferPasses; k++) {
                GpuStats ps;
                if (!Render(frame, po, -1, nullptr, ps, k)) return false;
                // the first uploads the world's data, which this frame reuses
                st.pre_passes++;
                st.pre_plan_ms += ps.plan_ms;
                st.pre_walk_ms += ps.plan_walk_ms;
                st.pre_targets_made += ps.targets_made;
                st.pre_targets_ms += ps.targets_ms;
                st.premade_used += ps.premade_used;
                st.arrays_premade_used += ps.arrays_premade_used;
                st.pre_arrays_grown += ps.arrays_grown;
                st.pre_arrays_ms += ps.arrays_ms;
                st.pre_arrays_mb += ps.arrays_mb;
                st.pre_upload_ms += ps.upload_ms;
                st.pre_record_ms += ps.record_ms;
                st.pre_acquire_ms += ps.acquire_ms;
                st.pre_first_draw_ms += ps.first_draw_ms;
                st.pre_uniform_slow += ps.uniform_slow;
                st.pre_uniform_slow_ms += ps.uniform_slow_ms;
                st.pre_draw_slow_ms += ps.draw_slow_ms;
                st.pre_submit_ms += ps.submit_ms;
                st.pre_submits += ps.submits;
                st.pre_wait_ms += ps.wait_ms;
                st.pre_evict_ms += ps.evict_ms;
                st.pool_meshes += ps.pool_meshes;
                st.arena_moved += ps.arena_moved;
                st.arena_sent += ps.arena_sent;
                st.arena_copied += ps.arena_copied;
                st.mesh_bytes += ps.mesh_bytes;
                st.textures_sent += ps.textures_sent;
                st.texture_bytes += ps.texture_bytes;
            }
            st.pre_ms = ms_since(pre_start);
            world_behind = pre_scratch.texture;
        }
    }
    const bool keeps_pre_now = keeps_pre && EnsureKept(pre_buffer, width, height);
    const auto walk_start = Clock::now();
    st.plan_setup_ms =
        std::chrono::duration<double, std::milli>(walk_start - render_start).count() - st.pre_ms;
    walk_stats = &st;
    serial++;
    frame_now = walk_start;
    frame_world = frame.world_frame;
    frame_in_song = o.clock_keep;
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
    shades.resize(frame.draws.size());
    diffuse_source.assign(frame.draws.size(), kSourceNone);
    proj_source.assign(frame.draws.size(), kSourceNone);
    for (auto& v : normal_source) v.assign(frame.draws.size(), kSourceNone);
    spot_draw.assign(frame.draws.size(), kSpotNone);
    const std::vector<PassRun> runs = PlanPasses(frame, o);
    run_clear.assign(runs.size(), 0);
    uint32_t pool_vert_count = 0, pool_index_count = 0;
    // in the arena already, into it from the last frame's pool, or into this
    // frame's pool
    auto use_mesh = [&](const std::shared_ptr<const Geometry>& geom) {
        Mesh& m = meshes[geom.get()];
        if (!m.keep) {
            m.keep = geom;
            m.first = serial;
            m.first_world = frame_world;
            st.meshes_first++;
        }
        if (m.used == serial) return;
        m.used = serial;
        m.used_at = frame_now;
        m.drawn_in_song = frame_in_song;
        if (frame_world != m.first_world) m.across_worlds = true;
        if (m.in_arena) {
            // already there
        } else if (m.first != serial) {
            // drawn again: to the arena, from the last frame's pool if that
            // frame drew it, else sent again (its pool was overwritten)
            m.arena_vertex = ~0u;
            if (MeshFromLastPool(m.first, serial)) {
                m.pool_vertex = m.first_vertex;
                m.pool_index = m.first_index;
            } else {
                m.pool_vertex = ~0u;
            }
            to_arena.push_back(&m);
        } else {
            m.first_vertex = pool_vert_count;
            m.first_index = pool_index_count;
            pool_vert_count += uint32_t(geom->verts.size());
            pool_index_count += IndexSlots(*geom);
            to_pool.push_back(&m);
        }
    };
    for (size_t r = 0; r < runs.size(); r++) {
        const PassRun& run = runs[r];
        // a texture pass's target, cleared where its camera (or NgLight)
        // cleared it and where this frame hasn't drawn it yet (transparent
        // black, as on the CPU), so a frame never draws over an earlier
        // frame's (e.g. depth volume blurs would blur the last shot's beams).
        // One sampled but not drawn still reads what was drawn last.
        Rt* target = nullptr;
        // The world ahead draws the world's texture passes, not the
        // overlay's. A post frame using it skips those whose targets it drew
        // and draws any other itself (counted).
        if (run.pass && ahead_pass && run.first >= frame.post_boundary) continue;
        if (run.pass && run.first < ahead_end) {
            const auto f = rts.find(run.pass->tex_obj);
            if (f != rts.end() && f->second.drawn && f->second.drawn_in == ahead_was.serial)
                continue;
            st.ahead_fallback_passes++;
        }
        if (run.pass) {
            uint32_t tw, th;
            PassTargetSize(frame, *run.pass, o, tw, th);
            target = TargetFor(*run.pass, tw, th, o.premake_targets);
            if (!target) continue;
            const bool fresh = !target->drawn || target->drawn_in != serial;
            const uint32_t clear = PassClearFlags(frame, *run.pass);
            run_clear[r] = kRunDrawn;
            if ((clear & 0x0f) || fresh) run_clear[r] |= kRunClearColor;
            if ((clear & 0x30) || fresh) run_clear[r] |= kRunClearDepth;
            st.passes++;
        }
        for (size_t d = run.first; d < run.end; d++) {
            const DrawItem& it = frame.draws[d];
            if (!DrawnIn(run, frame, it, o)) continue;
            if (!run.pass && shows_kept && d < frame.post_boundary) continue;
            if (!run.pass && (d < ahead_end || (ahead_pass && d >= frame.post_boundary))) continue;
            if (!run.pass && d < frame.post_boundary) st.world_draws++;
            const ShadeState* state = shade::ShadeOf(frame, it);
            // depth volume blurs read a copy of it, not the quad's texture;
            // cones need the scene's depth or are skipped
            if (run.pass && spot::SpotBlur(it, state, *run.pass)) {
                spot_draw[d] = kSpotBlur;
                continue;
            }
            if (run.pass && IsSpotCone(it, state)) {
                spot_draw[d] = depth_sampled ? kSpotCone : kSpotConeSkipped;
                if (!depth_sampled) continue;
            }
            // soft-particle blurs read a copy of the other surface's target
            if (run.pass && SoftBlur(frame, it, state, *run.pass)) {
                spot_draw[d] = kSoftBlur;
                if (auto f = rts.find(it.tex->tex_obj); f != rts.end()) UseRt(f->second);
                continue;
            }
            if (run.pass && IsSoftParticle(it, state)) spot_draw[d] = kSoftParticle;
            use_mesh(it.geom);
            if (Skinned(it, o)) {
                bone_base[d] = uint32_t(frame_bones.size());
                frame_bones.insert(frame_bones.end(), it.bones.begin(), it.bones.end());
            }
            // as soft_raster.cpp's Diffuse(): a render target is its pass's
            // target if drawn (this frame or earlier), else guest pixels if
            // kept and wanted, else transparent black; never itself or a
            // shadow map
            uint8_t& source = diffuse_source[d];
            if (o.textures && it.tex && SamplesDiffuse(it)) {
                const Texture& tex = *it.tex;
                if (o.texture_passes && IsPassTarget(&tex)) {
                    auto f = rts.find(tex.tex_obj);
                    const bool self = run.pass && run.pass->tex_obj == tex.tex_obj;
                    if (!self && f != rts.end() && f->second.drawn && !f->second.shadow) {
                        source = kSourceRt;
                        UseRt(f->second);
                    } else if (!self && o.rt_guest_pixels && !tex.rgba.empty()) {
                        source = kSourceTexture;
                    } else {
                        source = kSourceBlack;
                        st.rt_missing++;
                    }
                } else if (!tex.tex_obj || o.rt_guest_pixels) {
                    source = kSourceTexture;
                }
                if (source == kSourceTexture) UseTexture(it.tex);
            }
            // textured or not is settled at draw time, once the texture has
            // its layer
            shade::PackShade(it, state, o, false, shades[d]);
            uint32_t& flags = shades[d].flags.x;
            if (flags & shade::kShadeSpecMap) UseTexture(state->maps[kMapSpecular]);
            if (flags & shade::kShadeGlow) UseTexture(state->maps[kMapGlow]);
            // as soft_raster.cpp's NormalMap(): one RB3 draws (a head's) is
            // its pass's target if drawn, else guest pixels if kept and
            // wanted, else left out (counted). REFRACT_WORLD's refract normal
            // map is s1 too, in the normal map's slot.
            constexpr uint32_t kNormalSlotBits =
                shade::kShadeNormalMap | shade::kShadeRefractMap;
            for (int k = 0; k < 2; k++) {
                const uint32_t bit = k ? shade::kShadeDetailMap : kNormalSlotBits;
                if (!(flags & bit)) continue;
                const int m = k ? kMapDetailNormal : kMapNormal;
                const Texture* map = MapTargetOf(state, m);
                uint8_t& source = normal_source[k][d];
                if (!map) {
                    source = kSourceTexture;
                    UseTexture(state->maps[m]);
                    continue;
                }
                auto f = rts.find(map->tex_obj);
                const bool self = run.pass && run.pass->tex_obj == map->tex_obj;
                if (o.texture_passes && !self && f != rts.end() && f->second.drawn &&
                    !f->second.shadow) {
                    source = kSourceRt;
                    UseRt(f->second);
                } else if (o.rt_guest_pixels && !map->rgba.empty()) {
                    source = kSourceTexture;
                    UseTexture(state->maps[m]);
                } else {
                    flags &= k ? ~shade::kShadeDetailMap
                               : ~(kNormalSlotBits | shade::kShadeDetailMap);
                    if (o.texture_passes) st.rt_missing++;
                    if (!k) break;
                }
            }
            // the projected light's s5, as soft_raster.cpp's Projected(): one
            // RB3 draws (NgLight's shadow) is its target if this frame drew
            // the version read, else guest pixels if kept and wanted, else
            // the light is left out (counted)
            if (flags & (shade::kShadeProjMultiply | shade::kShadeProjGobo)) {
                const Texture* map = ProjectedTargetOf(state);
                if (map && o.texture_passes) {
                    auto f = rts.find(map->tex_obj);
                    const bool self = run.pass && run.pass->tex_obj == map->tex_obj;
                    if (!self && f != rts.end() && f->second.drawn_in == serial &&
                        f->second.version == map->version && !f->second.shadow) {
                        proj_source[d] = kSourceRt;
                        UseRt(f->second);
                    } else if (o.rt_guest_pixels && !map->rgba.empty()) {
                        UseTexture(state->maps[kMapProjected]);
                    } else {
                        flags &= ~(shade::kShadeProjMultiply | shade::kShadeProjGobo);
                        st.rt_missing++;
                    }
                } else if (!map || o.rt_guest_pixels) {
                    UseTexture(state->maps[kMapProjected]);
                }
            }
            if (flags & shade::kShadeProjGobo) UseTexture(state->maps[kMapGobo]);
            // the shadow map if this frame drew the version read
            // (soft_raster.cpp's DrawOne); else lit
            if (flags & shade::kShadeShadow) {
                const Texture* map = ShadowMapOf(state);
                auto f = map ? rts.find(map->tex_obj) : rts.end();
                const bool self = run.pass && map && run.pass->tex_obj == map->tex_obj;
                if (!self && f != rts.end() && f->second.shadow &&
                    f->second.drawn_in == serial && f->second.version == map->version)
                    UseRt(f->second);
                else
                    flags &= ~shade::kShadeShadow;
            }
        }
        if (target) {
            target->drawn = true;
            target->drawn_in = serial;
            target->version = run.pass->version;
        }
    }
    // the composite's noise map
    if (o.post && o.view == RasterView::kFinal && frame.noise_map) UseTexture(frame.noise_map);
    // the motion blur object pass's meshes, and its palettes as bones (each
    // entry's three rows, as the shader reads them)
    velocity_bone_base.assign(frame.velocity_objects.size(), 0);
    if (o.post && o.velocity && o.view == RasterView::kFinal && depth_sampled) {
        for (size_t i = 0; i < frame.velocity_objects.size(); i++) {
            const VelocityObject& v = frame.velocity_objects[i];
            if (!v.geom || v.geom->indices.empty() || v.rows.size() != size_t(v.bones) * 24)
                continue;
            use_mesh(v.geom);
            velocity_bone_base[i] = uint32_t(frame_bones.size());
            for (uint32_t e = 0; e < v.bones * 2; e++) {
                Mat4 m{};
                std::memcpy(m.m, &v.rows[size_t(e) * 12], 12 * sizeof(float));
                frame_bones.push_back(m);
            }
        }
    }
    walk_stats = nullptr;
    const auto arena_start = Clock::now();
    st.plan_walk_ms = std::chrono::duration<double, std::milli>(arena_start - walk_start).count();
    const uint64_t rebuilds = counts.arena_rebuilds;
    // A failure from here leaves meshes marked in_arena and textures placed
    // but never sent. Harmless: a failed Render fails Draw, which gives up
    // for the session and releases everything.
    if (!PlaceInArena()) return false;
    if (counts.arena_rebuilds != rebuilds)
        st.arena_new_mb = double(arena_verts.size + arena_indices.size) / 1048576;
    st.plan_arena_ms = ms_since(arena_start);

    // the upload: the pool's vertices and indices, arena meshes from the CPU,
    // the bones, then the textures, each aligned for its copy
    const uint32_t pool_index_at = Align(pool_vert_count * uint32_t(sizeof(Vertex)), 16);
    uint32_t at = Align(pool_index_at + pool_index_count * 2, 16);
    const uint32_t arena_at = at;
    for (const Mesh* m : to_arena) {
        if (!m->FromCpu()) continue;
        at = Align(at + uint32_t(m->keep->verts.size() * sizeof(Vertex)), 16);
        at = Align(at + IndexSlots(*m->keep) * 2, 16);
    }
    const uint32_t bones_at = at;
    const uint32_t bone_bytes = uint32_t(frame_bones.size() * sizeof(Mat4));
    const uint32_t textures_at = Align(bones_at + bone_bytes, kTextureOffsetAlign);
    uint32_t upload_bytes = textures_at;
    // A new texture's level l: rows of texels (RGBA8, or R8 for k_8) or
    // blocks, `pitch` apart, `bytes` in all; into its layer's level's `w` x `h`
    // corner, whole blocks for BC (BcExtent; SDL takes pixels_per_row and
    // rows_per_layer in texels, multiples of 4)
    struct LevelUpload {
        const uint8_t* src;
        uint32_t row_bytes, rows, pitch, bytes;
        uint32_t w, h, pixels_per_row, rows_per_layer;
    };
    auto level_upload = [&](const Tex& tx, uint32_t l) {
        const Texture& t = *tx.keep;
        const uint32_t w = std::max(t.width >> l, 1u), h = std::max(t.height >> l, 1u);
        LevelUpload u;
        if (IsBc(tx.array->format)) {
            const uint32_t bpb = BlockBytes(tx.array->format);
            u.src = (l ? t.blocks->mips[l - 1] : t.blocks->level0).data();
            u.row_bytes = (w + 3) / 4 * bpb;
            u.rows = (h + 3) / 4;
            u.pitch = Align(u.row_bytes, kRowPitchAlign);
            u.w = BcExtent(w, std::max(tx.array->w >> l, 1u));
            u.h = BcExtent(h, std::max(tx.array->h >> l, 1u));
            u.pixels_per_row = u.pitch / bpb * 4;
            u.rows_per_layer = u.rows * 4;
        } else if (tx.array->format == SDL_GPU_TEXTUREFORMAT_R8_UNORM) {
            // k_8's bytes, a texel each
            u.src = (l ? t.blocks->mips[l - 1] : t.blocks->level0).data();
            u.row_bytes = w;
            u.rows = h;
            u.pitch = Align(w, kRowPitchAlign);
            u.w = w;
            u.h = h;
            u.pixels_per_row = u.pitch;
            u.rows_per_layer = h;
        } else {
            u.src = reinterpret_cast<const uint8_t*>((l ? t.mips[l - 1] : t.rgba).data());
            u.row_bytes = w * 4;
            u.rows = h;
            u.pitch = Align(w * 4, kRowPitchAlign);
            u.w = w;
            u.h = h;
            u.pixels_per_row = u.pitch / 4;
            u.rows_per_layer = h;
        }
        u.bytes = Align(u.pitch * u.rows, kTextureOffsetAlign);
        return u;
    };
    for (const Tex* tx : new_textures)
        for (uint32_t l = 0; l < tx->levels; l++) upload_bytes += level_upload(*tx, l).bytes;
    // what comes from the CPU (GpuStats::mesh_bytes and the rest)
    st.pool_meshes += uint32_t(to_pool.size());
    st.mesh_bytes += uint64_t(pool_vert_count) * sizeof(Vertex) + uint64_t(pool_index_count) * 2;
    uint32_t arena_copied = 0;
    for (const Mesh* m : to_arena) {
        if (m->arena_vertex != ~0u) {
            arena_copied++;
            continue;
        }
        if (m->pool_vertex != ~0u) {
            st.arena_moved++;
            continue;
        }
        st.arena_sent++;
        st.mesh_bytes += m->keep->verts.size() * sizeof(Vertex) + uint64_t(IndexSlots(*m->keep)) * 2;
    }
    st.arena_copied += arena_copied;
    st.textures_sent += uint32_t(new_textures.size());
    st.texture_bytes += upload_bytes - textures_at;
    st.bone_bytes += bone_bytes;
    // Reserve, noting a buffer it grew in `kb` (before and after)
    const auto reserve_start = Clock::now();
    auto reserve = [&](Buffer& b, SDL_GPUBufferUsageFlags usage, uint32_t bytes, uint32_t* kb) {
        const uint32_t before = b.buffer ? b.size : 0;
        if (!Reserve(b, usage, bytes)) return false;
        if (b.size != before) {
            st.reserve_grew++;
            kb[0] = before >> 10;
            kb[1] = b.size >> 10;
        }
        return true;
    };
    if (pool_vert_count &&
        (!reserve(pool_v, SDL_GPU_BUFFERUSAGE_VERTEX, pool_vert_count * sizeof(Vertex),
                  st.pool_verts_kb) ||
         !reserve(pool_i, SDL_GPU_BUFFERUSAGE_INDEX, pool_index_count * 2, st.pool_indices_kb))) {
        return false;
    }
    if (bone_bytes &&
        !reserve(bones, SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, bone_bytes, st.bones_kb)) {
        return false;
    }
    st.plan_reserve_ms = ms_since(reserve_start);
    st.plan_ms = ms_since(render_start) - st.pre_ms;
    const auto upload_start = Clock::now();
    if (upload_bytes > upload_size) {
        if (upload) SDL_ReleaseGPUTransferBuffer(device, upload);
        SDL_GPUTransferBufferCreateInfo tbi{};
        tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbi.size = std::max(Align(upload_bytes + upload_bytes / 4, 1u << 16), upload_size);
        upload = SDL_CreateGPUTransferBuffer(device, &tbi);
        counts.buffers++;
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
            if (!m->FromCpu()) continue;
            const Geometry& g = *m->keep;
            std::memcpy(base + mesh_at, g.verts.data(), g.verts.size() * sizeof(Vertex));
            mesh_at = Align(mesh_at + uint32_t(g.verts.size() * sizeof(Vertex)), 16);
            put_indices(base + mesh_at, g);
            mesh_at = Align(mesh_at + IndexSlots(g) * 2, 16);
        }
        if (bone_bytes) std::memcpy(base + bones_at, frame_bones.data(), bone_bytes);
        uint32_t tex_at = textures_at;
        for (const Tex* tx : new_textures) {
            for (uint32_t l = 0; l < tx->levels; l++) {
                const LevelUpload u = level_upload(*tx, l);
                for (uint32_t y = 0; y < u.rows; y++)
                    std::memcpy(base + tex_at + size_t(y) * u.pitch,
                                u.src + size_t(y) * u.row_bytes, u.row_bytes);
                tex_at += u.bytes;
            }
        }
        SDL_UnmapGPUTransferBuffer(device, upload);
    }
    st.uploads = uint32_t(to_pool.size() + to_arena.size() - arena_copied + new_textures.size());
    st.upload_ms = ms_since(upload_start);
    const auto record_start = Clock::now();

    // SDL_AcquireGPUCommandBuffer, timed (GpuStats::acquire_ms): with its
    // pool empty SDL's Direct3D 12 makes a command list and allocator. Its
    // descriptor heaps (65536 views, 2048 samplers) it takes at the buffer's
    // first draw, making a pair if none is free: timed there (first_draw_ms)
    bool first_draw = true;
    auto acquire = [&]() {
        const auto t = Clock::now();
        SDL_GPUCommandBuffer* c = SDL_AcquireGPUCommandBuffer(device);
        const double ms = ms_since(t);
        st.acquires++;
        st.acquire_ms += ms;
        st.acquire_max_ms = std::max(st.acquire_max_ms, ms);
        first_draw = true;
        return c;
    };
    // a submission's time (GpuStats::submit_ms, submit_max_ms)
    auto add_submit = [&](double ms) {
        st.submit_ms += ms;
        st.submit_max_ms = std::max(st.submit_max_ms, ms);
    };
    // and a later draw over 0.05 ms (draw_slow): its sampler heap full (2048,
    // kSamplerBatch a draw that rebinds), SDL takes another pair
    auto timed_first = [&](auto&& draw_call) {
        const auto t = Clock::now();
        draw_call();
        const double ms = ms_since(t);
        if (first_draw) {
            first_draw = false;
            st.first_draw_ms += ms;
            st.first_draw_max_ms = std::max(st.first_draw_max_ms, ms);
        } else if (ms > 0.05) {
            st.draw_slow++;
            st.draw_slow_ms += ms;
        }
    };
    // SDL keeps uniforms in 32 KB buffers, pooled: a pipeline bind takes one
    // per stage if the command buffer has none, and a push past one's end
    // another, made if the pool has none free. A draw pushes 2 KB (1 a
    // stage), so the first frame with many more draws than any before makes
    // dozens (WarmPools makes them first).
    // Such calls (over 0.05 ms) are counted (GpuStats::uniform_slow)
    auto timed_uniform = [&](auto&& call) {
        const auto t = Clock::now();
        call();
        const double ms = ms_since(t);
        if (ms > 0.05) {
            st.uniform_slow++;
            st.uniform_slow_ms += ms;
        }
    };
    SDL_GPUCommandBuffer* cmd = acquire();
    if (!cmd) {
        REXLOG_WARN("native view gpu: no command buffer ({})", SDL_GetError());
        return false;
    }
    StartTiming(cmd, o, slot, pre_pass, ahead_pass);
    // RasterOptions::gpu_labels: each indexed draw in record order, a list
    // per command buffer, handed to draw_log on submit; callers check
    // gpu_labels first so the text is only made when it's on
    std::vector<std::vector<std::string>> indexed_draws(1);
    auto label = [&](std::string text) { indexed_draws.back().push_back(std::move(text)); };
    const std::string frame_label =
        o.gpu_labels ? fmt::format("band3 frame {} (game frame {}, proc_cmds {}, composed {}) "
                                   "at {}x{}",
                                   frame.frame, frame.game_frame, frame.proc_cmds,
                                   frame.composed, width, height)
                     : std::string();
    if (upload_bytes || !to_arena.empty() || !array_copies.empty()) {
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        // grown texture arrays first; SDL keeps the old one until the copy
        // is done
        for (const ArrayCopy& c : array_copies) {
            for (const auto& [l, levels] : c.layers) {
                for (uint32_t m = 0; m < levels; m++) {
                    SDL_GPUTextureLocation src{c.from, m, l, 0, 0, 0};
                    SDL_GPUTextureLocation dst{c.to, m, l, 0, 0, 0};
                    const uint32_t w = std::max(c.w >> m, 1u), h = std::max(c.h >> m, 1u);
                    SDL_CopyGPUTextureToTexture(copy, &src, &dst, c.bc ? BcExtent(w, w) : w,
                                                c.bc ? BcExtent(h, h) : h, 1, false);
                }
            }
            SDL_ReleaseGPUTexture(device, c.from);
        }
        // The frame before may still be drawing (RenderFrameToOutput doesn't
        // wait). The pools and bones, refilled each frame, cycle so SDL gives
        // this frame a buffer the GPU isn't reading. The arena is only
        // appended to, into space no in-flight frame reads. A texture goes
        // into a new layer or one Evict or PlaceTexture freed, which in-flight
        // frames don't sample, except after a pre_pass freed the previous
        // frame's; there SDL's barriers hold this copy until those reads end.
        auto send = [&](uint32_t from, const Buffer& to, uint32_t offset, uint32_t size,
                        bool cycle) {
            if (!size) return;
            SDL_GPUTransferBufferLocation src{upload, from};
            SDL_GPUBufferRegion dst{to.buffer, offset, size};
            SDL_UploadToGPUBuffer(copy, &src, &dst, cycle);
        };
        send(0, pool_v, 0, pool_vert_count * uint32_t(sizeof(Vertex)), true);
        send(pool_index_at, pool_i, 0, pool_index_count * 2, true);
        uint32_t mesh_at = arena_at;
        for (const Mesh* m : to_arena) {
            const Geometry& g = *m->keep;
            const uint32_t vsize = uint32_t(g.verts.size() * sizeof(Vertex));
            const uint32_t isize = IndexSlots(g) * 2;
            const uint32_t vto = m->first_vertex * uint32_t(sizeof(Vertex));
            const uint32_t ito = m->first_index * 2;
            // from the old arena (kept by a rebuild) or the last frame's pool
            auto copy_in = [&](const Buffer& verts, uint32_t vertex, const Buffer& indices,
                               uint32_t index) {
                SDL_GPUBufferLocation src{verts.buffer, vertex * uint32_t(sizeof(Vertex))};
                SDL_GPUBufferLocation dst{arena_verts.buffer, vto};
                SDL_CopyGPUBufferToBuffer(copy, &src, &dst, vsize, false);
                src = {indices.buffer, index * 2};
                dst = {arena_indices.buffer, ito};
                SDL_CopyGPUBufferToBuffer(copy, &src, &dst, isize, false);
            };
            if (m->arena_vertex != ~0u) {
                copy_in(old_arena_verts, m->arena_vertex, old_arena_indices, m->arena_index);
                continue;
            }
            if (m->pool_vertex != ~0u) {
                copy_in(last_pool_v, m->pool_vertex, last_pool_i, m->pool_index);
                continue;
            }
            send(mesh_at, arena_verts, vto, vsize, false);
            mesh_at = Align(mesh_at + vsize, 16);
            send(mesh_at, arena_indices, ito, isize, false);
            mesh_at = Align(mesh_at + isize, 16);
        }
        send(bones_at, bones, 0, bone_bytes, true);
        uint32_t tex_at = textures_at;
        for (const Tex* tx : new_textures) {
            for (uint32_t l = 0; l < tx->levels; l++) {
                const LevelUpload u = level_upload(*tx, l);
                SDL_GPUTextureTransferInfo src{upload, tex_at, u.pixels_per_row, u.rows_per_layer};
                SDL_GPUTextureRegion dst{};
                dst.texture = tx->array->texture;
                dst.mip_level = l;
                dst.layer = tx->layer;
                dst.w = u.w;
                dst.h = u.h;
                dst.d = 1;
                SDL_UploadToGPUTexture(copy, &src, &dst, false);
                tex_at += u.bytes;
            }
        }
        SDL_EndGPUCopyPass(copy);
    }
    ReleaseBuffer(old_arena_verts);
    ReleaseBuffer(old_arena_indices);

    // The runs in order: the back buffer's render pass, split where
    // Rasterize() clears depth (a new camera) and resumed after each texture
    // pass; each texture pass into its target, then its mips
    SDL_GPUBuffer* bone_buffer = bone_bytes ? bones.buffer : no_bones;
    SDL_GPURenderPass* pass = nullptr;
    // bound state, so a draw binds only what changes
    SDL_GPUGraphicsPipeline* bound = nullptr;
    SDL_GPUBuffer* bound_verts = nullptr;
    SDL_GPUTexture* bound_tex[kNumSpotSlots] = {};
    float bound_viewport[4] = {};
    // the scissor is the song list's cut (InOverlayCut)
    bool scissor_cut = false;
    uint32_t pass_samples = 1;  // the open pass's targets'
    auto begin_pass = [&](const SDL_GPUColorTargetInfo& ct,
                          const SDL_GPUDepthStencilTargetInfo& dt) {
        pass = BeginPass(cmd, &ct, 1, &dt);
        scissor_cut = false;
        SDL_BindGPUVertexStorageBuffers(pass, 0, &bone_buffer, 1);
        bound = nullptr;
        bound_verts = nullptr;
        std::fill(std::begin(bound_tex), std::end(bound_tex), nullptr);
        std::fill(std::begin(bound_viewport), std::end(bound_viewport), 0.0f);
    };
    auto end_pass = [&] {
        if (pass) {
            // a multisampled overlay pass resolves as it ends: timed apart
            if (pass_samples > 1) MarkTime(cmd, gpu_timing::kOverlayResolve);
            SDL_EndGPURenderPass(pass);
        }
        pass = nullptr;
    };
    // a mid-frame submission (at a texture pass's mips and, with
    // RasterOptions::submit_points, the resolve), counted in GpuStats
    auto submit_part = [&](SDL_GPUCommandBuffer* c) {
        const auto t = Clock::now();
        const bool ok = SDL_SubmitGPUCommandBuffer(c);
        add_submit(ms_since(t));
        if (ok) st.submits++;
        return ok;
    };
    // the draws recorded by the last submission (draws_since_submit)
    uint32_t draws_at_submit = 0;
    // Submits the work so far and continues in a new command buffer (between
    // passes only) so the GPU draws while the CPU records; the gap is
    // kIdle. One queue keeps order and SDL tracks per-resource states, so the
    // frame's fence covers all and no barrier is lost. The new buffer has
    // nothing bound, so the caches reset. False on failure (caller logs).
    auto next_part = [&]() -> bool {
        MarkTime(cmd, gpu_timing::kIdle);
        SDL_GPUCommandBuffer* next = submit_part(cmd) ? acquire() : nullptr;
        if (!next) return false;
        cmd = next;
        bound = nullptr;
        bound_verts = nullptr;
        std::fill(std::begin(bound_tex), std::end(bound_tex), nullptr);
        std::fill(std::begin(bound_viewport), std::end(bound_viewport), 0.0f);
        if (o.gpu_labels) indexed_draws.emplace_back();
        draws_at_submit = st.draws;
        return true;
    };

    // the back buffer: the world into `scene` (first cleared, alpha 0), the
    // overlay into the picture after the resolve; depth cleared at first and
    // after the resolve, or per new camera in pre-camera captures
    const BackBufferLayout layout = LayoutBackBuffer(frame);
    bool clear_overlay_depth = false;
    bool back_begun = false;
    bool resolved = false;
    // the picture after the resolve, for the overlay's start and the gamma
    // pass: `color`, or the post buffer a frame showing it leaves there,
    // until a multisampled overlay pass resolves into `color`
    SDL_GPUTexture* picture = color;
    // the picture as resolved, for the overlay's REFRACT_WORLD draws: the
    // post buffer if it's that already, else `behind`
    SDL_GPUTexture* behind_now = behind;
    bool depth_fresh = false;  // the open pass's depth is cleared and untouched
    // The overlay's samples (OverlaySamples) as the device draws them; 1
    // draws into `color` over the world's depth. A capture from before
    // cameras were kept draws its overlay over the world's depth, which the
    // overlay's start must sample: single-sampled if it can't.
    SDL_GPUTexture* const world_depth = depth_sampled ? depth : no_depth;
    uint32_t overlay_samples = DeviceSamples(OverlaySamples(o));
    if (overlay_samples > 1 &&
        ((!layout.cameras && !depth_sampled) || !EnsureOverlayTargets(overlay_samples) ||
         !OverlayStartPipeline(overlay_samples)))
        overlay_samples = 1;
    bool overlay_start = false;  // the overlay's targets are yet to be started
    // the scene's alpha 0
    const uint32_t clear_rgba = ClearRgba(frame);
    const SDL_FColor clear_color = {float(clear_rgba & 0xff) / 255.0f,
                                    float(clear_rgba >> 8 & 0xff) / 255.0f,
                                    float(clear_rgba >> 16 & 0xff) / 255.0f, 0.0f};
    auto begin_back = [&](bool clear_depth) {
        // marked here, not at each texture pass's end, so back-to-back
        // passes take one timestamp each
        MarkTime(cmd, resolved ? gpu_timing::kOverlay : gpu_timing::kWorld);
        const bool ms = resolved && overlay_samples > 1;
        bool depth_cleared = false;
        if (ms && overlay_start) {
            // as RB3's DoPostProcess (BeginTiling, CopyPostProcess): the
            // picture into every sample, depth 0, or the world's for a
            // capture from before cameras were kept (if no camera clears it)
            SDL_GPUColorTargetInfo start_ct{};
            start_ct.texture = color_ms;
            start_ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
            start_ct.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPUDepthStencilTargetInfo start_dt{};
            start_dt.texture = depth_ms;
            start_dt.load_op = SDL_GPU_LOADOP_DONT_CARE;
            start_dt.store_op = SDL_GPU_STOREOP_STORE;
            start_dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            start_dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            SDL_GPURenderPass* rp = BeginPass(cmd, &start_ct, 1, &start_dt);
            SDL_BindGPUGraphicsPipeline(rp, OverlayStartPipeline(overlay_samples));
            const SDL_GPUTextureSamplerBinding tb[2] = {{picture, sampler},
                                                        {world_depth, sampler}};
            SDL_BindGPUFragmentSamplers(rp, 0, tb, 2);
            post::PostPass p{};
            const bool world = !layout.cameras && !clear_depth;
            p.mode = {0, world ? 1u : 0u, 0, 0};
            p.target = {float(width), float(height), 1.0f / float(width), 1.0f / float(height)};
            SDL_PushGPUFragmentUniformData(cmd, 0, &p, sizeof(p));
            timed_first([&] { SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0); });
            SDL_EndGPURenderPass(rp);
            overlay_start = false;
            depth_cleared = !world;
            clear_depth = false;
        }
        if (ms) picture = color;
        SDL_GPUColorTargetInfo ct{};
        ct.texture = ms ? color_ms : resolved ? color : scene;
        ct.clear_color = clear_color;
        ct.load_op = back_begun ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        if (ms) {
            ct.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
            ct.resolve_texture = color;
        }
        SDL_GPUDepthStencilTargetInfo dt{};
        dt.texture = ms ? depth_ms : depth;
        dt.clear_depth = 0.0f;
        clear_depth |= !back_begun;
        dt.load_op = clear_depth ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        dt.store_op = SDL_GPU_STOREOP_STORE;
        dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        begin_pass(ct, dt);
        // SDL_gpu's default viewport, until a draw's camera sets another
        // (PlaceBackBufferDraw)
        bound_viewport[2] = float(width);
        bound_viewport[3] = float(height);
        back_begun = true;
        depth_fresh = clear_depth || depth_cleared;
        pass_samples = ms ? overlay_samples : 1;
    };

    // a post.hlsl full-screen pass into `target` (w x h) and `second` if
    // given, reading `sources` at t0, t1...
    auto fullscreen = [&](SDL_GPUTexture* target, uint32_t w, uint32_t h,
                          SDL_GPUGraphicsPipeline* pipeline,
                          std::initializer_list<SDL_GPUTexture*> sources, post::PostPass& params,
                          SDL_GPUTexture* second = nullptr) {
        params.target = {float(w), float(h), 1.0f / float(w), 1.0f / float(h)};
        SDL_GPUColorTargetInfo ct[2]{};
        ct[0].texture = target;
        ct[1].texture = second;
        for (SDL_GPUColorTargetInfo& c : ct) {
            c.load_op = SDL_GPU_LOADOP_DONT_CARE;
            c.store_op = SDL_GPU_STOREOP_STORE;
        }
        SDL_GPURenderPass* rp = BeginPass(cmd, ct, second ? 2 : 1, nullptr);
        timed_uniform([&] { SDL_BindGPUGraphicsPipeline(rp, pipeline); });
        SDL_GPUTextureSamplerBinding tb[12];
        uint32_t n = 0;
        for (SDL_GPUTexture* t : sources) tb[n++] = {t, linear_sampler};
        SDL_BindGPUFragmentSamplers(rp, 0, tb, n);
        timed_uniform([&] { SDL_PushGPUFragmentUniformData(cmd, 0, &params, sizeof(params)); });
        timed_first([&] { SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0); });
        SDL_EndGPURenderPass(rp);
    };
    SDL_GPUTexture* const scene_depth = depth_sampled ? depth : no_depth;

    // RB3's post-processing (post_model.h), RunPost's passes: DOF, bloom,
    // then the composite into the picture
    post::PostPlan post_plan;
    const auto post_plan_start = Clock::now();
    bool post_on = o.post && o.view == RasterView::kFinal &&
                   post::PlanPost(frame, o.post_only, post_plan, o.grain, o.velocity);
    st.post_plan_ms = ms_since(post_plan_start);
    // without sampled depth DOF would blur everything: left out
    if (post_on && !depth_sampled) {
        post_plan.composite.flags.x &= ~(post::kPostDof | post::kPostVelocity);
        post_on = post_plan.composite.flags.x != 0;
    }
    // the motion blur's object pass (velocity.hlsl) over the camera pass's
    // texels, with its own depth cleared to 1
    auto velocity_objects = [&] {
        if (post_plan.velocity_objects.empty() || frame.velocity_objects.empty()) return;
        SDL_GPUColorTargetInfo ct{};
        ct.texture = post_velocity;
        ct.load_op = SDL_GPU_LOADOP_LOAD;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo dt{};
        dt.texture = post_velocity_depth;
        dt.clear_depth = 1.0f;
        dt.load_op = SDL_GPU_LOADOP_CLEAR;
        dt.store_op = SDL_GPU_STOREOP_DONT_CARE;
        dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        SDL_GPURenderPass* rp = BeginPass(cmd, &ct, 1, &dt);
        SDL_BindGPUVertexStorageBuffers(rp, 0, &bone_buffer, 1);
        const SDL_GPUTextureSamplerBinding tb{scene_depth, sampler};
        SDL_BindGPUFragmentSamplers(rp, 0, &tb, 1);
        SDL_GPUBuffer* objects_bound = nullptr;
        SDL_GPUGraphicsPipeline* pipeline_bound = nullptr;
        for (size_t n = 0; n < post_plan.velocity_objects.size(); n++) {
            const VelocityObject& v = *post_plan.velocity_objects[n];
            const size_t index = size_t(&v - frame.velocity_objects.data());
            if (index >= frame.velocity_objects.size()) continue;
            const auto found = meshes.find(v.geom.get());
            if (found == meshes.end() || found->second.used != serial) continue;
            const Mesh& m = found->second;
            RasterOptions cull_options = o;
            DrawItem cull_item{};
            cull_item.cull = v.cull;
            const CullWinding cull = CullFor(cull_item, cull_options);
            if (cull == CullWinding::kAll) continue;
            SDL_GPUGraphicsPipeline* pipeline = velocity_object_pipelines[int(cull)];
            if (pipeline != pipeline_bound) {
                SDL_BindGPUGraphicsPipeline(rp, pipeline);
                pipeline_bound = pipeline;
            }
            SDL_GPUBuffer* verts = m.in_arena ? arena_verts.buffer : pool_v.buffer;
            if (verts != objects_bound) {
                const SDL_GPUBufferBinding vb{verts, 0};
                SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
                const SDL_GPUBufferBinding ib{m.in_arena ? arena_indices.buffer : pool_i.buffer, 0};
                SDL_BindGPUIndexBuffer(rp, &ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);
                objects_bound = verts;
            }
            post::VelocityObjectPass u = post_plan.velocity_object_passes[n];
            u.mesh.z = velocity_bone_base[index];
            u.target = {float(velocity_w), float(velocity_h), 1.0f / float(velocity_w),
                        1.0f / float(velocity_h)};
            u.depth = {width, height, 0, 0};
            SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof(u));
            SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
            if (o.gpu_labels)
                label(fmt::format("velocity object {}: mesh {:#x}, {} indices, {} vertices, {} "
                                  "bones",
                                  index, v.mesh, v.geom->indices.size(), v.geom->verts.size(),
                                  v.bones));
            timed_first([&] {
                SDL_DrawGPUIndexedPrimitives(rp, uint32_t(v.geom->indices.size() / 3 * 3), 1,
                                             m.first_index, int32_t(m.first_vertex), 0);
            });
        }
        SDL_EndGPURenderPass(rp);
    };
    auto post_process = [&] {
        post::PostPass p = post_plan.composite;
        const uint32_t flags = p.flags.x;
        // 4x downsample (or bright pass) of `src` into `dst` at level k's size
        auto downsample = [&](SDL_GPUTexture* src, uint32_t sw, uint32_t sh, SDL_GPUTexture* dst,
                              int k, bool bright) {
            p.mode = {0, bright ? 1u : 0u, 0, 0};
            p.half_pixel = {0.5f / float(sw), 0.5f / float(sh), 0, 0};
            fullscreen(dst, post_w[k], post_h[k], downsample_pipeline, {src}, p);
        };
        // across into k's spare, then down back into `level`
        auto blur = [&](SDL_GPUTexture* level, int k, const post::float4* across,
                        const post::float4* down, uint32_t taps) {
            p.mode = {0, 0, taps, 0};
            std::copy(across, across + taps, p.taps);
            fullscreen(post_tmp[k], post_w[k], post_h[k], blur_pipeline, {level}, p);
            std::copy(down, down + taps, p.taps);
            fullscreen(level, post_w[k], post_h[k], blur_pipeline, {post_tmp[k]}, p);
        };
        // camera velocity from the scene's depth (t1), then the objects over it
        if (flags & post::kPostVelocity) {
            MarkTime(cmd, gpu_timing::kVelocity);
            p.mode = {0, width, height, 0};
            fullscreen(post_velocity, velocity_w, velocity_h, velocity_pipeline,
                       {scene, scene_depth}, p);
            velocity_objects();
        }
        if (flags & post::kPostDof) {
            MarkTime(cmd, gpu_timing::kDof);
            downsample(scene, width, height, post_dof, 0, false);
            blur(post_dof, 0, post_plan.dof_taps[0], post_plan.dof_taps[1], 8);
        }
        // the level 0 the composite reads
        SDL_GPUTexture* bloom0 = post_bloom[0];
        if (flags & (post::kPostBloom | post::kPostGlare)) {
            MarkTime(cmd, gpu_timing::kBloom);
            // glare has level 0 only
            const int levels = (flags & post::kPostBloom) ? 3 : 1;
            for (int k = 0; k < levels; k++) {
                if (k)
                    downsample(post_bloom[k - 1], post_w[k - 1], post_h[k - 1], post_bloom[k], k,
                               false);
                else
                    downsample(scene, width, height, post_bloom[0], 0, true);
                blur(post_bloom[k], k, post_plan.bloom_taps[k][0], post_plan.bloom_taps[k][1], 15);
            }
            // glare into level 0's spare
            if (flags & post::kPostGlare) {
                p.mode = {0, 0, 0, 0};
                fullscreen(post_tmp[0], post_w[0], post_h[0], glare_pipeline, {post_bloom[0]}, p);
                bloom0 = post_tmp[0];
            }
        }
        // a target this frame (or its world ahead) drew, else transparent
        // black
        auto drawn_now = [&](uint32_t tex_obj) {
            const auto f = rts.find(tex_obj);
            return tex_obj && f != rts.end() &&
                           (f->second.drawn_in == serial ||
                            (uses_ahead && f->second.drawn_in == ahead_was.serial))
                       ? f->second.color
                       : black;
        };
        // without a layer the noise is left out
        SDL_GPUTexture* noise = black;
        if (flags & post::kPostNoise) {
            const Tex* tx = TextureFor(post_plan.noise);
            if (tx) {
                noise = tx->array->texture;
                p.noise_tex.z = tx->layer;
                uint32_t packed[4];
                PackSampler(frame.noise_sampler, tx->levels, packed);
                p.noise_sampler = {packed[0], packed[1], packed[2], packed[3]};
            } else {
                p.flags.x &= ~(post::kPostNoise | post::kPostNoiseMidtone);
            }
        }
        // levels of effects that are off are bound but not read
        MarkTime(cmd, gpu_timing::kComposite);
        p.mode = {0, 0, 0, 0};
        // The live view's composite also writes the trails' post buffer into
        // the history's other texture, which a post frame makes current
        // (once per game frame); trails need an earlier post frame.
        if (o.trails && EnsureHistory(width, height)) {
            const bool have_prev = history.cur >= 0 && history.game_frame &&
                                   history.game_frame < frame.game_frame;
            if (!have_prev) p.flags.x &= ~post::kPostTrails;
            const int next = history.cur == 0 ? 1 : 0;
            SDL_GPUTexture* prev = history.tex[1 - next];
            fullscreen(color, width, height, composite_history_pipeline,
                       {scene, scene_depth, post_dof, bloom0, post_bloom[1], post_bloom[2],
                        drawn_now(post_plan.spot_volume), drawn_now(post_plan.spot_density),
                        drawn_now(post_plan.soft), noise, post_velocity, prev},
                       p, history.tex[next]);
            if (post_plan.trails_update && frame.game_frame &&
                frame.game_frame != history.game_frame) {
                history.cur = next;
                history.game_frame = frame.game_frame;
            }
            return;
        }
        p.flags.x &= ~post::kPostTrails;
        fullscreen(color, width, height, composite_pipeline,
                   {scene, scene_depth, post_dof, bloom0, post_bloom[1], post_bloom[2],
                    drawn_now(post_plan.spot_volume), drawn_now(post_plan.spot_density),
                    drawn_now(post_plan.soft), noise, post_velocity},
                   p);
    };

    // whether an overlay draw reads the picture behind it, so the resolve
    // keeps a copy
    bool refracts = false;
    for (size_t d = frame.post_boundary; d < frame.draws.size() && !refracts; d++) {
        const DrawItem& it = frame.draws[d];
        refracts = DrawnToBackBuffer(it) && RefractsWorld(shade::ShadeOf(frame, it));
    }

    // the scene into the picture at post_boundary or the frame's end:
    // post-processed, as is, or the requested view. False if the submission
    // before it failed (logged).
    auto resolve = [&]() -> bool {
        end_pass();
        // the scene cleared if nothing drew to it (the world ahead counts)
        if (!back_begun && uses_ahead) {
            back_begun = true;
        } else if (!back_begun) {
            begin_back(true);
            end_pass();
        }
        // RasterOptions::submit_points: submit the world so the GPU draws it
        // while the CPU records the rest (SubmitAtResolve)
        if (SubmitAtResolve(o.submit_points, pre_pass || ahead_pass,
                            st.draws - draws_at_submit) &&
            !next_part()) {
            REXLOG_WARN("native view gpu: the frame's world didn't submit ({})", SDL_GetError());
            return false;
        }
        // the pre-post scene, as DoWorldEnd's SavePreBuffer keeps it, for the
        // next world frame's or world pass's REFRACT_WORLD draws
        MarkTime(cmd, gpu_timing::kCopies);
        if (pre_pass || keeps_pre_now) {
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
            const SDL_GPUTextureLocation from{scene, 0, 0, 0, 0, 0};
            const SDL_GPUTextureLocation to{pre_pass ? pre_scratch.texture : pre_buffer.tex, 0,
                                            0, 0, 0, 0};
            SDL_CopyGPUTextureToTexture(copy, &from, &to, width, height, 1, false);
            SDL_EndGPUCopyPass(copy);
            if (!pre_pass) pre_buffer.game_frame = WorldFrameOf(frame);
        }
        // the next post frame post-processes it
        if (ahead_pass) {
            resolved = true;
            return true;
        }
        // Multisampled, the overlay's start reads the post buffer directly
        // and resolves into `color`, so no copy is needed
        if (shows_kept && overlay_samples > 1) {
            picture = post_buffer.tex;
        } else if (shows_kept) {
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
            const SDL_GPUTextureLocation from{post_buffer.tex, 0, 0, 0, 0, 0};
            const SDL_GPUTextureLocation to{color, 0, 0, 0, 0, 0};
            SDL_CopyGPUTextureToTexture(copy, &from, &to, width, height, 1, false);
            SDL_EndGPUCopyPass(copy);
        } else if (post_on) {
            post_process();
        } else {
            MarkTime(cmd, gpu_timing::kComposite);
            post::PostPass p{};
            p.mode = {uint32_t(o.view), 0, 0, 0};
            fullscreen(color, width, height, resolve_pipeline, {scene, scene_depth}, p);
        }
        MarkTime(cmd, gpu_timing::kCopies);
        if (keeps) {
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
            const SDL_GPUTextureLocation from{color, 0, 0, 0, 0, 0};
            const SDL_GPUTextureLocation to{post_buffer.tex, 0, 0, 0, 0, 0};
            SDL_CopyGPUTextureToTexture(copy, &from, &to, width, height, 1, false);
            SDL_EndGPUCopyPass(copy);
            post_buffer.game_frame = frame.game_frame;
        }
        // the post buffer if it's this picture, else a copy
        if (refracts && (shows_kept || keeps)) {
            behind_now = post_buffer.tex;
        } else if (refracts) {
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
            const SDL_GPUTextureLocation from{color, 0, 0, 0, 0, 0};
            const SDL_GPUTextureLocation to{behind, 0, 0, 0, 0, 0};
            SDL_CopyGPUTextureToTexture(copy, &from, &to, width, height, 1, false);
            SDL_EndGPUCopyPass(copy);
        }
        resolved = true;
        MarkTime(cmd, gpu_timing::kOverlay);
        // the overlay's depth starts cleared with the capture's cameras, as
        // Rasterize's resolve does
        clear_overlay_depth = layout.cameras;
        overlay_start = overlay_samples > 1;
        return true;
    };

    // the last density map drawn (0 none), which cones read; the current
    // texture pass's target
    uint32_t density_obj = 0;
    const Rt* pass_rt = nullptr;

    // one draw into the open pass; `no_z`: target without depth;
    // `shadow_depth`: into a shadow map, depth alone
    auto draw = [&](size_t d, AlphaMode alpha, bool no_z, bool shadow_depth = false,
                    const DepthMap& depth_map = DepthMap{}, bool overlay_edge = false) {
        const DrawItem& it = frame.draws[d];
        const Mesh& m = meshes[it.geom.get()];
        const int blend = BlendFor(it, o);
        const CullWinding cull = CullFor(it, o);
        if (cull == CullWinding::kAll) return;
        const bool cone = spot_draw[d] == kSpotCone;
        const bool soft = spot_draw[d] == kSoftParticle;
        const PixelKind kind = cone ? PixelKind::kSpot : soft ? PixelKind::kSoft : PixelKind::kMesh;
        SDL_GPUGraphicsPipeline* pipeline =
            shadow_depth ? ShadowDepthPipeline(cull)
                         : Pipeline(blend, RulesFor(it, o, no_z), alpha, cull, kind, pass_samples);
        if (!pipeline) {
            st.skipped++;
            return;
        }
        if (pipeline != bound) {
            timed_uniform([&] { SDL_BindGPUGraphicsPipeline(pass, pipeline); });
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
        // needs only the vertex positions
        if (shadow_depth) {
            VertexUniforms vu{};
            vu.world = it.world;
            vu.view_proj = it.view_proj;
            if (Skinned(it, o)) {
                vu.skinned = 1;
                vu.bone_base = bone_base[d];
                vu.bone_count = uint32_t(it.bones.size());
            }
            vu.shadow_depth = 1;
            ClipOffset(it, bound_viewport[2], bound_viewport[3], vu.clip_offset);
            SetDepthMap(depth_map, vu.depth_map);
            vu.shade = shades[d];
            timed_uniform([&] { SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu)); });
            if (o.gpu_labels)
                label(fmt::format("draw {} shadow depth: mesh {:#x} into {:#x}, {} indices, {} "
                                  "vertices, {} bones",
                                  d, it.mesh, it.target, it.geom->indices.size(),
                                  it.geom->verts.size(), it.bones.size()));
            timed_first([&] {
                SDL_DrawGPUIndexedPrimitives(pass, uint32_t(it.geom->indices.size() / 3 * 3), 1,
                                             m.first_index, int32_t(m.first_vertex), 0);
            });
            st.draws++;
            return;
        }

        // an array layer or a render target; a map without a layer is left
        // out, as if the capture had none
        struct Sampled {
            SDL_GPUTexture* texture = nullptr;
            uint32_t layer = 0, w = 0, h = 0;
            uint32_t levels = 1;
            bool dxn = false;  // a DXN texture kept as BC5 (BcFormat)
            // a k_8 texture kept as R8: how its texels expand (KeptFormat)
            uint32_t r8 = 0;
        };
        auto layer_of = [&](const Texture* t) {
            const Tex* tx = TextureFor(t);
            if (!tx) return Sampled{};
            const SDL_GPUTextureFormat f = tx->array->format;
            return Sampled{tx->array->texture,
                           tx->layer,
                           t->width,
                           t->height,
                           tx->levels,
                           f == SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM,
                           f == SDL_GPU_TEXTUREFORMAT_R8_UNORM
                               ? guest_format::R8ExpandCode(tx->keep->blocks->swizzle)
                               : 0u};
        };
        shade::ShadeParams& sp = shades[d];
        const ShadeState* state = shade::ShadeOf(frame, it);
        Sampled tex[kNumSlots];
        switch (diffuse_source[d]) {
            case kSourceTexture: tex[kSlotDiffuse] = layer_of(it.tex.get()); break;
            case kSourceRt: {
                const Rt& rt = rts[it.tex->tex_obj];
                tex[kSlotDiffuse] = {rt.color, 0, rt.w, rt.h, rt.levels};
                break;
            }
            case kSourceBlack: tex[kSlotDiffuse] = {black, 0, 1, 1}; break;
            default: break;
        }
        if (tex[kSlotDiffuse].texture) sp.flags.x |= shade::kShadeTextured;
        if (sp.flags.x & shade::kShadeSpecMap) {
            tex[kSlotSpecular] = layer_of(state->maps[kMapSpecular].get());
            if (!tex[kSlotSpecular].texture) sp.flags.x &= ~shade::kShadeSpecMap;
        }
        if (sp.flags.x & shade::kShadeGlow) {
            tex[kSlotGlow] = layer_of(state->maps[kMapGlow].get());
            if (!tex[kSlotGlow].texture) sp.flags.x &= ~shade::kShadeGlow;
        }
        if (sp.flags.x & (shade::kShadeNormalMap | shade::kShadeRefractMap)) {
            auto map_of = [&](int k) {
                const int m = k ? kMapDetailNormal : kMapNormal;
                if (normal_source[k][d] != kSourceRt) return layer_of(state->maps[m].get());
                const Rt& rt = rts[MapTargetOf(state, m)->tex_obj];
                return Sampled{rt.color, 0, rt.w, rt.h, rt.levels};
            };
            tex[kSlotNormal] = map_of(0);
            if (sp.flags.x & shade::kShadeDetailMap) tex[kSlotDetail] = map_of(1);
            if (!tex[kSlotDetail].texture) sp.flags.x &= ~shade::kShadeDetailMap;
            if (!tex[kSlotNormal].texture)
                sp.flags.x &= ~(shade::kShadeNormalMap | shade::kShadeDetailMap |
                                shade::kShadeRefractMap);
        }
        // the plan kept the flag only where the target has the right version
        if (sp.flags.x & shade::kShadeShadow) {
            const Rt& rt = rts[ShadowMapOf(state)->tex_obj];
            tex[kSlotShadow] = {rt.color, 0, rt.w, rt.h};
            shade::RescaleShadowCoord(sp, rt.game_w, rt.game_h, rt.w, rt.h);
        }
        if (sp.flags.x & (shade::kShadeProjMultiply | shade::kShadeProjGobo)) {
            if (proj_source[d] == kSourceRt) {
                const Rt& rt = rts[ProjectedTargetOf(state)->tex_obj];
                tex[kSlotProjected] = {rt.color, 0, rt.w, rt.h};
            } else {
                tex[kSlotProjected] = layer_of(state->maps[kMapProjected].get());
            }
            if (sp.flags.x & shade::kShadeProjGobo)
                tex[kSlotGobo] = layer_of(state->maps[kMapGobo].get());
            if (!tex[kSlotProjected].texture ||
                ((sp.flags.x & shade::kShadeProjGobo) && !tex[kSlotGobo].texture))
                sp.flags.x &= ~(shade::kShadeProjMultiply | shade::kShadeProjGobo);
        }
        // REFRACT_WORLD reads the picture behind it: in the overlay, the
        // resolve's copy; in the world, world_behind
        if (sp.flags.x & shade::kShadeRefract) {
            if (resolved && refracts && alpha != AlphaMode::kTexture)
                tex[kSlotBehind] = {behind_now, 0, width, height};
            else if (!resolved && alpha != AlphaMode::kTexture)
                tex[kSlotBehind] = {world_behind, 0, width, height};
            else
                sp.flags.x &= ~(shade::kShadeRefract | shade::kShadeRefractMap);
        }
        for (int s = 0; s < kNumSlots; s++) {
            // behind's and the shadow map's bindings are plain 2D, as is
            // no_depth
            SDL_GPUTexture* sampled = tex[s].texture                            ? tex[s].texture
                                      : s == kSlotBehind || s == kSlotShadow ? no_depth
                                                                             : white;
            if (sampled == bound_tex[s]) continue;
            const SDL_GPUTextureSamplerBinding ts{sampled, sampler};
            SDL_BindGPUFragmentSamplers(pass, uint32_t(s), &ts, 1);
            bound_tex[s] = sampled;
        }
        // a cone reads the scene's depth at its screen pixel and the last
        // density map (black if none), with uniforms in a second buffer
        // (soft_raster.cpp's SpotPixel)
        if (cone) {
            SDL_GPUTexture* density = black;
            if (auto f = rts.find(density_obj);
                density_obj && f != rts.end() && f->second.drawn_in == serial)
                density = f->second.color;
            const SDL_GPUTextureSamplerBinding spot_tex[2] = {{depth, sampler},
                                                              {density, linear_sampler}};
            if (spot_tex[0].texture != bound_tex[kSlotSceneDepth] ||
                spot_tex[1].texture != bound_tex[kSlotDensity]) {
                SDL_BindGPUFragmentSamplers(pass, kSlotSceneDepth, spot_tex, 2);
                bound_tex[kSlotSceneDepth] = spot_tex[0].texture;
                bound_tex[kSlotDensity] = spot_tex[1].texture;
            }
            SpotUniforms su{};
            spot::PackSpot(*state, pass_rt->w, pass_rt->h, su.spot);
            su.viewport[0] = bound_viewport[0];
            su.viewport[1] = bound_viewport[1];
            su.viewport[2] = 1.0f / bound_viewport[2];
            su.viewport[3] = 1.0f / bound_viewport[3];
            su.sizes[0] = width;
            su.sizes[1] = height;
            SDL_PushGPUFragmentUniformData(cmd, 1, &su, sizeof(su));
        }
        // a soft particle reads the scene's depth too (0, unfaded, if it
        // can't be sampled) and the far plane in depth_range
        // (soft_raster.cpp's SoftPixelFade)
        if (soft) {
            if (scene_depth != bound_tex[kSlotSceneDepth]) {
                const SDL_GPUTextureSamplerBinding ts{scene_depth, sampler};
                SDL_BindGPUFragmentSamplers(pass, kSlotSceneDepth, &ts, 1);
                bound_tex[kSlotSceneDepth] = scene_depth;
            }
            SpotUniforms su{};
            const float* c89 = state->Ps(89);
            su.spot.depth_range = {c89[0], c89[1], c89[2], c89[3]};
            su.viewport[0] = bound_viewport[0];
            su.viewport[1] = bound_viewport[1];
            su.viewport[2] = 1.0f / bound_viewport[2];
            su.viewport[3] = 1.0f / bound_viewport[3];
            su.sizes[0] = width;
            su.sizes[1] = height;
            SDL_PushGPUFragmentUniformData(cmd, 1, &su, sizeof(su));
        }

        VertexUniforms vu{};
        vu.world = it.world;
        vu.view_proj = it.view_proj;
        if (Skinned(it, o)) {
            vu.skinned = 1;
            vu.bone_base = bone_base[d];
            vu.bone_count = uint32_t(it.bones.size());
        }
        ClipOffset(it, bound_viewport[2], bound_viewport[3], vu.clip_offset);
        SetDepthMap(depth_map, vu.depth_map);
        if (overlay_edge) {
            vu.overlay_edge[0] = o.overlay_edge[0];
            vu.overlay_edge[1] = o.overlay_edge[1];
        }
        vu.shade = sp;
        timed_uniform([&] { SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu)); });
        PixelUniforms pu{};
        pu.shade = sp;
        for (int s = 0; s < kSlotBehind; s++) {
            if (!tex[s].texture) continue;
            pu.tex_layer[s] = tex[s].layer;
            pu.tex_size[s][0] = tex[s].w;
            pu.tex_size[s][1] = tex[s].h;
        }
        pu.tex_size[5][0] = tex[kSlotShadow].w;
        pu.tex_size[5][1] = tex[kSlotShadow].h;
        for (int s : {kSlotNormal, kSlotDetail}) {
            if (!tex[s].texture) continue;
            pu.tex_layer[s - 2] = tex[s].layer;
            pu.tex_size[s - 1][0] = tex[s].w;
            pu.tex_size[s - 1][1] = tex[s].h;
        }
        // the game's samplers if kept and filtering is on, else nearest (as
        // soft_raster.cpp's DrawOne)
        {
            const TexSampler none;
            auto sampler_of = [&](int slot) -> const TexSampler& {
                if (!o.filtering || !state) return none;
                switch (slot) {
                    case kSlotDiffuse: return state->diffuse_sampler;
                    case kSlotSpecular: return state->samplers[kMapSpecular];
                    case kSlotGlow: return state->samplers[kMapGlow];
                    case kSlotNormal: return state->samplers[kMapNormal];
                    case kSlotDetail: return state->samplers[kMapDetailNormal];
                    default: return none;  // the projected light filters its own way
                }
            };
            for (int s : {kSlotDiffuse, kSlotSpecular, kSlotGlow, kSlotNormal, kSlotDetail}) {
                if (!tex[s].texture) continue;
                PackSampler(sampler_of(s), tex[s].levels,
                            pu.tex_sampler[s >= kSlotNormal ? s - 2 : s]);
            }
        }
        pu.flags[0] = blend == kBlendSrcAlpha || blend == kBlendSrcAlphaAdd ? kPremultiply : 0;
        // DXN kept as BC5, in tex_layer's order: y copied to z and w as
        // DecodeBlock has it (mesh.hlsl's MapTexel; a head's normal map pass
        // reads the alpha)
        for (int s : {kSlotDiffuse, kSlotSpecular, kSlotGlow, kSlotProjected, kSlotGobo,
                      kSlotNormal, kSlotDetail})
            if (tex[s].dxn) pu.flags[1] |= 1u << (s >= kSlotNormal ? s - 2 : s);
        // the k_8 textures kept as R8, by tex_layer's order: how each texel
        // expands to the one DecodeLevel8 makes by its swizzle, a byte each
        // (guest_formats.h's R8ExpandCode; mesh.hlsl's MapTexel), maps 0-3 in
        // z and 4-6 in w
        for (int s : {kSlotDiffuse, kSlotSpecular, kSlotGlow, kSlotProjected, kSlotGobo,
                      kSlotNormal, kSlotDetail}) {
            const uint32_t k = s >= kSlotNormal ? s - 2 : s;
            pu.flags[2 + k / 4] |= tex[s].r8 << (8 * (k % 4));
        }
        timed_uniform([&] { SDL_PushGPUFragmentUniformData(cmd, 0, &pu, sizeof(pu)); });

        if (o.gpu_labels) {
            // size and sampler anisotropy, in tex_layer's order
            std::string textures;
            for (int t = 0; t < 8; t++)
                if (pu.tex_size[t][0])
                    textures += fmt::format(" {}:{}x{}/{}", t, pu.tex_size[t][0],
                                            pu.tex_size[t][1], pu.tex_sampler[t][3]);
            label(fmt::format("draw {} {}: mesh {:#x} into {:#x}, {} indices, {} vertices, {} "
                              "bones, blend {}, alpha {}, {} samples, shade {}, rect {}, "
                              "textures{}, layers {} {} {} {} {}",
                              d, cone ? "spot cone" : soft ? "soft particle" : "mesh", it.mesh,
                              it.target, it.geom->indices.size(), it.geom->verts.size(),
                              it.bones.size(), blend, int(alpha), pass_samples, it.shade,
                              it.rect_shader, textures, pu.tex_layer[0], pu.tex_layer[1],
                              pu.tex_layer[2], pu.tex_layer[3], pu.tex_layer[4]));
        }
        timed_first([&] {
            SDL_DrawGPUIndexedPrimitives(pass, uint32_t(it.geom->indices.size() / 3 * 3), 1,
                                         m.first_index, int32_t(m.first_vertex), 0);
        });
        st.draws++;
    };

    cams_seen.clear();
    uint32_t last_cam = 0;
    // back buffer draws in their cameras' viewports, layered by z range
    // (soft_raster.h's LayoutBackBuffer, as Rasterize())
    for (size_t r = 0; r < runs.size(); r++) {
        const PassRun& run = runs[r];
        if ((pre_pass || ahead_pass) && resolved) break;
        if (!run.pass) {
            for (size_t d = run.first; d < run.end; d++) {
                const DrawItem& it = frame.draws[d];
                if (!DrawnToBackBuffer(it) || (shows_kept && d < frame.post_boundary)) continue;
                if (d < ahead_end) continue;
                if (!resolved && d >= frame.post_boundary && !resolve()) return false;
                if (resolved && (o.view != RasterView::kFinal || pre_pass || ahead_pass)) break;
                bool clear_depth = false;
                // DrawRect quads have no camera
                if (it.rect_shader < 0) {
                    if (o.clear_depth_per_camera && !layout.cameras && it.cam != last_cam &&
                        std::find(cams_seen.begin(), cams_seen.end(), it.cam) ==
                            cams_seen.end()) {
                        cams_seen.push_back(it.cam);
                        clear_depth = true;
                    }
                    last_cam = it.cam;
                }
                if (!Drawable(it) || !DrawnByOptions(frame, it, o)) {
                    st.skipped++;
                    continue;
                }
                clear_depth |= clear_overlay_depth;
                clear_overlay_depth = false;
                if (pass && clear_depth && !depth_fresh) end_pass();
                if (!pass) begin_back(clear_depth);
                float vp[4];
                DepthMap depth_map;
                PlaceBackBufferDraw(layout, frame, it, width, height, vp, depth_map);
                if (!std::equal(std::begin(vp), std::end(vp), bound_viewport)) {
                    const SDL_GPUViewport v{vp[0], vp[1], vp[2], vp[3], 0.0f, 1.0f};
                    SDL_SetGPUViewport(pass, &v);
                    std::copy(std::begin(vp), std::end(vp), bound_viewport);
                }
                AlphaMode alpha = AlphaMode::kNone;
                if (!resolved && WritesSceneAlpha(shade::ShadeOf(frame, it)))
                    alpha = AlphaMode::kScene;
                // whole-picture overlay camera draws reach the edges
                // (RasterOptions::overlay_edge); DrawRect's quads are in
                // pixels
                const bool whole = vp[0] == 0.0f && vp[1] == 0.0f &&
                                   vp[2] == float(width) && vp[3] == float(height);
                const bool overlay_whole = resolved && whole && it.rect_shader < 0;
                // except the song list's rows, scissored to 16:9 (InOverlayCut)
                const bool cut = overlay_whole && InOverlayCut(it, o);
                if (cut != scissor_cut) {
                    const float e = o.overlay_edge[0];
                    const int x0 = cut ? int(std::lround((1 - e) / 2 * float(width))) : 0;
                    const int x1 = cut ? int(std::lround((1 + e) / 2 * float(width))) : int(width);
                    const SDL_Rect r{x0, 0, x1 - x0, int(height)};
                    SDL_SetGPUScissor(pass, &r);
                    scissor_cut = cut;
                }
                draw(d, alpha, false, false, depth_map, overlay_whole && !cut);
                depth_fresh = false;
            }
            continue;
        }
        if (!(run_clear[r] & kRunDrawn)) continue;
        end_pass();
        const Pass& p = *run.pass;
        const Rt& rt = rts[p.tex_obj];
        pass_rt = &rt;
        if (p.tex_type == kTexTypeDensityMap) density_obj = p.tex_obj;
        const bool no_z = (p.tex_type & kTexTypeNoZ) != 0;
        // a shadow map's colour is its depth (clip z/w), both cleared to the
        // pass's clear_z, else the far plane (soft_raster.cpp's RtTarget::zw)
        const bool shadow_map = rt.shadow;
        const float clear_z = (p.clear_flags & 0x30) ? p.clear_z : 1.0f;
        // blurs are timed apart
        const uint8_t pass_part =
            shadow_map ? gpu_timing::kPassShadow
            : p.tex_type == kTexTypeDepthVolume || p.tex_type == kTexTypeDensityMap
                ? gpu_timing::kPassSpot
                : gpu_timing::kPassOther;
        MarkTime(cmd, pass_part);
        // cleared as the run says the first time; loaded after a blur
        auto begin_rt = [&](bool first) {
            SDL_GPUColorTargetInfo ct{};
            ct.texture = rt.color;
            const uint32_t c = (p.clear_flags & 0x0f) ? ArgbToRgba(p.clear_color) : 0;
            ct.clear_color = {float(c & 0xff) / 255.0f, float(c >> 8 & 0xff) / 255.0f,
                              float(c >> 16 & 0xff) / 255.0f, float(c >> 24) / 255.0f};
            ct.load_op = first && (run_clear[r] & kRunClearColor) ? SDL_GPU_LOADOP_CLEAR
                                                                  : SDL_GPU_LOADOP_LOAD;
            if (shadow_map) {
                ct.clear_color = {clear_z, clear_z, clear_z, clear_z};
                ct.load_op = first && (run_clear[r] & kRunClearDepth) ? SDL_GPU_LOADOP_CLEAR
                                                                      : SDL_GPU_LOADOP_LOAD;
            }
            ct.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPUDepthStencilTargetInfo dt{};
            dt.texture = rt.depth;
            dt.clear_depth = shadow_map ? clear_z : 0.0f;
            dt.load_op = no_z || (first && (run_clear[r] & kRunClearDepth)) ? SDL_GPU_LOADOP_CLEAR
                                                                            : SDL_GPU_LOADOP_LOAD;
            dt.store_op = no_z ? SDL_GPU_STOREOP_DONT_CARE : SDL_GPU_STOREOP_STORE;
            dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            begin_pass(ct, dt);
            pass_samples = 1;
        };
        begin_rt(true);
        for (size_t d = run.first; d < run.end; d++) {
            const DrawItem& it = frame.draws[d];
            if (!DrawnIn(run, frame, it, o)) continue;
            if (spot_draw[d] == kSpotConeSkipped) {
                st.skipped++;
                continue;
            }
            if (spot_draw[d] == kSpotBlur) {
                // The depth volume's (or NgLight's shadow's) blur, as
                // soft_raster.cpp's SpotBlurDraw and BlurRT (BlurShadowRT):
                // taps of a copy (offsets c31.., weights c47..) over the whole
                // target with Src. Separate scratches so neither remakes the other's.
                end_pass();
                Scratch& scratch =
                    p.tex_type == kTexTypeDepthVolume ? spot_scratch : light_scratch;
                if (rt.shadow || !EnsureScratch(scratch, rt.w, rt.h)) {
                    st.skipped++;
                    continue;
                }
                MarkTime(cmd, gpu_timing::kPassBlur);
                SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
                const SDL_GPUTextureLocation from{rt.color, 0, 0, 0, 0, 0};
                const SDL_GPUTextureLocation to{scratch.texture, 0, 0, 0, 0, 0};
                SDL_CopyGPUTextureToTexture(copy, &from, &to, rt.w, rt.h, 1, false);
                SDL_EndGPUCopyPass(copy);
                const ShadeState& state = *shade::ShadeOf(frame, it);
                post::PostPass blur{};
                // a target bigger than the game's: each tap covers the game's
                // texel (soft_raster.h's BlurSubTaps)
                const BlurSubTaps sub = BlurSubTapsFor(state, spot::kSpotBlurTaps, p, rt.w, rt.h);
                blur.mode = {0, 0, uint32_t(spot::kSpotBlurTaps), sub.count};
                blur.half_pixel = {0, 0, sub.step[0], sub.step[1]};
                for (int k = 0; k < spot::kSpotBlurTaps; k++)
                    blur.taps[k] = {state.Ps(31 + k)[0], state.Ps(31 + k)[1], state.Ps(47 + k)[0],
                                    0};
                fullscreen(rt.color, rt.w, rt.h, blur_pipeline, {scratch.texture}, blur);
                st.draws++;
                continue;
            }
            if (spot_draw[d] == kSoftBlur) {
                // The soft-particle blur, as soft_raster.cpp's TapBlurDraw:
                // a copy of the other surface (the blur reads plain 2D), its
                // taps over all of this one, as BlurSurface draws with Src;
                // transparent black (no_depth) if no pass drew it
                end_pass();
                MarkTime(cmd, gpu_timing::kPassBlur);
                SDL_GPUTexture* source = no_depth;
                const auto f = rts.find(it.tex->tex_obj);
                if (f != rts.end() && f->second.drawn_in == serial) {
                    const Rt& from_rt = f->second;
                    if (!EnsureScratch(soft_scratch, from_rt.w, from_rt.h)) {
                        st.skipped++;
                        continue;
                    }
                    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
                    const SDL_GPUTextureLocation from{from_rt.color, 0, 0, 0, 0, 0};
                    const SDL_GPUTextureLocation to{soft_scratch.texture, 0, 0, 0, 0, 0};
                    SDL_CopyGPUTextureToTexture(copy, &from, &to, from_rt.w, from_rt.h, 1, false);
                    SDL_EndGPUCopyPass(copy);
                    source = soft_scratch.texture;
                } else {
                    st.rt_missing++;
                }
                const ShadeState& state = *shade::ShadeOf(frame, it);
                post::PostPass blur{};
                const BlurSubTaps sub = BlurSubTapsFor(state, kSoftBlurTaps, p, rt.w, rt.h);
                blur.mode = {0, 0, uint32_t(kSoftBlurTaps), sub.count};
                blur.half_pixel = {0, 0, sub.step[0], sub.step[1]};
                for (int k = 0; k < kSoftBlurTaps; k++)
                    blur.taps[k] = {state.Ps(31 + k)[0], state.Ps(31 + k)[1], state.Ps(47 + k)[0],
                                    0};
                fullscreen(rt.color, rt.w, rt.h, blur_pipeline, {source}, blur);
                st.draws++;
                continue;
            }
            if (!pass) {
                MarkTime(cmd, pass_part);  // after a blur
                begin_rt(false);
            }
            // the camera's viewport scaled with the target; DrawRect's quads
            // cover all of it in pixels (as soft_raster.cpp)
            float vp[4] = {0, 0, float(rt.w), float(rt.h)};
            if (it.rect_shader < 0 && p.viewport[2] > 0 && p.viewport[3] > 0)
                ScalePassViewport(p, rt.w, rt.h, vp);
            if (!std::equal(std::begin(vp), std::end(vp), bound_viewport)) {
                const SDL_GPUViewport v{vp[0], vp[1], vp[2], vp[3], 0.0f, 1.0f};
                SDL_SetGPUViewport(pass, &v);
                std::copy(std::begin(vp), std::end(vp), bound_viewport);
            }
            draw(d, AlphaMode::kTexture, no_z, shadow_map);
        }
        end_pass();
        // FinishDrawTarget's downsamples, then submit (next_part): worth the
        // idle, since a frame submitted only at the end waited 1 ms longer
        // for its post frames' GPU (1.2 vs 2.2 ms p50)
        if (rt.levels > 1) {
            bool next = false;
            if (o.inline_mips && inline_mips_ok) {
                // same texels as SDL's blits, but through BeginPass so the
                // sampler heap stays on kSamplerBatch's step
                MarkTime(cmd, gpu_timing::kMips);
                DrawMips(cmd, rt.color, rt.w, rt.h, rt.levels);
                next = next_part();
            } else {
                // SDL's blits bind one sampler each, which would put the heap
                // off kSamplerBatch's step (BeginPass), so they get a command
                // buffer of their own, with fresh heaps
                if (next_part()) {
                    MarkTime(cmd, gpu_timing::kMips);
                    SDL_GenerateMipmapsForGPUTexture(cmd, rt.color);
                    next = next_part();
                }
            }
            if (!next) {
                REXLOG_WARN("native view gpu: a texture pass's mips didn't submit ({})",
                            SDL_GetError());
                return false;
            }
        }
    }
    if (!resolved && !resolve()) return false;
    end_pass();

    // The world ahead is submitted without a fence of its own: the next
    // Render follows on SDL's one queue and its fence covers both. Likewise
    // the pre passes; a fence polled from outside hung AMD GPUs
    // (RasterOptions::gpu_no_wait).
    if (ahead_pass) {
        if (o.gpu_labels) {
            std::lock_guard lock(draw_log_mutex);
            draw_log_frame = frame_label + " (world ahead)";
            draw_log = std::move(indexed_draws);
        }
        // ends the aside's ladder; the next post frame resolves it
        MarkTime(cmd, gpu_timing::kNone);
        st.record_ms = ms_since(record_start);
        const auto submit_start = Clock::now();
        if (!SDL_SubmitGPUCommandBuffer(cmd)) {
            REXLOG_WARN("native view gpu: the world ahead didn't submit ({})", SDL_GetError());
            return false;
        }
        add_submit(ms_since(submit_start));
        st.submits++;
        const auto evict_start = Clock::now();
        Evict();
        st.evict_ms = ms_since(evict_start);
        ahead = {serial, frame.game_frame, width, height};
        return true;
    }

    // the display's gamma ramp, as the presenter applies it (gamma_ramp.h):
    // a LUT entry per value, red in the low byte, four to a uint4. Every
    // frame ends in this pass into its output; without a ramp the LUT is the
    // identity
    MarkTime(cmd, gpu_timing::kGamma);
    {
        uint8_t lut[3][256];
        if (o.gamma && o.view == RasterView::kFinal && frame.gamma.mode != GammaRamp::kNone) {
            GammaLut(frame.gamma, lut);
        } else {
            for (int c = 0; c < 3; c++)
                for (int v = 0; v < 256; v++) lut[c][v] = uint8_t(v);
        }
        uint32_t packed[256];
        for (int v = 0; v < 256; v++)
            packed[v] =
                uint32_t(lut[0][v]) | uint32_t(lut[1][v]) << 8 | uint32_t(lut[2][v]) << 16;
        SDL_GPUColorTargetInfo ct{};
        ct.texture = output;
        ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* rp = BeginPass(cmd, &ct, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(rp, gamma_pipeline);
        const SDL_GPUTextureSamplerBinding tb{picture, sampler};
        SDL_BindGPUFragmentSamplers(rp, 0, &tb, 1);
        SDL_PushGPUFragmentUniformData(cmd, 0, packed, sizeof(packed));
        timed_first([&] { SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0); });
        SDL_EndGPURenderPass(rp);
    }

    // RenderFrame's is read back in the same submission; the presenter's
    // stays on the GPU
    if (rgba) {
        MarkTime(cmd, gpu_timing::kReadback);
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion src{};
        src.texture = output;
        src.w = width;
        src.h = height;
        src.d = 1;
        SDL_GPUTextureTransferInfo dst{readback, 0, width, height};
        SDL_DownloadFromGPUTexture(copy, &src, &dst);
        SDL_EndGPUCopyPass(copy);
    }
    // the last timestamp: a pre pass ends the aside's ladder; a frame
    // resolves its own with the aside's, read once it finishes (below, or
    // OutputDone)
    if (pre_pass) MarkTime(cmd, gpu_timing::kNone);
    ResolveTimes(cmd);
    if (o.gpu_labels) {
        std::lock_guard lock(draw_log_mutex);
        draw_log_frame = frame_label;
        draw_log = std::move(indexed_draws);
    }
    // With gpu_no_wait, pre passes and presenter frames aren't waited for:
    // later work on SDL's one queue follows them, and the presenter's caller
    // waits for the output's fence (OutputDone) before sampling it.
    // Otherwise, and for RenderFrame's readback, wait here.
    st.record_ms = ms_since(record_start);
    const auto submit_start = Clock::now();
    if (o.gpu_no_wait && (pre_pass || (slot >= 0 && !rgba))) {
        bool submitted;
        if (pre_pass) {
            submitted = SDL_SubmitGPUCommandBuffer(cmd);
        } else {
            SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            submitted = fence != nullptr;
            if (fence) {
                ReleaseFence(outputs[slot]);
                outputs[slot].fence = fence;
            }
        }
        if (!submitted) {
            REXLOG_WARN("native view gpu: the frame didn't submit ({})", SDL_GetError());
            return false;
        }
        add_submit(ms_since(submit_start));
        st.submits++;
        const auto evict_start = Clock::now();
        Evict();
        st.evict_ms = ms_since(evict_start);
        return true;
    }
    const auto submitted = std::chrono::steady_clock::now();
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) {
        REXLOG_WARN("native view gpu: the frame didn't submit ({})", SDL_GetError());
        return false;
    }
    add_submit(ms_since(submit_start));
    st.submits++;
    stall_watch::SetWorker(stall_watch::Worker::kGpuWait);
    const bool done = SDL_WaitForGPUFences(device, true, &fence, 1);
    stall_watch::SetWorker(stall_watch::Worker::kRecording);
    SDL_ReleaseGPUFence(device, fence);
    if (!done) {
        REXLOG_WARN("native view gpu: the frame didn't finish ({})", SDL_GetError());
        return false;
    }
    if (rgba) {
        const auto* px =
            static_cast<const uint32_t*>(SDL_MapGPUTransferBuffer(device, readback, false));
        if (!px) {
            REXLOG_WARN("native view gpu: couldn't read the frame back ({})", SDL_GetError());
            return false;
        }
        // alpha is the resolve's 0xff throughout
        rgba->resize(size_t(width) * height);
        std::memcpy(rgba->data(), px, rgba->size() * sizeof(uint32_t));
        SDL_UnmapGPUTransferBuffer(device, readback);
    }
    st.wait_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                           submitted).count();
    // a pre pass's timings are read with its frame's
    if (!pre_pass) ReadTimes(slot >= 0 ? slot : kOutputs, st);
    const auto evict_start = Clock::now();
    Evict();
    st.evict_ms = ms_since(evict_start);
    return true;
}

void GpuRenderer::Impl::Evict() {
    // gpu_view.h's residency: pool geometry stays keep_frames; arena
    // geometry kEvictAfter frames, or kKeepSeconds per ClockKeep. Arena
    // space comes back on a rebuild.
    meshes_by_time = textures_by_time = 0;
    for (auto it = meshes.begin(); it != meshes.end();) {
        const Mesh& m = it->second;
        if (KeepMesh(m.used, m.in_arena, serial, keep_frames, kEvictAfter, IdleSeconds(m.used_at),
                     KeepSecondsOf(m))) {
            if (m.used + kEvictAfter < serial) meshes_by_time++;
            ++it;
            continue;
        }
        it = meshes.erase(it);
        counts.evicted_meshes++;
    }
    // likewise textures (one kept by the clock goes sooner if its array is
    // full: PlaceTexture)
    for (auto it = textures.begin(); it != textures.end();) {
        const Tex& t = it->second;
        if (KeepTexture(t.first, t.used, serial, keep_frames, kEvictAfter,
                        IdleSeconds(t.used_at), KeepSecondsOf(t))) {
            if (t.used + kEvictAfter < serial) textures_by_time++;
            ++it;
            continue;
        }
        LetTextureGo(it->second);
        it = textures.erase(it);
        counts.evicted_textures++;
    }
    // a render target unused for kEvictAfter frames is forgotten (marked
    // undrawn, counted once) and its textures released kKeepSeconds after
    rts_forgotten.clear();
    for (auto it = rts.begin(); it != rts.end();) {
        Rt& rt = it->second;
        const RtResidency keep =
            KeepRt(rt.used, serial, kEvictAfter, IdleSeconds(rt.used_at), kKeepSeconds);
        if (keep == RtResidency::kKeep) {
            ++it;
            continue;
        }
        if (rt.drawn || rt.drawn_in) {
            rt.drawn = false;
            rt.drawn_in = 0;
            counts.evicted_rts++;
        }
        if (keep == RtResidency::kForget) {
            rts_forgotten.emplace_back(rt.used, it->first);
            ++it;
            continue;
        }
        ReleaseRt(rt);
        it = rts.erase(it);
        counts.rts_released++;
    }
    // past kMaxRts the least recently used forgotten ones go at once
    const size_t over = RtsOverCap(rts_forgotten, rts.size(), kMaxRts);
    for (size_t i = 0; i < over; i++) {
        const auto f = rts.find(rts_forgotten[i].second);
        ReleaseRt(f->second);
        rts.erase(f);
        counts.rts_released++;
    }
    // empty arrays go after kEvictAfter frames (one made ahead, unused, after
    // kArrayKeepSeconds)
    for (auto it = tex_arrays.begin(); it != tex_arrays.end();) {
        const TexArray& a = it->second;
        if (a.free.size() < a.layers || a.empty_since + kEvictAfter >= serial ||
            frame_now < a.ahead_until) {
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
    std::string refused;
    {
        std::lock_guard why_lock(impl_->zero_copy_mutex);
        refused = impl_->refused;
    }
    if (!refused.empty()) {
        REXLOG_WARN("native view gpu: not started, the native view draws on the CPU: {}",
                    refused);
        return false;
    }
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
    return Draw(frame, options, -1, &rgba, stats);
}

bool GpuRenderer::RenderFrameToOutput(const FrameCapture& frame, const RasterOptions& options,
                                      int slot, GpuOutput& out, GpuStats& stats) {
    if (slot < 0 || slot >= kOutputs) return false;
    std::lock_guard lock(impl_->mutex);
    if (!Draw(frame, options, slot, nullptr, stats)) return false;
    const Impl::Output& o = impl_->outputs[slot];
    out.d3d12_resource = o.resource;
    out.width = o.w;
    out.height = o.h;
    out.generation = o.generation;
    return true;
}

bool GpuRenderer::OutputDone(int slot, GpuStats* times) {
    if (slot < 0 || slot >= kOutputs) return true;
    std::lock_guard lock(impl_->mutex);
    Impl::Output& o = impl_->outputs[slot];
    if (!impl_->device || !o.fence) return true;
    if (!SDL_QueryGPUFence(impl_->device, o.fence)) return false;
    impl_->ReleaseFence(o);
    if (times) impl_->ReadTimes(slot, *times);
    return true;
}

bool GpuRenderer::RenderWorldAhead(const FrameCapture& world, const RasterOptions& options,
                                   GpuStats& stats) {
    std::lock_guard lock(impl_->mutex);
    // only after Draw has set up and warmed the device
    if (!impl_->device || !impl_->warm) return false;
    const auto start = std::chrono::steady_clock::now();
    stats = GpuStats{};
    RasterOptions o = options;
    o.self_shadow = options.self_shadow && impl_->shadow_maps;
    // its own world, not the post buffer a world frame shows
    o.post_buffer = false;
    if (!impl_->Render(world, o, -1, nullptr, stats, 0, true)) {
        REXLOG_WARN("native view gpu: the world ahead failed; giving up for this session, the "
                    "native view draws on the CPU");
        impl_->ready = false;
        impl_->Release(false);
        return false;
    }
    stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                   .count();
    return true;
}

bool GpuRenderer::DownloadOutput(int slot, std::vector<uint32_t>& rgba, uint32_t& width,
                                 uint32_t& height) {
    if (slot < 0 || slot >= kOutputs) return false;
    std::lock_guard lock(impl_->mutex);
    const Impl::Output& o = impl_->outputs[slot];
    if (!impl_->device || !o.texture) return false;
    if (!impl_->Download(o.texture, o.w, o.h, rgba)) return false;
    width = o.w;
    height = o.h;
    return true;
}

std::string GpuRenderer::DescribeIndexedDraw(uint32_t before, uint32_t total) {
    std::unique_lock lock(impl_->draw_log_mutex, std::try_to_lock);
    if (!lock) return "";
    const auto& log = impl_->draw_log;
    if (log.empty()) return "the native renderer kept no draws (gpu_labels off, or no frame yet)";
    // the last command buffer with that many indexed draws (the GPU got
    // furthest into it)
    for (size_t i = log.size(); i-- > 0;) {
        if (log[i].size() != total || before >= total) continue;
        return fmt::format("the native renderer's {}, command buffer {} of {}\n  its indexed "
                           "draw {} of {}: {}",
                           impl_->draw_log_frame, i + 1, log.size(), before, total,
                           log[i][before]);
    }
    std::string sizes;
    for (const auto& list : log) sizes += (sizes.empty() ? "" : ", ") + std::to_string(list.size());
    return fmt::format("not the native renderer's last frame, whose command buffers had {} "
                       "indexed draws: {}",
                       sizes, impl_->draw_log_frame);
}

void GpuRenderer::Prewarm(uint32_t overlay_samples) {
    // checked without waiting out a frame
    if (impl_->warm) return;
    std::lock_guard lock(impl_->mutex);
    if (impl_->device && !impl_->warm) impl_->Prewarm(overlay_samples);
}

bool GpuRenderer::Idle(const RasterOptions& options) {
    // only after Draw or Prewarm has set up the device
    if (!impl_->warm) return false;
    std::lock_guard lock(impl_->mutex);
    if (!impl_->device || !impl_->warmed_up) return false;
    impl_->WarmPools();
    return impl_->Premake(options);
}

void GpuRenderer::SetPresentDevice(void* d3d12_device, uint64_t timestamp_frequency) {
    std::lock_guard lock(impl_->mutex);
    impl_->present_device = d3d12_device;
    impl_->timestamp_frequency = timestamp_frequency;
}

void GpuRenderer::RefuseDevice(std::string why) {
    {
        std::lock_guard lock(impl_->zero_copy_mutex);
        impl_->refused = std::move(why);
    }
    // already made: a frame in progress finishes first; the next is drawn
    // on the CPU
    std::lock_guard lock(impl_->mutex);
    if (!impl_->device) return;
    impl_->ready = false;
    impl_->Release(true);
}

bool GpuRenderer::CheckZeroCopy(std::string& why) {
    if (!impl_->ready) {
        std::lock_guard lock(impl_->zero_copy_mutex);
        why = impl_->refused.empty() ? "no GPU device for the native view" : impl_->refused;
        return false;
    }
    if (!impl_->zero_copy_checked) {
        // once: waits out a frame being drawn
        std::lock_guard lock(impl_->mutex);
        if (impl_->device) impl_->CheckZeroCopyOnce();
    }
    std::lock_guard lock(impl_->zero_copy_mutex);
    why = impl_->zero_copy_why;
    return impl_->zero_copy;
}

// with the mutex held: RenderFrame's and RenderFrameToOutput's frame
bool GpuRenderer::Draw(const FrameCapture& frame, const RasterOptions& options, int slot,
                       std::vector<uint32_t>* rgba, GpuStats& stats) {
    if (!impl_->device) return false;
    // before the clock starts: device setup, not frame time
    if (!impl_->warm) impl_->Prewarm(OverlaySamples(options));
    const auto start = std::chrono::steady_clock::now();
    stats = GpuStats{};
    // without R32_FLOAT targets, SHADOW_BUFFER draws are lit, as on the CPU
    // without self_shadow
    RasterOptions o = options;
    o.self_shadow = options.self_shadow && impl_->shadow_maps;
    const Impl::Counts before = impl_->counts;
    if (!impl_->Render(frame, o, slot, rgba, stats)) {
        // it would fail every frame; the CPU takes over
        REXLOG_WARN("native view gpu: giving up for this session, the native view draws on "
                    "the CPU");
        impl_->ready = false;
        impl_->Release(false);
        SetKeepBlocks(false);
        SetKeepR8(false);
        return false;
    }
    // later captures keep BC textures as blocks and k_8 as bytes to match
    // (deferred_decode.h)
    SetKeepBlocks(impl_->bc_now);
    SetKeepR8(impl_->r8_now);
    stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                   .count();
    const Impl::Counts& now = impl_->counts;
    stats.pipelines_made = uint32_t(now.pipelines - before.pipelines);
    stats.buffers_made = uint32_t(now.buffers - before.buffers);
    stats.textures_made = uint32_t(now.textures - before.textures);
    stats.arena_rebuilt = now.arena_rebuilds != before.arena_rebuilds;
    stats.evicted_meshes = uint32_t(now.evicted_meshes - before.evicted_meshes);
    stats.evicted_textures = uint32_t(now.evicted_textures - before.evicted_textures);
    stats.evicted_rts = uint32_t(now.evicted_rts - before.evicted_rts);
    stats.rts_released = uint32_t(now.rts_released - before.rts_released);
    stats.textures_pressured = uint32_t(now.textures_pressured - before.textures_pressured);
    stats.meshes_pressured = uint32_t(now.meshes_pressured - before.meshes_pressured);
    stats.resident_meshes = uint32_t(impl_->meshes.size());
    stats.resident_textures = uint32_t(impl_->textures.size());
    stats.resident_rts = uint32_t(impl_->rts.size());
    stats.meshes_by_time = impl_->meshes_by_time;
    stats.textures_by_time = impl_->textures_by_time;
    for (const auto& [obj, rt] : impl_->rts) {
        // colour (RGBA8, or a shadow map's R32_FLOAT) and D32 depth, a
        // shared one counted once (below)
        const double pixels = double(rt.w) * rt.h;
        stats.rts_mb +=
            pixels * 4 * ((rt.levels > 1 ? 4.0 / 3 : 1) + (rt.depth_shared ? 0 : 1)) / 1048576;
        if (rt.premade) stats.premade_unused++;
    }
    for (const auto& [size, d] : impl_->shared_depths)
        stats.rts_mb += double(size >> 32) * double(size & 0xffffffffu) * 4 / 1048576;
    stats.shared_depths = uint32_t(impl_->shared_depths.size());
    // made ahead since the last frame (Idle)
    stats.targets_premade = std::exchange(impl_->premade_since, 0);
    stats.premake_ms = std::exchange(impl_->premade_ms_since, 0.0);
    stats.arrays_premade = std::exchange(impl_->arrays_premade_since, 0);
    stats.texture_array_mb = impl_->TextureArrayMb();
    stats.arena_mb = double(impl_->arena_verts.size + impl_->arena_indices.size) / 1048576;
    return true;
}

void GpuRenderer::Shutdown() {
    std::lock_guard lock(impl_->mutex);
    impl_->ready = false;
    impl_->Release(true);
}

}  // namespace band3::render
