#include "src/Render/gpu_view.h"

#include "src/Render/frame_compose.h"
#include "src/Render/gamma_ramp.h"
#include "src/Render/post_model.h"
#include "src/Render/sample_model.h"
#include "src/Render/shade_model.h"
#include "src/Render/spot_model.h"
#include "src/Render/shaders/gamma_shaders.gen.h"
#include "src/Render/shaders/mesh_shaders.gen.h"
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
// a mesh or texture no frame has drawn for this many frames, and kKeepSeconds,
// is let go (a render target's picture is forgotten at the frames alone)
constexpr uint64_t kEvictAfter = 120;
// the seconds unused after which, with kEvictAfter frames, a forgotten render
// target's textures are let go, and in a song a mesh in the arena and a
// texture drawn in more than one frame (by frames of more than one world:
// ClockKeep); render targets too past kMaxRts kept, least recently used first
// (gpu_view.h's residency; GpuStats::rts_mb is what they hold)
constexpr double kKeepSeconds = 30;
constexpr size_t kMaxRts = 128;
// over this many bytes an arena rebuild keeps less than the clock would
// (gpu_view.h's residency, ArenaRebuildKeep); a song's arena is 24 to 34 MB
constexpr uint64_t kMaxArenaBytes = 256u << 20;
// A texture array of a size class starts with layers to about this many
// bytes, and doubles when full. Making a texture costs about half a
// millisecond, so textures share them rather than have one each.
constexpr uint32_t kTextureArrayBytes = 4u << 20;
constexpr uint32_t kMaxTextureLayers = 2048;  // Direct3D 12's limit
// Direct3D 12 copies texture rows from an upload at this pitch, starting at an
// offset aligned to kTextureOffsetAlign; textures are laid out so
constexpr uint32_t kRowPitchAlign = 256;
constexpr uint32_t kTextureOffsetAlign = 512;
// the arena's least size; it's made twice as big as it needs
constexpr uint32_t kMinArenaBytes = 4u << 20;
// the upload buffer's first size, which holds a song's usual frame: making it
// bigger later costs a frame several milliseconds
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
    // RasterOptions::overlay_edge (x, y, then 0 0) for an overlay draw over
    // the whole picture; 1 1 for every other
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
// half a pixel right and down, so that the pixel SDL_gpu samples at its
// centre, x + .5, sees what the game's device sampled at x, on D3D9's pixel
// centres; none for DrawRect's quads, which RB3 draws on D3D10's
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
    // each one's own width and height: the five before the normal map, then
    // the shadow map's, the normal map's and the detail map's
    uint32_t tex_size[8][4];
    uint32_t flags[4];  // x: kPremultiply
    // the samplers they're read with (sample_model.h's PackSampler), in
    // tex_layer's order
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

// the textures a draw samples, in mesh.hlsl's sampler order: the maps, which
// PixelUniforms sizes, then the picture behind (kShadeRefract), the shadow
// map (kShadeShadow, sized after the maps) and the normal map and the detail
// map (kShadeNormalMap, kShadeDetailMap, sized after the shadow map); a
// spotlight's cone reads two more, the scene's depth and the density map
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

// What a draw's pipeline does with its target's alpha: leaves it (the
// picture's stays the resolve's 1, and a world draw that doesn't write it
// keeps the scene's), blends it by the colour's factors (into a texture), or
// as RB3's back buffer does (WritesSceneAlpha: ONE ONE MAX where it blends)
enum class AlphaMode { kNone, kTexture, kScene };
constexpr int kNumAlphaModes = 3;

// which of mesh.hlsl's pixel shaders a draw's pipeline runs: PSMain,
// PSSpotCone for a spotlight's cone, PSSoftParticle for a soft particle,
// PSShadowDepth for a shadow map's draw (into its R32_FLOAT target, LESS
// against its depth cleared to the pass's clear_z, no blend)
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

// how many of a texture's levels the GPU gets: level 0 and the mips after it
// that are the size they should be
uint32_t LevelsOf(const Texture& t) {
    uint32_t n = 1;
    for (const auto& level : t.mips) {
        const size_t texels =
            size_t(std::max(t.width >> n, 1u)) * std::max(t.height >> n, 1u);
        if (level.size() != texels) break;
        n++;
    }
    return n;
}

// index counts are kept even, so every copy of indices is whole 4-byte words
uint32_t IndexSlots(const Geometry& g) { return Align(uint32_t(g.indices.size()), 2); }

uint32_t NextPow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

// A texture's size class, the layer size of the array it goes in: powers of
// two that hold it, at least kMinClassSize and at most 2:1, so a frame's
// textures need few arrays. An array has every level of its class's chain;
// a texture's levels go in its layer's corner of each, as its level 0 does.
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
    // mesh.hlsl's PSSpotCone, for the spotlights' cones, PSSoftParticle, for
    // the soft particles, and PSShadowDepth, for the shadow maps
    SDL_GPUShader* spot_shader = nullptr;
    SDL_GPUShader* soft_shader = nullptr;
    SDL_GPUShader* shadow_shader = nullptr;
    // whether the device draws into and samples R32_FLOAT, the shadow maps'
    // format; without, the characters are drawn without their self-shadows
    bool shadow_maps = false;
    // post.hlsl's: the full-screen triangle; the resolve, the scene into the
    // picture as it is; and post-processing's downsample, blur, glare pass
    // and composite
    SDL_GPUShader* fullscreen_shader = nullptr;
    SDL_GPUShader* resolve_shader = nullptr;
    // the overlay's start in its multisampled target: the picture copied
    // into every sample, and its depth (PSOverlayStart)
    SDL_GPUShader* overlay_start_shader = nullptr;
    SDL_GPUShader* downsample_shader = nullptr;
    SDL_GPUShader* blur_shader = nullptr;
    SDL_GPUShader* glare_shader = nullptr;
    SDL_GPUShader* composite_shader = nullptr;
    // the live view's composite, which also keeps the post buffer the
    // trails read (PSCompositeHistory: two targets)
    SDL_GPUShader* composite_history_shader = nullptr;
    // the camera motion blur's velocity pass (PSVelocity), and its object
    // pass (velocity.hlsl): a pipeline for each way it culls (CullWinding's
    // first three)
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
    // by blend mode, DepthRules::Key, AlphaMode, CullWinding, PixelKind and
    // sample count, all made before the first frame; and the overlay's
    // start's, by sample count (OverlayStartPipeline)
    std::unordered_map<int, SDL_GPUGraphicsPipeline*> pipelines;
    // Prewarm has started (read without the mutex by GpuRenderer::Prewarm),
    // and has finished: a pipeline made after it is one it doesn't make,
    // which a frame waited for (logged)
    std::atomic<bool> warm{false};
    bool warmed_up = false;
    // RasterOptions::gpu_labels: the last frame submitted, and what each of
    // its indexed draws is, in order, a list per command buffer it took
    // (DescribeIndexedDraw)
    std::mutex draw_log_mutex;
    std::string draw_log_frame;
    std::vector<std::vector<std::string>> draw_log;
    SDL_GPUSampler* sampler = nullptr;
    // linear and clamping, as RB3 samples its post-processing levels
    SDL_GPUSampler* linear_sampler = nullptr;
    SDL_GPUTexture* white = nullptr;     // bound for untextured draws, which don't read it
    SDL_GPUTexture* black = nullptr;     // transparent: a render target nothing has drawn
    SDL_GPUBuffer* no_bones = nullptr;   // one identity bone, bound when nothing is skinned

    // The world's draws go to the scene target (its alpha the bloom weight),
    // which the resolve reads into the picture, `color`; the overlay's go on
    // top of that. The depth buffer is both's, and readable by the resolve
    // where the device can sample D32 (depth_sampled); where it can't, the
    // resolve reads no_depth, which is 0 (nothing drew), and post-processing
    // leaves depth of field out.
    SDL_GPUTexture* scene = nullptr;
    SDL_GPUTexture* color = nullptr;
    SDL_GPUTexture* depth = nullptr;
    // The overlay's multisampled targets (soft_raster.h's OverlaySamples),
    // ms_samples a pixel: its colour, which each of its passes resolves into
    // `color` as it ends (the mean of each pixel's samples, as RB3's
    // EndTiling), and its own depth, so the world's stays as the world left
    // it. Render targets alone: SDL can't sample a multisampled texture.
    // Made at the picture's size when a frame first draws an overlay.
    SDL_GPUTexture* color_ms = nullptr;
    SDL_GPUTexture* depth_ms = nullptr;
    uint32_t ms_samples = 1, ms_w = 0, ms_h = 0;
    // whether the device draws kColorFormat and kDepthFormat multisampled,
    // [0] at 2 samples and [1] at 4; and the counts asked for that it
    // couldn't, logged once each, and whether making the targets failed
    // (logged once)
    bool ms_supported[2] = {};
    uint32_t ms_fallback_logged = 0;
    bool ms_failure_logged = false;
    // a copy of `color` as the resolve left it, for the overlay's
    // REFRACT_WORLD draws (RefractsWorld), made on frames that have one
    SDL_GPUTexture* behind = nullptr;
    // RenderFrame's output: the finished picture through the frame's gamma
    // ramp (or the identity), which it reads back
    SDL_GPUTexture* graded = nullptr;
    bool depth_sampled = false;
    SDL_GPUTexture* no_depth = nullptr;
    SDL_GPUTransferBuffer* readback = nullptr;
    uint32_t width = 0, height = 0;
    // post-processing's levels (post_model.h), RGBA8 as the 360's: the DOF's
    // at a quarter of the picture's size, bloom's at a quarter, a sixteenth
    // and a sixty-fourth, and one of each size for a blur's first direction;
    // and the camera motion blur's velocity texture, half the picture's size
    SDL_GPUTexture* post_dof = nullptr;
    SDL_GPUTexture* post_velocity = nullptr;
    uint32_t velocity_w = 0, velocity_h = 0;
    // the object pass's depth, at the velocity texture's size
    SDL_GPUTexture* post_velocity_depth = nullptr;
    // where each of the frame's velocity objects' palettes start in the
    // frame's bones (its two, this frame's then the last's)
    std::vector<uint32_t> velocity_bone_base;
    SDL_GPUTexture* post_bloom[3] = {};
    SDL_GPUTexture* post_tmp[3] = {};
    uint32_t post_w[3] = {}, post_h[3] = {};
    // a texture pass's target copied for a blur to read, RGBA8 at its size, a
    // plain 2D texture as the blur's source is (a target is an array of one
    // layer): the depth volume as it was before a blur (the game blurs it in
    // place, through a resolve), NgLight's shadow likewise, and the
    // soft-particle surface a blur reads into the other
    struct Scratch {
        SDL_GPUTexture* texture = nullptr;
        uint32_t w = 0, h = 0;
    };
    // and the scene a world pass left (soft_raster.h's kPreBufferPasses),
    // which the next one's REFRACT_WORLD draws read, and the frame's after
    // them
    Scratch spot_scratch, light_scratch, soft_scratch, pre_scratch;
    // The post buffer the trails read (RasterOptions::trails, the live
    // view's): the last post frame's composite, its colour and alpha, at the
    // picture's size, in tex[cur] (-1 none yet), from game frame game_frame;
    // the composite of a post frame writes the other and makes it cur.
    // Apart from the frame's targets, so a capture drawn at another size in
    // between leaves it be.
    struct History {
        SDL_GPUTexture* tex[2] = {};
        uint32_t w = 0, h = 0;
        int cur = -1;
        uint64_t game_frame = 0;
    };
    History history;
    // The post buffer as the screen shows it (RasterOptions::post_buffer,
    // the live view's): the last post frame's picture before its overlay, at
    // the picture's size, from game frame game_frame (0 none), which the
    // frames after it that post-process nothing show under their own
    // overlay. Apart from the frame's targets, as the history is.
    struct PostBuffer {
        SDL_GPUTexture* tex = nullptr;
        uint32_t w = 0, h = 0;
        uint64_t game_frame = 0;
    };
    PostBuffer post_buffer;
    // The pre-process buffer (RasterOptions::pre_buffer, the live view's):
    // the last world frame's scene before post-processing, at the picture's
    // size, from the world of game frame game_frame (0 none), kept where that
    // world has a REFRACT_WORLD draw, which the next one's read. Apart from
    // the frame's targets, as the post buffer is.
    PostBuffer pre_buffer;

    // The presenter's outputs (RenderFrameToOutput), each at the size it was
    // last drawn at: apart from the frame's targets, so a frame drawn at
    // another size (a capture's) leaves them be. COLOR_TARGET and SAMPLER,
    // which SDL leaves in ALL_SHADER_RESOURCE after its passes, so the SDK's
    // command list samples one without a barrier. Never cycled: SDL's texture
    // behind each stays the one that was checked. `fence` is the submission
    // of the frame last drawn into it until OutputDone finds it signalled.
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
    // RefuseDevice's why, which keeps Init from making the device (under
    // zero_copy_mutex)
    std::string refused;

    // GPU timings (RasterOptions::gpu_timestamps; gpu_view.h, and
    // gpu_timing_model.h): the SDK's direct queue's ticks a second
    // (SetPresentDevice), and whether SDL's command list passed
    // CheckTimingOnce's checks (once: logged if not), which made the query
    // heap and its readback buffer on the SDK's device. A heap region of
    // kTimingSlots timestamps per frame being drawn: each output's, RenderFrame's
    // (kOutputs), and the aside's (kAsideRegion), where a world pass before a
    // frame or the world drawn ahead, submitted without a fence of their own,
    // mark theirs until the next whole frame resolves them with its own. The
    // readback buffer has two regions' room for each frame's (its own, then
    // the aside's), so neither is written over before it's read: an output's
    // isn't drawn again until its fence was seen (native_view.cpp's
    // PresentSlots), and RenderFrame's is read before it returns.
    static constexpr uint32_t kTimingSlots = 256;
    static constexpr int kFrameRegions = kOutputs + 1;
    static constexpr int kAsideRegion = kOutputs + 1;
    uint64_t timestamp_frequency = 0;
    bool timing_checked = false, timing_ok = false;
#ifdef _WIN32
    ID3D12QueryHeap* query_heap = nullptr;
    ID3D12Resource* query_readback = nullptr;
#endif
    // the frame being recorded's ladder, and the aside's; the region it marks
    // (-1 untimed: off, or its command buffer failed the checks), and whether
    // it marks the aside (a world pass before a frame, or the world ahead)
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
    // copy pass. It's mapped cycling, so a frame never waits on an earlier one
    // still reading it.
    SDL_GPUTransferBuffer* upload = nullptr;
    uint32_t upload_size = 0;

    struct Buffer {
        SDL_GPUBuffer* buffer = nullptr;
        uint32_t size = 0;
    };
    // Geometry drawn in more than one frame lives in the arena, appended to
    // and, when full, rebuilt from the meshes still within their keep, copied
    // from the old one on the GPU (PlaceInArena). Geometry new this frame
    // goes in the frame's pool: mutable meshes and particles are new every
    // frame, and a buffer each costs far more than copying them. Neither
    // makes a GPU buffer per mesh. The pool alternates between two buffers,
    // so geometry the next frame draws again moves to the arena by a copy on
    // the GPU rather than being sent again.
    Buffer arena_verts, arena_indices;
    uint32_t arena_vert_count = 0, arena_index_count = 0;
    // the arena a rebuild this frame replaced, which its copy pass copies
    // out of and then releases: SDL_ReleaseGPUBuffer's handle mustn't be
    // used after, though SDL keeps the buffer until the GPU is done with it
    Buffer old_arena_verts, old_arena_indices;
    Buffer pool_verts[2], pool_indices[2];  // by frame serial & 1
    Buffer bones;  // this frame's skinned draws' bones, one after another

    struct Mesh {
        std::shared_ptr<const Geometry> keep;  // so the key stays this geometry's
        uint64_t first = 0;                    // the frame that first drew it
        // the frame that last drew it, and when (frame_now)
        uint64_t used = 0;
        std::chrono::steady_clock::time_point used_at;
        // the world of the frame that first drew it (frame_world), whether
        // frames of another have drawn it too, and whether the frame that
        // last drew it was in a song (ClockKeep)
        uint64_t first_world = 0;
        bool across_worlds = false;
        bool drawn_in_song = false;
        bool in_arena = false;
        // where it starts in the arena, or else in its frame's pool
        uint32_t first_vertex = 0;
        uint32_t first_index = 0;
        // when moving to the arena: where it was in the last frame's pool,
        // or where it was in the old arena (kept by a rebuild); with neither
        // (~0u) it's sent from the CPU
        uint32_t pool_vertex = ~0u;
        uint32_t pool_index = 0;
        uint32_t arena_vertex = ~0u;
        uint32_t arena_index = 0;
        bool FromCpu() const { return pool_vertex == ~0u && arena_vertex == ~0u; }
    };
    std::unordered_map<const Geometry*, Mesh> meshes;

    // the textures of a size class, a layer each
    struct TexArray {
        SDL_GPUTexture* texture = nullptr;
        uint32_t layers = 0;
        uint32_t levels = 1;  // its class's whole chain
        std::vector<uint32_t> free;
        uint64_t empty_since = 0;  // when its last texture went, if none are left
    };
    std::unordered_map<uint64_t, TexArray> tex_arrays;
    struct Tex {
        std::shared_ptr<const Texture> keep;
        TexArray* array = nullptr;  // null if it couldn't have a layer
        uint32_t layer = 0;
        uint32_t levels = 1;  // of the texture's, in it (LevelsOf)
        uint64_t first = 0;
        // the frame that last drew it, and when (frame_now); as a Mesh's
        uint64_t used = 0;
        std::chrono::steady_clock::time_point used_at;
        uint64_t first_world = 0;
        bool across_worlds = false;
        bool drawn_in_song = false;
    };
    std::unordered_map<const Texture*, Tex> textures;
    uint64_t serial = 0;
    bool texture_failure_logged = false;

    // A texture pass's target, by DxTex: RGBA8 with the texture's mips, a 2D
    // array of one layer so draws sample it through the same binding as any
    // texture, and a depth buffer; a shadow map's is R32_FLOAT, a plain 2D
    // texture (mesh.hlsl's shadow_tex), its depth alone. Kept between frames,
    // so a render target a frame samples but doesn't draw is what was drawn
    // last (one it draws starts over: Render); forgotten when no frame has
    // drawn or sampled it for kEvictAfter frames, and released kKeepSeconds
    // after (Evict, gpu_view.h's residency); made again when its size
    // changes.
    struct Rt {
        SDL_GPUTexture* color = nullptr;
        SDL_GPUTexture* depth = nullptr;
        uint32_t w = 0, h = 0, levels = 1;
        // its pass's size in the game, which w x h is unless the pass is
        // drawn bigger (soft_raster.h's PassTargetSize)
        uint32_t game_w = 0, game_h = 0;
        bool shadow = false;    // a shadow map's
        bool drawn = false;     // by a pass, this frame or before
        uint64_t drawn_in = 0;  // the frame a pass last drew it
        uint32_t version = 0;   // the version that pass made
        // the frame that last drew or sampled it, and when (frame_now)
        uint64_t used = 0;
        std::chrono::steady_clock::time_point used_at;
    };
    std::unordered_map<uint32_t, Rt> rts;
    bool rt_failure_logged = false;
    // the DxTexes a target has been made for since the device started, so
    // one made again after it was released counts as returning
    // (GpuStats::targets_returning)
    std::unordered_set<uint32_t> rts_seen;
    // the frame's time, taken once as its walk starts: what marks a target,
    // a mesh or a texture used (used_at), and what Evict measures their idle
    // seconds from
    std::chrono::steady_clock::time_point frame_now;
    // and its world's game frame (FrameCapture::world_frame), what tells
    // meshes and textures the capture keeps between game frames, and whether
    // the game is in a song (RasterOptions::clock_keep; ClockKeep)
    uint64_t frame_world = 0;
    bool frame_in_song = false;
    // the seconds `m` (a Mesh or Tex) is kept by after this frame
    template <typename T>
    double KeepSecondsOf(const T& m) const {
        return ClockKeep(frame_in_song, m.drawn_in_song, m.across_worlds, kKeepSeconds);
    }
    // seconds since `used_at`, at frame_now
    double IdleSeconds(std::chrono::steady_clock::time_point used_at) const {
        return std::chrono::duration<double>(frame_now - used_at).count();
    }
    // Evict's count of the meshes and textures it kept by the clock alone
    // (GpuStats::meshes_by_time and textures_by_time)
    uint32_t meshes_by_time = 0, textures_by_time = 0;
    // Evict's forgotten targets (used, DxTex) for kMaxRts, kept between
    // frames so it allocates nothing once grown
    std::vector<std::pair<uint64_t, uint32_t>> rts_forgotten;

    // a frame's work, kept between frames so a frame allocates nothing once
    // they've grown
    std::vector<Mesh*> to_pool, to_arena;
    std::vector<Tex*> new_textures;
    // An array that grew: the layers of the old one holding textures sent in
    // an earlier frame, each with its levels written (Tex::levels), go over
    // to the new one. Nothing else is copied, as nothing else was written: a
    // layer placed this frame is sent to the new array, and an array made
    // this frame (more of a size arriving at once than it holds) holds
    // nothing yet.
    struct ArrayCopy {
        SDL_GPUTexture* from;
        SDL_GPUTexture* to;
        uint32_t w, h;
        std::vector<std::pair<uint32_t, uint32_t>> layers;  // layer, levels
    };
    std::vector<ArrayCopy> array_copies;  // arrays that grew, old into new
    std::vector<Mat4> frame_bones;
    std::vector<uint32_t> bone_base;  // per draw, where its bones start
    std::vector<shade::ShadeParams> shades;  // per draw
    std::vector<uint32_t> cams_seen;
    // per draw, what its diffuse texture samples, and its projected light's
    // s5 (kSourceNone the capture's map, or none)
    enum Source : uint8_t { kSourceNone, kSourceTexture, kSourceRt, kSourceBlack };
    std::vector<uint8_t> diffuse_source;
    std::vector<uint8_t> proj_source;
    // the normal map's and the detail map's (MapTargetOf: a head's normal map
    // is a target's), kSourceTexture or kSourceRt
    std::vector<uint8_t> normal_source[2];
    // per draw, a spotlight drawer's: a cone (PSSpotCone), one left out (no
    // scene depth to read), or a blur of the depth volume into itself; or
    // the soft-particle buffer's: a particle (PSSoftParticle), or a blur of
    // one of its surfaces into the other
    enum SpotDraw : uint8_t {
        kSpotNone,
        kSpotCone,
        kSpotConeSkipped,
        kSpotBlur,
        kSoftParticle,
        kSoftBlur
    };
    std::vector<uint8_t> spot_draw;
    // per run, a texture pass's: drawn (it has a target), and whether that
    // starts cleared
    enum : uint8_t { kRunDrawn = 1, kRunClearColor = 2, kRunClearDepth = 4 };
    std::vector<uint8_t> run_clear;
    // Device objects made and things let go of since the device started,
    // which Draw turns into a frame's (GpuStats::pipelines_made and the rest)
    struct Counts {
        uint64_t pipelines = 0, buffers = 0, textures = 0, arena_rebuilds = 0;
        uint64_t evicted_meshes = 0, evicted_textures = 0, evicted_rts = 0, rts_released = 0;
        uint64_t textures_pressured = 0, meshes_pressured = 0;
    };
    Counts counts;
    // the frame whose walk is placing what it draws (Render), which
    // TargetFor and PlaceTexture count what they make into
    // (GpuStats::targets_made and the rest); null outside it, so a world
    // pass before a frame counts into its own
    GpuStats* walk_stats = nullptr;

    bool StartVideo(const char* driver);
    void StopVideo();
    bool Create();
    // stop_video: on the UI thread only, as SDL wants
    void Release(bool stop_video);
    // SDL's Direct3D 12 backend copies a pipeline's fragment samplers into
    // its command buffer's GPU sampler heap (2048 of them) as one batch each
    // time they're bound again, checking for room before the batch only and
    // skipping null slots without counting them (SDL 3.4, and main as of
    // 2026-10: "FIXME: need to error on overflow"). A batch that starts short
    // of the heap's end runs past it: descriptors copied outside any heap
    // (the debug layer's INVALID_DESCRIPTOR_HANDLE), which the GPU then reads,
    // and AMD GPUs hung on the next draw (DEVICE_HUNG). So every fragment
    // shader that samples declares kSamplerBatch samplers (MakeShader) and
    // every pass binds all of them first (BeginPass): each batch is
    // kSamplerBatch, which divides the heap, so batches end on its end
    // exactly, where SDL moves to a fresh heap (the view heap with it).
    static constexpr uint32_t kSamplerBatch = 16;
    static_assert(2048 % kSamplerBatch == 0, "a batch divides SDL's sampler heap");
    // a render pass, its kSamplerBatch fragment samplers bound to white first
    SDL_GPURenderPass* BeginPass(SDL_GPUCommandBuffer* cmd, const SDL_GPUColorTargetInfo* ct,
                                 uint32_t targets, const SDL_GPUDepthStencilTargetInfo* dt);
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
    // the overlay's samples for a frame that asks for `want` (OverlaySamples):
    // `want` if the device can, else the other of 2 and 4, else 1 (logged once)
    uint32_t DeviceSamples(uint32_t want);
    // the overlay's targets at the picture's size and `samples`; false if
    // they couldn't be made (logged)
    bool EnsureOverlayTargets(uint32_t samples);
    // a shadow map's draw's: PSShadowDepth into R32_FLOAT, LESS, no blend
    SDL_GPUGraphicsPipeline* ShadowDepthPipeline(CullWinding cull) {
        return Pipeline(kBlendSrc, {true, false, true}, AlphaMode::kNone, cull,
                        PixelKind::kShadowDepth);
    }
    // a full-screen pass's pipeline: post.hlsl's triangle and `pixel`, into RGBA8
    SDL_GPUGraphicsPipeline* MakeFullscreenPipeline(SDL_GPUShader* pixel, const char* name,
                                                    uint32_t targets = 1);
    // the motion blur's object pass's (velocity.hlsl), culling as `cull`
    // says: into the velocity texture, SrcAlpha (alpha 1 or 0), its depth
    // smaller-or-equal tested and written
    SDL_GPUGraphicsPipeline* MakeVelocityObjectPipeline(CullWinding cull);
    bool EnsureHistory(uint32_t w, uint32_t h);
    void ReleaseHistory();
    // the post buffer or the pre-process buffer `b` at w x h, emptied
    // (game_frame 0) if it's made again
    bool EnsureKept(PostBuffer& b, uint32_t w, uint32_t h);
    void ReleaseKept(PostBuffer& b);
    void ReleaseTargets();
    // makes every pipeline a frame can ask for and the upload buffer's usual
    // size, so no frame stalls making them: the overlay's at overlay_samples
    // (RasterOptions::msaa) as the device draws them (DeviceSamples)
    void Prewarm(uint32_t overlay_samples);
    bool EnsureTargets(uint32_t w, uint32_t h);
    // `s` at w x h; false if it couldn't be made
    bool EnsureScratch(Scratch& s, uint32_t w, uint32_t h);
    // grows `b` to hold `bytes`; what it held is lost when it grows
    bool Reserve(Buffer& b, SDL_GPUBufferUsageFlags usage, uint32_t bytes);
    void ReleaseBuffer(Buffer& b);
    // places this frame's new arena meshes, rebuilding the arena if they
    // don't fit (with what it keeps added to to_arena, from the old arena);
    // false if it couldn't grow
    bool PlaceInArena();
    // marks `t` drawn this frame, giving it a layer and queueing its upload
    // if it's new
    void UseTexture(const std::shared_ptr<const Texture>& t);
    // the layer `t` has, or null (none, or it couldn't have one)
    const Tex* TextureFor(const Texture* t);
    // a layer of its size class's array for `tx`, growing the array if full
    // and none of its textures can go for room
    bool PlaceTexture(Tex& tx);
    void LetTextureGo(Tex& tx);
    // the texture arrays' MB, their mips counted as a third more
    // (GpuStats::texture_array_mb)
    double TextureArrayMb() const;
    // the target for texture pass `p`, made (again) at w x h (PassTargetSize);
    // null if it couldn't be
    Rt* TargetFor(const Pass& p, uint32_t w, uint32_t h);
    // marks `rt` drawn or sampled by this frame, for Evict
    void UseRt(Rt& rt) {
        rt.used = serial;
        rt.used_at = frame_now;
    }
    void ReleaseRt(Rt& rt);
    // output `slot` at w x h, made again (a new generation) if it isn't;
    // false if it couldn't be
    bool EnsureOutput(int slot, uint32_t w, uint32_t h);
    void ReleaseOutputs();
    // lets go of `out`'s fence, signalled or not (SDL's own reference keeps
    // it until its submission is done)
    void ReleaseFence(Output& out);
    // the ID3D12Resource behind `texture`, made with `info`, if the SDK's
    // presenter can sample it in place; null, and why not, otherwise
    void* SdkResource(SDL_GPUTexture* texture, const SDL_GPUTextureCreateInfo& info,
                      std::string& why);
    // CheckZeroCopy's first check, with a texture of its own
    void CheckZeroCopyOnce();
    // a zero-copy check's result, for CheckZeroCopy (the presenter's drawer
    // logs a change)
    void SetZeroCopy(bool ok, const std::string& why);
    // SDL's ID3D12GraphicsCommandList behind `cmd` (as void*), if the
    // pointers of SDL 3.4.14's command buffer layout check out (no call made
    // on any of them), else null
    void* TimingList(SDL_GPUCommandBuffer* cmd);
    // whether GPU timings can be taken in `cmd`: the first time, the whole of
    // the checks on it (its command list's and allocator's interfaces, the
    // list's type and device), and the query heap and readback buffer made;
    // after, whether they passed then and `cmd`'s pointers check out now
    bool CheckTimingOnce(SDL_GPUCommandBuffer* cmd);
    // A Render's timing, its command buffer just acquired: untimed without
    // RasterOptions::gpu_timestamps or if the checks fail; a world pass
    // before a frame (`pre_pass`) or the world ahead (`ahead_pass`) on the
    // aside's ladder, as one part each; a frame on its own region's (`slot`'s,
    // or RenderFrame's), from kUpload.
    void StartTiming(SDL_GPUCommandBuffer* cmd, const RasterOptions& o, int slot, int pre_pass,
                     bool ahead_pass);
    // a timestamp in `cmd` from which the GPU's time goes to `part`, if the
    // frame is timed and the part changes (gpu_timing::Ladder::Mark); on the
    // aside's ladder, only its start and its end (kNone)
    void MarkTime(SDL_GPUCommandBuffer* cmd, uint8_t part);
    // a frame's last command buffer, before it's submitted: its ladder ended
    // and resolved into its region of the readback buffer, and the aside's
    // with it (then started over)
    void ResolveTimes(SDL_GPUCommandBuffer* cmd);
    // once the GPU has finished frame region `region`'s frame: its times into
    // `st` (GpuStats::gpu_ms), if it has any not read yet
    void ReadTimes(int region, GpuStats& st);
    void ReleaseTiming();
    // Draws `frame` into output `slot`, or with -1 into `graded`, which it
    // reads back into rgba. With `pre_pass` (from 1) it's that one of the
    // world passes before a frame whose world refracts (soft_raster.h's
    // kPreBufferPasses): its world alone, which leaves its scene in
    // pre_scratch, its REFRACT_WORLD draws reading the last pass's there (the
    // first black). With `ahead_pass` (RenderWorldAhead) it's a world
    // frame's world alone, left in the scene target and submitted unwaited.
    bool Render(const FrameCapture& frame, const RasterOptions& o, int slot,
                std::vector<uint32_t>* rgba, GpuStats& stats, int pre_pass = 0,
                bool ahead_pass = false);
    // The world RenderWorldAhead left in the scene target (and its depth):
    // drawn as frame `serial` (0 none), of game frame world_frame, at w x h.
    // Every Render forgets it as it starts, so only the one right after it
    // can post-process it: one in between (a capture's, say) draws over the
    // scene target, or may.
    struct Ahead {
        uint64_t serial = 0, world_frame = 0;
        uint32_t w = 0, h = 0;
    };
    Ahead ahead;
    // reads `texture` (w x h) back into rgba; false if the GPU failed (logged)
    bool Download(SDL_GPUTexture* texture, uint32_t w, uint32_t h, std::vector<uint32_t>& rgba);
    // lets go of what no frame has drawn for long enough (gpu_view.h's
    // residency), after each frame
    void Evict();
    // the frames geometry and textures drawn in one frame are kept for
    // (ResidencyKeepFrames), from the frame's RasterOptions::world_period
    uint64_t keep_frames = 0;
};

// SDL_CreateGPUDevice wants a video subsystem in band3's SDL copy, which is
// otherwise used for audio and HID only
bool GpuRenderer::Impl::StartVideo(const char* driver) {
    if (driver) {
        // over an SDL_VIDEO_DRIVER in the environment, which would otherwise
        // win and could make this copy open windows
        SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, driver, SDL_HINT_OVERRIDE);
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

SDL_GPUGraphicsPipeline* GpuRenderer::Impl::MakeFullscreenPipeline(SDL_GPUShader* pixel,
                                                                    const char* name,
                                                                    uint32_t targets) {
    SDL_GPUColorTargetDescription target[2]{};
    target[0].format = target[1].format = kColorFormat;
    SDL_GPUGraphicsPipelineCreateInfo pi{};
    pi.vertex_shader = fullscreen_shader;
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
    // the scene's depth, read after the world's draws: D32 the resolve samples
    // where the device can (Direct3D 12 and Vulkan both should)
    depth_sampled = SDL_GPUTextureSupportsFormat(
        device, kDepthFormat, SDL_GPU_TEXTURETYPE_2D,
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!depth_sampled)
        REXLOG_WARN("native view gpu: the device can't sample a D32 depth buffer; the scene's "
                    "depth reads as 0, post-processing has no depth of field, the "
                    "spotlights' cones aren't drawn and the soft particles don't fade");
    // the shadow maps' depth, drawn as a colour and read by Load (Direct3D 12
    // and Vulkan both should)
    shadow_maps = SDL_GPUTextureSupportsFormat(
        device, kShadowFormat, SDL_GPU_TEXTURETYPE_2D,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!shadow_maps)
        REXLOG_WARN("native view gpu: the device can't draw into an R32_FLOAT texture; the "
                    "characters are drawn without their self-shadows");
    // the overlay's multisampled targets (Direct3D 12 and Vulkan both must
    // have 4 samples; 2 every desktop GPU has)
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

    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    ti.format = kColorFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ti.width = ti.height = 1;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    white = SDL_CreateGPUTexture(device, &ti);
    black = SDL_CreateGPUTexture(device, &ti);
    // a plain 2D texture, as the resolve's depth binding is: a depth of 0
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
              velocity_object_pipelines[2]})
            if (p) SDL_ReleaseGPUGraphicsPipeline(device, p);
        for (SDL_GPUShader* sh : {vertex_shader, pixel_shader, spot_shader, soft_shader,
                                  shadow_shader, fullscreen_shader, resolve_shader,
                                  overlay_start_shader,
                                  downsample_shader, blur_shader, glare_shader, composite_shader,
                                  composite_history_shader, velocity_shader, velocity_object_vs,
                                  velocity_object_ps, gamma_shader})
            if (sh) SDL_ReleaseGPUShader(device, sh);
        if (sampler) SDL_ReleaseGPUSampler(device, sampler);
        if (linear_sampler) SDL_ReleaseGPUSampler(device, linear_sampler);
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
    arena_vert_count = arena_index_count = 0;
    pipelines.clear();
    warm = false;
    warmed_up = false;
    device = nullptr;
    vertex_shader = pixel_shader = spot_shader = soft_shader = shadow_shader = nullptr;
    fullscreen_shader = nullptr;
    shadow_maps = false;
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
    // Blend() in soft_raster.cpp. The picture's alpha is never written after
    // the resolve's 1, as the CPU's picture has it, so the readback is the
    // picture. A texture's blends alpha by the colour's factors; the scene's,
    // where the draw writes it, is ONE ONE MAX (AlphaMode). The target clamps
    // the colour to 0-1 before blending, as Blend() does; SrcAlpha's scaling
    // happens in the shader (kPremultiply), to the colour only, after the
    // shader's own clamp
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
    // the scene's alpha, where a draw writes it: the larger of the two
    // wherever RB3 blends (any mode but Src), the draw's own where it doesn't
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
    // a shadow map's depth, as it is: soft_raster.cpp's Target::zw
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
    // the culling RasterTri does: SDL's winding is the screen's, as D3D's is;
    // clipping at depth 1 is the CPU's near plane (mesh.hlsl)
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    pi.rasterizer_state.cull_mode = cull == CullWinding::kClockwise ? SDL_GPU_CULLMODE_BACK
                                    : cull == CullWinding::kCounterClockwise
                                        ? SDL_GPU_CULLMODE_FRONT
                                        : SDL_GPU_CULLMODE_NONE;
    pi.rasterizer_state.enable_depth_clip = true;
    // depth is larger-is-nearer, cleared to 0. A draw that writes without
    // testing tests "always", since a pipeline that doesn't test can't write
    pi.depth_stencil_state.enable_depth_test = rules.test || rules.write;
    pi.depth_stencil_state.enable_depth_write = rules.write;
    pi.depth_stencil_state.compare_op = !rules.test          ? SDL_GPU_COMPAREOP_ALWAYS
                                        : rules.equal_passes ? SDL_GPU_COMPAREOP_GREATER_OR_EQUAL
                                                             : SDL_GPU_COMPAREOP_GREATER;
    // a shadow map's is clip z/w, smaller nearer, cleared to its pass's
    // clear_z, LESS as RndShadowMap's ZFunc (soft_raster.cpp's RasterTri)
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
    // one Prewarm doesn't make: a frame waited for it (add it there)
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
    // RulesFor's, with blending on and off, culling nothing, what RndMat's
    // cull flag does (D3DCULL_CW) or the other side (D3DCULL_CCW: the
    // reflections, and a song draws it with several blends and alphas)
    constexpr DepthRules kRules[] = {{false, false, false}, {true, true, false},
                                     {false, false, true},  {true, true, true},
                                     {true, false, true}};
    for (CullWinding cull :
         {CullWinding::kNone, CullWinding::kClockwise, CullWinding::kCounterClockwise})
        for (int alpha = 0; alpha < kNumAlphaModes; alpha++)
            for (int blend = kBlendDest; blend <= kBlendPreMultAlpha; blend++)
                for (const DepthRules& r : kRules) Pipeline(blend, r, AlphaMode(alpha), cull);
    // the spotlights' cones: Add into the depth volume, which has no depth,
    // culled as each cone's draw says (RenderConeDefs sets D3DCULL_CCW)
    for (CullWinding cull :
         {CullWinding::kNone, CullWinding::kClockwise, CullWinding::kCounterClockwise})
        Pipeline(kBlendAdd, {false, false, false}, AlphaMode::kTexture, cull, PixelKind::kSpot);
    // the soft particles: into the soft-particle buffer, which has no depth,
    // by their materials' blends, culling nothing (particles have no cull mode)
    for (int blend = kBlendDest; blend <= kBlendPreMultAlpha; blend++)
        Pipeline(blend, {false, false, false}, AlphaMode::kTexture, CullWinding::kNone,
                 PixelKind::kSoft);
    // the shadow maps' depth, culled as each draw says (PrepShadow's
    // D3DCULL_CCW; ShadowDepthPipeline)
    if (shadow_maps)
        for (CullWinding cull :
             {CullWinding::kNone, CullWinding::kClockwise, CullWinding::kCounterClockwise})
            ShadowDepthPipeline(cull);
    // the overlay's, multisampled: its start, and its draws, which leave
    // the picture's alpha be (AlphaMode::kNone); at the game's 2 samples
    // whatever the setting is now, so turning it back on doesn't wait for
    // them, and at the setting's (the same ones again are found made)
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
        // Full: it starts over in new buffers with the meshes still within
        // their keep (the clock's included, gpu_view.h's residency) and this
        // frame's new ones, as much as kMaxArenaBytes allows
        // (ArenaRebuildKeep). What it keeps is copied from the old buffers on
        // the GPU in this frame's copy pass (Mesh::arena_vertex), not sent
        // again; the rest is let go, and comes back through the pool if it's
        // drawn again.
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
        // The old buffers stay this frame's until its copy pass has copied
        // out of them, which releases them (SDL keeps them until the GPU is
        // done with them: the frame before, which the GPU may still be
        // drawing, reads them). Ones a failed frame left are released first.
        ReleaseBuffer(old_arena_verts);
        ReleaseBuffer(old_arena_indices);
        old_arena_verts = std::exchange(arena_verts, Buffer{});
        old_arena_indices = std::exchange(arena_indices, Buffer{});
        arena_vert_count = arena_index_count = 0;
        // twice what goes in, so a rebuild is rare: Reserve makes a buffer
        // half again as big as it's asked for
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

bool GpuRenderer::Impl::PlaceTexture(Tex& tx) {
    const Texture& t = *tx.keep;
    uint32_t w, h;
    SizeClass(t.width, t.height, w, h);
    TexArray& a = tex_arrays[uint64_t(w) << 32 | h];
    if (a.free.empty() && a.texture) {
        // Full: before it grows, its textures no frame has drawn for
        // kEvictAfter frames, kept by the clock alone (gpu_view.h's
        // residency), go for room, as they went before the clock kept them.
        // Their layers are safe to send this frame's into: no frame within
        // kEvictAfter sampled them (so neither this frame's draws nor the
        // frames the GPU may still be drawing), and in the copy pass an
        // upload into a layer comes after this frame's ArrayCopy. One this
        // frame draws later is placed and sent again, as after Evict.
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
            ti.num_levels = FullMipChain(w, h);
            grown = SDL_CreateGPUTexture(device, &ti);
            counts.textures++;
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
            // before anything is sent to the new one (ArrayCopy)
            ArrayCopy c{a.texture, grown, w, h, {}};
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
                double(w) * h * 4 * layers * (a.levels > 1 ? 4.0 / 3 : 1) / 1048576;
            walk_stats->arrays_ms += std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - start)
                                         .count();
        }
    }
    tx.array = &a;
    tx.levels = std::min(LevelsOf(t), a.levels);
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

void GpuRenderer::Impl::UseTexture(const std::shared_ptr<const Texture>& t) {
    if (!t || !t->width || !t->height || t->rgba.size() != size_t(t->width) * t->height) return;
    Tex& tx = textures[t.get()];
    if (!tx.keep) {
        tx.keep = t;
        tx.first = serial;
        tx.first_world = frame_world;
        if (walk_stats) walk_stats->textures_first++;
        if (PlaceTexture(tx)) new_textures.push_back(&tx);
    }
    tx.used = serial;
    tx.used_at = frame_now;
    tx.drawn_in_song = frame_in_song;
    if (frame_world != tx.first_world) tx.across_worlds = true;
}

double GpuRenderer::Impl::TextureArrayMb() const {
    double mb = 0;
    for (const auto& [size, a] : tex_arrays) {
        const double layer = double(size >> 32) * double(size & 0xffffffffu) * 4;
        mb += layer * a.layers * (a.levels > 1 ? 4.0 / 3 : 1) / 1048576;
    }
    return mb;
}

const GpuRenderer::Impl::Tex* GpuRenderer::Impl::TextureFor(const Texture* t) {
    if (!t) return nullptr;
    auto it = textures.find(t);
    return it != textures.end() && it->second.array ? &it->second : nullptr;
}

GpuRenderer::Impl::Rt* GpuRenderer::Impl::TargetFor(const Pass& p, uint32_t w, uint32_t h) {
    Rt& rt = rts[p.tex_obj];
    UseRt(rt);
    rt.game_w = p.width;
    rt.game_h = p.height;
    // the texture's mips (FinishDrawTarget's downsamples), down to 1x1 at most
    uint32_t levels = 1;
    if (p.num_mips > 1) {
        uint32_t chain = 1;
        for (uint32_t s = std::max(w, h); s > 1; s >>= 1) chain++;
        levels = std::min(p.num_mips, chain);
    }
    // a shadow map's depth, read by Load: no mips
    const bool shadow = p.tex_type == kTexTypeShadowMap;
    if (shadow) levels = 1;
    if (rt.color && rt.w == w && rt.h == h && rt.levels == levels && rt.shadow == shadow)
        return &rt;
    const auto start = std::chrono::steady_clock::now();
    const bool resized = rt.color != nullptr;
    ReleaseRt(rt);
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
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = kDepthFormat;
    ti.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    ti.num_levels = 1;
    rt.depth = SDL_CreateGPUTexture(device, &ti);
    if (!rt.color || !rt.depth) {
        if (!rt_failure_logged) {
            rt_failure_logged = true;
            REXLOG_WARN("native view gpu: no {}x{} render target ({}); what samples it draws "
                        "transparent black",
                        w, h, SDL_GetError());
        }
        ReleaseRt(rt);
        rts.erase(p.tex_obj);
        return nullptr;
    }
    rt.w = w;
    rt.h = h;
    rt.levels = levels;
    rt.shadow = shadow;
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

void GpuRenderer::Impl::ReleaseRt(Rt& rt) {
    // SDL lets them go once the frames using them are done
    if (rt.color) SDL_ReleaseGPUTexture(device, rt.color);
    if (rt.depth) SDL_ReleaseGPUTexture(device, rt.depth);
    rt.color = rt.depth = nullptr;
    rt.w = rt.h = 0;
    rt.levels = 1;
    rt.shadow = false;
    rt.drawn = false;
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
    // post-processing's levels, each a quarter of the one before
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
// SDL 3.4.14's private Direct3D 12 texture (src/gpu/d3d12/SDL_gpu_d3d12.c's
// D3D12TextureContainer and D3D12Texture, SDL_sysgpu.h's TextureCommonHeader):
// an SDL_GPUTexture* is a container, its create info first. Nothing here is
// trusted until SdkResource's checks pass, and none of it is written.
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
// CommandBufferCommonHeader and its passes, SDL_gpu_d3d12.c's
// D3D12CommandBuffer, checked against release-3.4.14's source): an
// SDL_GPUCommandBuffer* is a D3D12CommandBuffer*, the common header first,
// none of it under an #ifdef. Read only, for the command list the GPU
// timings' timestamps go into (TimingList); nothing is trusted until the
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

// an object's identity: COM's rule is that its IUnknown pointer is the same
// however it's reached
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
    // the checks the N2 kill test ran (out/research/n2_design.md), on the
    // layout of SDL 3.4.14, the version these headers are; a newer SDL that
    // moved anything fails one of them before anything is called on it
    const auto* c = reinterpret_cast<const SdlD3D12TextureContainer*>(texture);
    // (a) the create info, all but props: SDL gives the container a
    // properties object of its own
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
    // pass's command buffer to its own, every time; (b) and what's after it
    // is there. Pointers compared only: nothing is called on them here.
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
        // the layout of SDL 3.4.14, the version these headers are; a newer
        // SDL that moved anything fails here before anything is called on it
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
        // the heap, a region per frame being drawn and the aside's, and the
        // readback buffer the frames' regions resolve into, two each
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
        // nothing of the aside's is left for a later frame to be charged
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
        // a command buffer that doesn't check out: the frame goes untimed,
        // and the aside's ladder, which it may have left open, starts over
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
    // the world passes before it and the world drawn ahead for it, ended
    // (one left open was cut short: not counted)
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
    // (a device removed fails here: no times)
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
    // SDL lets it go once its own work on it is done; the presenter holds the
    // Direct3D 12 texture itself for as long as its paints need it, and
    // native_view.cpp only has a slot drawn again once they're done with it
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
    // each one checked as the test texture was, while zero-copy is on
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
    // the world drawn ahead into the scene target, if the Render just before
    // this one did that (Ahead)
    const Ahead ahead_was = std::exchange(ahead, Ahead{});
    // the frame's parts' times (GpuStats::plan_ms and the rest)
    using Clock = std::chrono::steady_clock;
    const auto render_start = Clock::now();
    auto ms_since = [](Clock::time_point t) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    };
    if (!o.width || !o.height || !EnsureTargets(o.width, o.height)) return false;
    if (slot >= 0 && !EnsureOutput(slot, o.width, o.height)) return false;
    keep_frames = ResidencyKeepFrames(o.world_period);
    // where the last pass, the gamma ramp's, puts the finished frame
    SDL_GPUTexture* const output = slot >= 0 ? outputs[slot].texture : graded;
    // the post buffer (RasterOptions::post_buffer): a post frame's picture
    // kept, and shown by the frames after it that post-process nothing, in
    // place of their world, whose draws are left out
    const bool kept_buffer = o.post_buffer && o.view == RasterView::kFinal;
    const bool shows_kept = kept_buffer && ShowsPostBuffer(frame) && post_buffer.tex &&
                            post_buffer.w == width && post_buffer.h == height &&
                            PostBufferFor(frame, post_buffer.game_frame);
    const bool keeps = kept_buffer && ProcKnown(frame) && (frame.proc_cmds & kProcPost) &&
                       EnsureKept(post_buffer, width, height);
    st.shows_kept = shows_kept;
    // RasterOptions::world_ahead: this composed post frame's world is in the
    // scene target already, drawn ahead by the Render just before (serial is
    // still that one's), so its draws [0, composed_world_end) are left out
    // and it goes on from the resolve
    const bool uses_ahead = !ahead_pass && !pre_pass && o.world_ahead && frame.composed &&
                            frame.composed_world_end && !shows_kept &&
                            o.view == RasterView::kFinal && ahead_was.serial &&
                            ahead_was.serial == serial &&
                            ahead_was.world_frame == frame.world_frame &&
                            ahead_was.w == width && ahead_was.h == height;
    const uint32_t ahead_end = uses_ahead ? frame.composed_world_end : 0;
    st.ahead_used = uses_ahead ? 1 : 0;
    // The world's REFRACT_WORLD draws read the pre-process buffer (soft_raster.h's
    // RefractsWorld, RasterOptions::pre_buffer): the one kept from the world
    // frames before, or else the world drawn kPreBufferPasses times first
    // (each a Render of its own, before this one starts), as Rasterize()
    // does; black (no_depth's 0) where neither, as in a view of the scene
    // target, whose alpha and depth they don't change. This frame's world is
    // kept in turn.
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
            // the world alone, with nothing of the live view's: no
            // post-processing, history or samples
            RasterOptions po = o;
            po.post = po.trails = po.post_buffer = po.pre_buffer = false;
            po.msaa = 1;
            const auto pre_start = Clock::now();
            for (int k = 1; k <= kPreBufferPasses; k++) {
                GpuStats ps;
                if (!Render(frame, po, -1, nullptr, ps, k)) return false;
                // the first sends what the world draws, which this frame
                // then finds there
                st.pre_passes++;
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
    // a geometry this frame draws: in the arena already, or into it from
    // the last frame's pool, or into this frame's pool
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
            // drawn again (Evict keeps pool geometry ResidencyKeepFrames
            // after): it stays, moved from the last frame's pool if that
            // frame drew it, else sent again (its pool has been drawn over)
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
        // cleared it and where nothing in this frame has drawn it yet
        // (transparent black, as on the CPU, whose targets are the frame's
        // own): what an earlier frame left in it is never drawn over, so a
        // frame's picture doesn't depend on what was drawn before it (a
        // capture's on the live view's or the last capture's; the depth
        // volume's blurs in a frame with no cone would blur the last shot's
        // beams). One the frame samples but doesn't draw still reads what
        // was drawn last.
        Rt* target = nullptr;
        // The world drawn ahead draws the world's texture passes, not the
        // overlay's. A post frame using it leaves out the world's whose
        // targets it drew, and draws any other itself (counted: one the world
        // frame's capture planned without, which only the post frame's own
        // overlay samples).
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
            target = TargetFor(*run.pass, tw, th);
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
            // the depth volume's blurs read a copy of it, not its quad's
            // texture (so they don't count as sampling a target nothing drew);
            // its cones read the scene's depth, without which they're left out
            if (run.pass && spot::SpotBlur(it, state, *run.pass)) {
                spot_draw[d] = kSpotBlur;
                continue;
            }
            if (run.pass && IsSpotCone(it, state)) {
                spot_draw[d] = depth_sampled ? kSpotCone : kSpotConeSkipped;
                if (!depth_sampled) continue;
            }
            // the soft-particle buffer's blurs read the other surface's
            // target (a copy of it), not their quad's texture
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
            // what the diffuse texture is, as soft_raster.cpp's Diffuse() has
            // it: a render target is its pass's target if one has drawn it (by
            // this frame's runs before this one, or an earlier frame's), else
            // guest pixels if kept and wanted, else transparent black; a target
            // never samples itself (nor a shadow map's, which isn't a colour)
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
            // textured or not is settled when it draws, once the texture has
            // its layer; the maps are the shade's
            shade::PackShade(it, state, o, false, shades[d]);
            uint32_t& flags = shades[d].flags.x;
            if (flags & shade::kShadeSpecMap) UseTexture(state->maps[kMapSpecular]);
            if (flags & shade::kShadeGlow) UseTexture(state->maps[kMapGlow]);
            // the normal map and the detail map, as soft_raster.cpp's
            // NormalMap() has them: one RB3 draws (a head's) is its pass's
            // target if one has drawn it, else guest pixels if kept and
            // wanted, else it's left out (counted). REFRACT_WORLD's refract
            // normal map is s1 too, in the normal map's slot.
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
            // the projected light's s5, as soft_raster.cpp's Projected() has
            // it: a texture RB3 draws (NgLight's shadow) is its target where
            // this frame's last pass of it made the version the draw reads,
            // else guest pixels if kept and wanted, else the light is left
            // out (counted)
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
            // the shadow map, where this frame's last pass of it made the
            // version the draw reads (soft_raster.cpp's DrawOne); else lit
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
    // the composite's noise map, a texture like a draw's
    if (o.post && o.view == RasterView::kFinal && frame.noise_map) UseTexture(frame.noise_map);
    // the motion blur's object pass's meshes, and its palettes as bones:
    // each entry's three rows, as the shader reads them
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
    // From here a frame that fails leaves to_arena's meshes marked in_arena
    // at places their copies never reached (from the CPU, the last pool or
    // the old arena), and new_textures in layers never sent. Harmless: a
    // failed Render fails Draw, which gives up for the session and lets go
    // of everything (Release: meshes, textures, the arena, the device), so
    // no frame draws them after.
    if (!PlaceInArena()) return false;
    if (counts.arena_rebuilds != rebuilds)
        st.arena_new_mb = double(arena_verts.size + arena_indices.size) / 1048576;
    st.plan_arena_ms = ms_since(arena_start);

    // the upload: the pool's vertices and indices, the arena's new meshes that
    // weren't in the last frame's pool (nor kept from the old arena), the
    // bones, then the textures, each where a copy may start
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
    // each level of a texture where a copy may start
    auto level_bytes = [](const Texture& t, uint32_t l) {
        const uint32_t w = std::max(t.width >> l, 1u), h = std::max(t.height >> l, 1u);
        return Align(Align(w * 4, kRowPitchAlign) * h, kTextureOffsetAlign);
    };
    for (const Tex* tx : new_textures)
        for (uint32_t l = 0; l < tx->levels; l++) upload_bytes += level_bytes(*tx->keep, l);
    // what of it comes from the CPU (GpuStats::mesh_bytes and the rest)
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
            const Texture& t = *tx->keep;
            for (uint32_t l = 0; l < tx->levels; l++) {
                const uint32_t w = std::max(t.width >> l, 1u), h = std::max(t.height >> l, 1u);
                const uint32_t pitch = Align(w * 4, kRowPitchAlign);
                const uint32_t* px = l ? t.mips[l - 1].data() : t.rgba.data();
                for (uint32_t y = 0; y < h; y++)
                    std::memcpy(base + tex_at + size_t(y) * pitch, px + size_t(y) * w, w * 4);
                tex_at += level_bytes(t, l);
            }
        }
        SDL_UnmapGPUTransferBuffer(device, upload);
    }
    st.uploads = uint32_t(to_pool.size() + to_arena.size() - arena_copied + new_textures.size());
    st.upload_ms = ms_since(upload_start);
    const auto record_start = Clock::now();

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    if (!cmd) {
        REXLOG_WARN("native view gpu: no command buffer ({})", SDL_GetError());
        return false;
    }
    // GPU timings: its first timestamp, the upload's start
    StartTiming(cmd, o, slot, pre_pass, ahead_pass);
    // RasterOptions::gpu_labels: what each indexed draw is, in the order
    // they're recorded, a list per command buffer, handed to draw_log as the
    // frame is submitted; callers check gpu_labels first, so the text is
    // only made when it's on
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
        // texture arrays that grew: the old one's layers into the new one
        // first, then it can go (SDL keeps it until the copy is done)
        for (const ArrayCopy& c : array_copies) {
            for (const auto& [l, levels] : c.layers) {
                for (uint32_t m = 0; m < levels; m++) {
                    SDL_GPUTextureLocation src{c.from, m, l, 0, 0, 0};
                    SDL_GPUTextureLocation dst{c.to, m, l, 0, 0, 0};
                    SDL_CopyGPUTextureToTexture(copy, &src, &dst, std::max(c.w >> m, 1u),
                                                std::max(c.h >> m, 1u), 1, false);
                }
            }
            SDL_ReleaseGPUTexture(device, c.from);
        }
        // The frame before may still be drawing as this one is sent
        // (RenderFrameToOutput doesn't wait for it). The pools and the
        // bones, which each frame fills from the start, cycle: a buffer the
        // GPU still reads is left to it and SDL gives this frame another
        // (the copies to the arena read the last frame's pool, the one it
        // filled). The arena is appended to, into space no frame drew from
        // since it was last rebuilt (in a new buffer, PlaceInArena, which
        // what it kept is copied into from the old one: the frames still
        // drawing only read that too), and a texture goes into a new layer or
        // one Evict or PlaceTexture let go, which the frames still drawing
        // don't sample but where a world pass before this frame (pre_pass)
        // let go of the frame before's own textures; there, as with the
        // targets every frame draws over, SDL's barriers hold this frame's
        // copy on its queue until those reads are done.
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
            // from the old arena (kept by a rebuild) or the last frame's
            // pool, into its place in the arena
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
            const Texture& t = *tx->keep;
            for (uint32_t l = 0; l < tx->levels; l++) {
                const uint32_t w = std::max(t.width >> l, 1u), h = std::max(t.height >> l, 1u);
                const uint32_t pitch = Align(w * 4, kRowPitchAlign);
                SDL_GPUTextureTransferInfo src{upload, tex_at, pitch / 4, h};
                SDL_GPUTextureRegion dst{};
                dst.texture = tx->array->texture;
                dst.mip_level = l;
                dst.layer = tx->layer;
                dst.w = w;
                dst.h = h;
                dst.d = 1;
                SDL_UploadToGPUTexture(copy, &src, &dst, false);
                tex_at += level_bytes(t, l);
            }
        }
        SDL_EndGPUCopyPass(copy);
    }
    // a rebuilt arena's old buffers, copied out of above
    ReleaseBuffer(old_arena_verts);
    ReleaseBuffer(old_arena_indices);

    // The runs in order, a render pass each stretch: the back buffer's, split
    // where Rasterize() clears depth (a camera it hasn't seen yet starts
    // drawing) and resumed after each texture pass; each texture pass into
    // its target, then its mips made
    SDL_GPUBuffer* bone_buffer = bone_bytes ? bones.buffer : no_bones;
    SDL_GPURenderPass* pass = nullptr;
    // what's bound in the pass, so a draw binds only what changes
    SDL_GPUGraphicsPipeline* bound = nullptr;
    SDL_GPUBuffer* bound_verts = nullptr;
    SDL_GPUTexture* bound_tex[kNumSpotSlots] = {};
    float bound_viewport[4] = {};
    // the open pass's scissor is the song list's cut (InOverlayCut), not the
    // whole target
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
            // a multisampled overlay pass resolves into the picture as it
            // ends: timed apart, until whatever comes next marks its own
            if (pass_samples > 1) MarkTime(cmd, gpu_timing::kOverlayResolve);
            SDL_EndGPURenderPass(pass);
        }
        pass = nullptr;
    };

    // the back buffer: the world's draws into the scene target, cleared to
    // the frame's clear colour (alpha 0) the first time, and the overlay's
    // into the picture once the resolve has filled it; depth cleared the
    // first time and, in a capture from before its cameras were kept,
    // whenever a new camera starts, else after the resolve
    const BackBufferLayout layout = LayoutBackBuffer(frame);
    bool clear_overlay_depth = false;
    bool back_begun = false;
    bool resolved = false;
    // what holds the picture once the resolve is done, for the overlay's
    // start and the gamma ramp's pass: `color`, or the post buffer itself
    // where a frame showing it leaves it there (resolve), until a
    // multisampled overlay pass resolves the picture into `color`
    SDL_GPUTexture* picture = color;
    // the picture as the resolve left it, for the overlay's REFRACT_WORLD
    // draws: the post buffer where it's that picture already, else `behind`,
    // the resolve's copy
    SDL_GPUTexture* behind_now = behind;
    bool depth_fresh = false;  // the open pass's depth is cleared and untouched
    // The overlay's samples (soft_raster.h's OverlaySamples) as the device
    // draws them, into its multisampled targets; 1 into `color`, over the
    // world's depth, as before. A capture from before the cameras were kept
    // has its overlay go on over the world's depth, which the overlay's start
    // reads: single-sampled where the device can't sample it.
    SDL_GPUTexture* const world_depth = depth_sampled ? depth : no_depth;
    uint32_t overlay_samples = DeviceSamples(OverlaySamples(o));
    if (overlay_samples > 1 &&
        ((!layout.cameras && !depth_sampled) || !EnsureOverlayTargets(overlay_samples) ||
         !OverlayStartPipeline(overlay_samples)))
        overlay_samples = 1;
    bool overlay_start = false;  // the overlay's targets are yet to be started
    // the frame's clear colour (soft_raster.h's ClearRgba), the scene's alpha 0
    const uint32_t clear_rgba = ClearRgba(frame);
    const SDL_FColor clear_color = {float(clear_rgba & 0xff) / 255.0f,
                                    float(clear_rgba >> 8 & 0xff) / 255.0f,
                                    float(clear_rgba >> 16 & 0xff) / 255.0f, 0.0f};
    auto begin_back = [&](bool clear_depth) {
        // the GPU's time from here is the world's or the overlay's (marked
        // here rather than as each texture pass ends: nothing goes between,
        // and back to back passes then take one timestamp each)
        MarkTime(cmd, resolved ? gpu_timing::kOverlay : gpu_timing::kWorld);
        // the overlay's, multisampled: each of its passes resolves into the
        // picture as it ends
        const bool ms = resolved && overlay_samples > 1;
        bool depth_cleared = false;
        if (ms && overlay_start) {
            // RB3's DoPostProcess clears its 2x target and draws the post
            // picture into it (BeginTiling, CopyPostProcess): here the
            // picture into every sample, and the overlay's depth 0, or the
            // world's where a capture from before the cameras goes on over
            // it (and no new camera clears it)
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
            SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
            SDL_EndGPURenderPass(rp);
            overlay_start = false;
            depth_cleared = !world;
            clear_depth = false;
        }
        // this pass resolves the picture into `color` as it ends
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
        // SDL_gpu's default viewport, all of the target, until a draw's
        // camera has another (PlaceBackBufferDraw)
        bound_viewport[2] = float(width);
        bound_viewport[3] = float(height);
        back_begun = true;
        depth_fresh = clear_depth || depth_cleared;
        pass_samples = ms ? overlay_samples : 1;
    };

    // one of post.hlsl's full-screen passes: `pipeline` into all of `target`
    // (w x h), and of `second` (as big) if given, reading `sources` at t0,
    // t1... with `params`
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
        SDL_BindGPUGraphicsPipeline(rp, pipeline);
        SDL_GPUTextureSamplerBinding tb[12];
        uint32_t n = 0;
        for (SDL_GPUTexture* t : sources) tb[n++] = {t, linear_sampler};
        SDL_BindGPUFragmentSamplers(rp, 0, tb, n);
        SDL_PushGPUFragmentUniformData(cmd, 0, &params, sizeof(params));
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        SDL_EndGPURenderPass(rp);
    };
    SDL_GPUTexture* const scene_depth = depth_sampled ? depth : no_depth;

    // RB3's post-processing (post_model.h), the passes RunPost runs on the
    // CPU, into the RGBA8 levels: the DOF's, then bloom's, then the composite
    // into the picture
    post::PostPlan post_plan;
    const auto post_plan_start = Clock::now();
    bool post_on = o.post && o.view == RasterView::kFinal &&
                   post::PlanPost(frame, o.post_only, post_plan, o.grain, o.velocity);
    st.post_plan_ms = ms_since(post_plan_start);
    // depth of field blurs by the depth, which reads as 0 (all blurred)
    // without a sampled one: left out then (Create warns of it, once)
    if (post_on && !depth_sampled) {
        post_plan.composite.flags.x &= ~(post::kPostDof | post::kPostVelocity);
        post_on = post_plan.composite.flags.x != 0;
    }
    // the motion blur's object pass (velocity.hlsl), over the camera pass's
    // texels: each object's mesh, its palettes in the frame's bones, its
    // depth in a buffer of its own cleared to 1
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
            SDL_DrawGPUIndexedPrimitives(rp, uint32_t(v.geom->indices.size() / 3 * 3), 1,
                                         m.first_index, int32_t(m.first_vertex), 0);
        }
        SDL_EndGPURenderPass(rp);
    };
    auto post_process = [&] {
        post::PostPass p = post_plan.composite;
        const uint32_t flags = p.flags.x;
        // the 4x downsample (or the bright pass) of `src`, sw x sh, into
        // `dst` at level k's size
        auto downsample = [&](SDL_GPUTexture* src, uint32_t sw, uint32_t sh, SDL_GPUTexture* dst,
                              int k, bool bright) {
            p.mode = {0, bright ? 1u : 0u, 0, 0};
            p.half_pixel = {0.5f / float(sw), 0.5f / float(sh), 0, 0};
            fullscreen(dst, post_w[k], post_h[k], downsample_pipeline, {src}, p);
        };
        // `level` (level k's size) blurred across into k's spare, then down
        // back into it
        auto blur = [&](SDL_GPUTexture* level, int k, const post::float4* across,
                        const post::float4* down, uint32_t taps) {
            p.mode = {0, 0, taps, 0};
            std::copy(across, across + taps, p.taps);
            fullscreen(post_tmp[k], post_w[k], post_h[k], blur_pipeline, {level}, p);
            std::copy(down, down + taps, p.taps);
            fullscreen(level, post_w[k], post_h[k], blur_pipeline, {post_tmp[k]}, p);
        };
        // the velocity pass, from the scene's depth (t1), then the objects
        // with their own motion over it
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
            // and its glare pass, into level 0's spare
            if (flags & post::kPostGlare) {
                p.mode = {0, 0, 0, 0};
                fullscreen(post_tmp[0], post_w[0], post_h[0], glare_pipeline, {post_bloom[0]}, p);
                bloom0 = post_tmp[0];
            }
        }
        // the spotlights' depth volume and density map, and the soft-particle
        // surface, as this frame's passes drew them (transparent black if
        // none did)
        auto drawn_now = [&](uint32_t tex_obj) {
            const auto f = rts.find(tex_obj);
            // (or by the world drawn ahead, which drew this frame's world)
            return tex_obj && f != rts.end() &&
                           (f->second.drawn_in == serial ||
                            (uses_ahead && f->second.drawn_in == ahead_was.serial))
                       ? f->second.color
                       : black;
        };
        // the noise map's layer, its sampler packed for the levels it has
        // there; none (no layer for it) leaves the noise out
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
        // the levels an effect that's off didn't draw are bound all the same,
        // and not read
        MarkTime(cmd, gpu_timing::kComposite);
        p.mode = {0, 0, 0, 0};
        // The live view's composite keeps the post buffer the trails read:
        // a post frame's goes into the history's other texture, which is
        // then the current one (once a game frame); a world frame's (no
        // constants) only reads it. The trails need a post frame from
        // before this one.
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

    // whether an overlay draw reads the picture behind it, for which the
    // resolve keeps a copy of it
    bool refracts = false;
    for (size_t d = frame.post_boundary; d < frame.draws.size() && !refracts; d++) {
        const DrawItem& it = frame.draws[d];
        refracts = DrawnToBackBuffer(it) && RefractsWorld(shade::ShadeOf(frame, it));
    }

    // the scene into the picture, at post_boundary or the frame's end:
    // post-processed, or as it is, or the view of the scene target asked for
    auto resolve = [&] {
        end_pass();
        // the scene cleared, if nothing drew to it (the world drawn ahead
        // did, and the overlay goes on over its depth as over the world's)
        if (!back_begun && uses_ahead) {
            back_begun = true;
        } else if (!back_begun) {
            begin_back(true);
            end_pass();
        }
        // the scene as the world left it, before post-processing, as
        // DoWorldEnd's SavePreBuffer keeps it: for the next world frame's
        // REFRACT_WORLD draws, or the next world pass's
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
        // the world drawn ahead ends with its scene, which the post frame
        // after it post-processes
        if (ahead_pass) {
            resolved = true;
            return;
        }
        // the post buffer as the picture, or the picture kept as it.
        // Multisampled, the overlay's start reads the post buffer itself and
        // its passes resolve into `color`, so it isn't copied there first
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
        // the picture behind the overlay's refracting draws: the post
        // buffer, where it's this picture (shown or just kept), else a copy
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
        // the back buffer's draws from here on are the overlay's
        MarkTime(cmd, gpu_timing::kOverlay);
        // the overlay's depth starts cleared with the capture's cameras, as
        // on the CPU (Rasterize's resolve); multisampled, the overlay's first
        // pass starts its targets
        clear_overlay_depth = layout.cameras;
        overlay_start = overlay_samples > 1;
    };

    // the density map the spotlights' cones read: the last drawn, as on the
    // CPU (0 none); and the texture pass being drawn, its target's
    uint32_t density_obj = 0;
    const Rt* pass_rt = nullptr;

    // one draw into the open pass; `no_z` a texture without a depth buffer,
    // `shadow_depth` a shadow map's, into which it draws its depth alone
    auto draw = [&](size_t d, AlphaMode alpha, bool no_z, bool shadow_depth = false,
                    const DepthMap& depth_map = DepthMap{}, bool overlay_edge = false) {
        const DrawItem& it = frame.draws[d];
        const Mesh& m = meshes[it.geom.get()];
        const int blend = BlendFor(it, o);
        const CullWinding cull = CullFor(it, o);
        if (cull == CullWinding::kAll) return;  // culls both sides: draws nothing
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
        // a shadow map's depth reads nothing but where the vertices are
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
            SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));
            if (o.gpu_labels)
                label(fmt::format("draw {} shadow depth: mesh {:#x} into {:#x}, {} indices, {} "
                                  "vertices, {} bones",
                                  d, it.mesh, it.target, it.geom->indices.size(),
                                  it.geom->verts.size(), it.bones.size()));
            SDL_DrawGPUIndexedPrimitives(pass, uint32_t(it.geom->indices.size() / 3 * 3), 1,
                                         m.first_index, int32_t(m.first_vertex), 0);
            st.draws++;
            return;
        }

        // the textures the shade samples: a layer of a size class's array,
        // or a render target's own; a map that couldn't have a layer is left
        // out, as if the capture had none
        struct Sampled {
            SDL_GPUTexture* texture = nullptr;
            uint32_t layer = 0, w = 0, h = 0;
            uint32_t levels = 1;
        };
        auto layer_of = [&](const Texture* t) {
            const Tex* tx = TextureFor(t);
            return tx ? Sampled{tx->array->texture, tx->layer, t->width, t->height, tx->levels}
                      : Sampled{};
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
        // the shadow map's target, as its pass left it (the plan kept the
        // flag only where that's the version the draw reads)
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
        // REFRACT_WORLD reads the picture behind it: into the picture, once
        // the resolve has kept a copy of it; into the world, the pre-process
        // buffer (world_behind)
        if (sp.flags.x & shade::kShadeRefract) {
            if (resolved && refracts && alpha != AlphaMode::kTexture)
                tex[kSlotBehind] = {behind_now, 0, width, height};
            else if (!resolved && alpha != AlphaMode::kTexture)
                tex[kSlotBehind] = {world_behind, 0, width, height};
            else
                sp.flags.x &= ~(shade::kShadeRefract | shade::kShadeRefractMap);
        }
        for (int s = 0; s < kNumSlots; s++) {
            // behind's and the shadow map's bindings are plain 2D textures;
            // no_depth is one
            SDL_GPUTexture* sampled = tex[s].texture                            ? tex[s].texture
                                      : s == kSlotBehind || s == kSlotShadow ? no_depth
                                                                             : white;
            if (sampled == bound_tex[s]) continue;
            const SDL_GPUTextureSamplerBinding ts{sampled, sampler};
            SDL_BindGPUFragmentSamplers(pass, uint32_t(s), &ts, 1);
            bound_tex[s] = sampled;
        }
        // a cone reads the scene's depth where its pixel is on the screen,
        // and the density map drawn before it (black if none: 0), with its
        // numbers in a second buffer (soft_raster.cpp's SpotPixel)
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
        // a soft particle reads the scene's depth there too (0 where the
        // device can't sample it: unfaded), and the camera's far plane, in
        // the second buffer's depth range (soft_raster.cpp's SoftPixelFade)
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
        SDL_PushGPUVertexUniformData(cmd, 0, &vu, sizeof(vu));
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
        // the samplers they're read with: the game's where the capture kept
        // them and filtering is on, else the old nearest (soft_raster.cpp's
        // DrawOne likewise)
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
                    default: return none;  // the projected light's: bilinear, its own way
                }
            };
            for (int s : {kSlotDiffuse, kSlotSpecular, kSlotGlow, kSlotNormal, kSlotDetail}) {
                if (!tex[s].texture) continue;
                PackSampler(sampler_of(s), tex[s].levels,
                            pu.tex_sampler[s >= kSlotNormal ? s - 2 : s]);
            }
        }
        pu.flags[0] = blend == kBlendSrcAlpha || blend == kBlendSrcAlphaAdd ? kPremultiply : 0;
        SDL_PushGPUFragmentUniformData(cmd, 0, &pu, sizeof(pu));

        if (o.gpu_labels) {
            // each texture's size and its sampler's anisotropy, in tex_layer's order
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
        SDL_DrawGPUIndexedPrimitives(pass, uint32_t(it.geom->indices.size() / 3 * 3), 1,
                                     m.first_index, int32_t(m.first_vertex), 0);
        st.draws++;
    };

    cams_seen.clear();
    uint32_t last_cam = 0;
    // the back buffer's draws in their cameras' viewports, layered by their z
    // ranges (soft_raster.h's LayoutBackBuffer, which Rasterize() follows)
    for (size_t r = 0; r < runs.size(); r++) {
        const PassRun& run = runs[r];
        // a world pass ends with the world
        if ((pre_pass || ahead_pass) && resolved) break;
        if (!run.pass) {
            for (size_t d = run.first; d < run.end; d++) {
                const DrawItem& it = frame.draws[d];
                if (!DrawnToBackBuffer(it) || (shows_kept && d < frame.post_boundary)) continue;
                if (d < ahead_end) continue;
                if (!resolved && d >= frame.post_boundary) resolve();
                if (resolved && (o.view != RasterView::kFinal || pre_pass || ahead_pass)) break;
                bool clear_depth = false;
                // (a DrawRect quad has no camera of its own)
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
                // the overlay's camera draws over the whole picture reach its
                // edges (RasterOptions::overlay_edge); DrawRect's quads are
                // in pixels, a camera's with a screen rect in its own
                const bool whole = vp[0] == 0.0f && vp[1] == 0.0f &&
                                   vp[2] == float(width) && vp[3] == float(height);
                const bool overlay_whole = resolved && whole && it.rect_shader < 0;
                // the song list's rows cut at 16:9 instead (InOverlayCut): to
                // the 16:9 frame's columns
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
        // a shadow map's: its depth alone, its colour the same depth (clip
        // z/w), both cleared to the pass's clear_z, or as far as it goes where
        // nothing has drawn it yet (soft_raster.cpp's RtTarget::zw)
        const bool shadow_map = rt.shadow;
        const float clear_z = (p.clear_flags & 0x30) ? p.clear_z : 1.0f;
        // its GPU time's part (the blurs in it apart)
        const uint8_t pass_part =
            shadow_map ? gpu_timing::kPassShadow
            : p.tex_type == kTexTypeDepthVolume || p.tex_type == kTexTypeDensityMap
                ? gpu_timing::kPassSpot
                : gpu_timing::kPassOther;
        MarkTime(cmd, pass_part);
        // into its target, cleared as the run says the first time; again
        // after a blur, as the blur left it
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
                // The depth volume's blur (or NgLight's shadow's), as
                // soft_raster.cpp's SpotBlurDraw: the target copied as it is,
                // then the copy's taps (offsets c31.., weights c47..: the same
                // in every channel) into all of it, which is the rect BlurRT
                // (BlurShadowRT) draws, with Src as its material blends. Each
                // has a copy of its own size, so neither remakes the other's.
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
                // in a target bigger than the game's, each tap over the
                // texels the game's covers (soft_raster.h's BlurSubTaps)
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
                // The soft-particle buffer's blur, as soft_raster.cpp's
                // TapBlurDraw: the other surface as this frame's pass left it
                // (copied: the blur reads a plain 2D texture), its taps into
                // all of this one, which is the rect BlurSurface draws, with
                // Src as its material blends; transparent black (no_depth's
                // 0) if no pass drew it
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
            // the camera's viewport, scaled with the target; DrawRect's quads
            // are in the target's pixels, over all of it (soft_raster.cpp
            // likewise)
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
        // in place of FinishDrawTarget's downsamples. SDL makes them with
        // blits of its own, a sampler each, which would put the command
        // buffer's sampler heap off kSamplerBatch's step (BeginPass); so in a
        // command buffer of their own, the frame's work so far submitted
        // before it and the rest in a new one (each starts its heaps afresh).
        // One queue runs them in order, so the frame's fence still waits out
        // all of it.
        if (rt.levels > 1) {
            SDL_GPUCommandBuffer* next = nullptr;
            // the GPU's time from this command buffer's end to the mips'
            // start, and from theirs to the next's, is spent waiting for the
            // CPU to submit them (gpu_timing_model.h's kIdle)
            MarkTime(cmd, gpu_timing::kIdle);
            if (SDL_SubmitGPUCommandBuffer(cmd)) {
                if (SDL_GPUCommandBuffer* mips = SDL_AcquireGPUCommandBuffer(device)) {
                    MarkTime(mips, gpu_timing::kMips);
                    SDL_GenerateMipmapsForGPUTexture(mips, rt.color);
                    MarkTime(mips, gpu_timing::kIdle);
                    if (SDL_SubmitGPUCommandBuffer(mips))
                        next = SDL_AcquireGPUCommandBuffer(device);
                }
            }
            if (!next) {
                REXLOG_WARN("native view gpu: a texture pass's mips didn't submit ({})",
                            SDL_GetError());
                return false;
            }
            cmd = next;
            if (o.gpu_labels) indexed_draws.emplace_back();
        }
    }
    if (!resolved) resolve();
    end_pass();

    // The world drawn ahead is submitted and left to the GPU, with no fence
    // of its own: the next Render goes after it on SDL's one queue, and its
    // fence (waited for, the post frame's or any other) waits it out too. As
    // the world passes before a frame (pre_pass, gpu_no_wait) do; a fence
    // polled from outside is what hung AMD GPUs (RasterOptions::gpu_no_wait).
    if (ahead_pass) {
        if (o.gpu_labels) {
            std::lock_guard lock(draw_log_mutex);
            draw_log_frame = frame_label + " (world ahead)";
            draw_log = std::move(indexed_draws);
        }
        // its GPU time's end, on the aside's ladder: the post frame after
        // it resolves and reads it with its own
        MarkTime(cmd, gpu_timing::kNone);
        st.record_ms = ms_since(record_start);
        const auto submit_start = Clock::now();
        if (!SDL_SubmitGPUCommandBuffer(cmd)) {
            REXLOG_WARN("native view gpu: the world ahead didn't submit ({})", SDL_GetError());
            return false;
        }
        st.submit_ms = ms_since(submit_start);
        const auto evict_start = Clock::now();
        Evict();
        st.evict_ms = ms_since(evict_start);
        ahead = {serial, frame.game_frame, width, height};
        return true;
    }

    // the display's gamma ramp over all of it, as the presenter applies it
    // (gamma_ramp.h), by the CPU's lookup: each value's entry, red in the low
    // byte, four to a uint4. Every frame ends in this pass, into its output:
    // with no ramp (or a view, or the ramp off) the lookup is the identity,
    // which gives each 8-bit value back as it was, alpha the resolve's 1
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
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
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
    // the GPU timings' last timestamp: a world pass's end on the aside's
    // ladder, or the frame's, resolved with the aside's for reading once
    // it's finished (below, or OutputDone)
    if (pre_pass) MarkTime(cmd, gpu_timing::kNone);
    ResolveTimes(cmd);
    if (o.gpu_labels) {
        std::lock_guard lock(draw_log_mutex);
        draw_log_frame = frame_label;
        draw_log = std::move(indexed_draws);
    }
    // With gpu_no_wait, a world pass before the frame (pre_pass) and the
    // presenter's frame aren't waited for: what comes after them on SDL's
    // one queue (this frame's passes, RenderFrame's wait, the next frame)
    // goes after them on the GPU too, and the presenter's caller waits for
    // its output's fence (OutputDone) before the presenter's queue may
    // sample it. Otherwise, and for RenderFrame's picture to read back, it
    // waits here.
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
        st.submit_ms = ms_since(submit_start);
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
    st.submit_ms = ms_since(submit_start);
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
        // alpha is the resolve's 0xff throughout: the passes after it, the
        // gamma ramp's too, write 1
        rgba->resize(size_t(width) * height);
        std::memcpy(rgba->data(), px, rgba->size() * sizeof(uint32_t));
        SDL_UnmapGPUTransferBuffer(device, readback);
    }
    st.wait_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                           submitted).count();
    // the frame's GPU timings, finished with it (a world pass's are its
    // frame's, read with them)
    if (!pre_pass) ReadTimes(slot >= 0 ? slot : kOutputs, st);
    const auto evict_start = Clock::now();
    Evict();
    st.evict_ms = ms_since(evict_start);
    return true;
}

void GpuRenderer::Impl::Evict() {
    // geometry only one frame drew stays for keep_frames (gpu_view.h's
    // residency), and moves to the arena if a frame draws it again; there
    // it stays until it's gone kEvictAfter frames undrawn, and in a song
    // kKeepSeconds if frames of more than one world drew it (ClockKeep). The
    // arena's space comes back when it's rebuilt.
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
    // likewise a texture drawn in one frame only, such as one RB3 rendered
    // that frame or a movie's frame, goes keep_frames after, and one drawn
    // in more than one as a mesh in the arena (one kept by the clock goes
    // sooner if its array is full: PlaceTexture)
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
    // a render target no frame has drawn or sampled for kEvictAfter frames
    // is forgotten: undrawn, so what samples it finds nothing drawn, as it
    // would with the target released (gpu_view.h's residency); counted once,
    // as a release was (a forgotten one a frame samples again without
    // drawing it stays forgotten). Its textures go kKeepSeconds after.
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
    // and past kMaxRts the forgotten least recently used go at once
    const size_t over = RtsOverCap(rts_forgotten, rts.size(), kMaxRts);
    for (size_t i = 0; i < over; i++) {
        const auto f = rts.find(rts_forgotten[i].second);
        ReleaseRt(f->second);
        rts.erase(f);
        counts.rts_released++;
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
    // (after a frame Draw has drawn: warm, the device set up)
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
    // the last of its command buffers with that many indexed draws: the one
    // the GPU got furthest into
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
    // done already, without waiting for a frame being drawn
    if (impl_->warm) return;
    std::lock_guard lock(impl_->mutex);
    if (impl_->device && !impl_->warm) impl_->Prewarm(overlay_samples);
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
    // made already: a frame being drawn finishes first, and the next finds
    // no device (Draw) and is drawn on the CPU
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
    // before the clock starts: it's the device's setting up, not a frame's
    if (!impl_->warm) impl_->Prewarm(OverlaySamples(options));
    const auto start = std::chrono::steady_clock::now();
    stats = GpuStats{};
    // without R32_FLOAT targets, no shadow map's pass: every SHADOW_BUFFER
    // draw lit, as the CPU draws them without self_shadow
    RasterOptions o = options;
    o.self_shadow = options.self_shadow && impl_->shadow_maps;
    const Impl::Counts before = impl_->counts;
    if (!impl_->Render(frame, o, slot, rgba, stats)) {
        // whatever went wrong would go wrong every frame; the CPU takes over
        REXLOG_WARN("native view gpu: giving up for this session, the native view draws on "
                    "the CPU");
        impl_->ready = false;
        impl_->Release(false);
        return false;
    }
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
        // colour (RGBA8, or a shadow map's R32_FLOAT) and D32 depth
        const double pixels = double(rt.w) * rt.h;
        stats.rts_mb += pixels * 4 * ((rt.levels > 1 ? 4.0 / 3 : 1) + 1) / 1048576;
    }
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
