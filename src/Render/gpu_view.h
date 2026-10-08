#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "src/Render/gpu_timing_model.h"
#include "src/Render/scene_capture.h"
#include "src/Render/soft_raster.h"

// Experimental: draws a FrameCapture on the GPU through SDL_gpu for the native
// view (native_view_backend = gpu), offscreen, reading back the same RGBA as
// Rasterize(). soft_raster.cpp stays the reference (tools/native_view_replay
// --diff) and the fallback.
//
// band3.exe links its own static SDL, separate from rexruntime's that owns the
// game window; the device uses SDL's offscreen video driver, so makes no
// windows.
//
// It matches Rasterize(): every blend mode, skinning on the GPU, depth as 1/w.
// Texture passes draw into render targets of their own (kept between frames
// by DxTex), mips made after by shaders/mips.hlsl in the frame's command
// buffer (RasterOptions::inline_mips). Material textures are filtered by the
// game's samplers in the shader (sample_model.h; LOD from ddx_fine/ddy_fine,
// as the CPU computes it), not hardware samplers, which can't wrap a texture
// in the corner of a bigger array layer (gpu_view.cpp's SizeClass); BC1/2/3/5
// stay compressed in arrays of their own (RasterOptions::bc_textures), k_8
// (movie planes) as R8 (r8_textures), the rest RGBA8. The world draws into a
// scene target (colour, alpha, depth) that RB3's post-processing
// (post_model.h, shaders/post.hlsl) reads into the picture before the overlay. Spotlight cones (spot_model.h) use mesh.hlsl's
// PSSpotCone, reading scene depth; soft particles (IsSoftParticle) use
// PSSoftParticle, faded by it; their blurs blur a copy with post.hlsl's blur.
// Shadow map passes write clip z/w into an R32_FLOAT target by PSShadowDepth
// (LESS); SHADOW_BUFFER draws read four of its texels
// (RasterOptions::self_shadow). NgLight's shadow (casters' silhouettes
// blurred twice in place) is a texture pass the projected light reads as s5.
// The gamma ramp (gamma_ramp.h) goes last by shaders/gamma.hlsl into the
// output texture; without a ramp an identity lookup (exact at 8 bits), so
// every frame ends in that pass. Targets are sized as the CPU's
// (PassTargetSize).
//
// The overlay is multisampled as the CPU's (OverlaySamples): 2 (or 4) sample
// colour and depth targets start as the picture (post.hlsl's PSOverlayStart)
// and a cleared depth, and resolve into the picture as each overlay pass ends
// (SDL_GPU_STOREOP_RESOLVE_AND_STORE). Where the device can't draw that
// count, the other of 2 and 4, else 1 (logged).
//
// With the native picture shown, the gamma pass writes one of kOutputs
// textures the SDK's presenter samples in place, no readback: on Windows
// SDL_gpu's D3D12 device is the SDK's own (one device per adapter per
// process), so the ID3D12Resource behind SDL's texture, reached through SDL
// 3.4.14's private texture layout and checked (CheckZeroCopy), is readable by
// the SDK's command list. Because the device is shared, none is made on WARP
// (vendor 0x1414): it faults (an AV that kills band3) once band3's and the
// emulated GPU's frames run on it together; native_view.cpp's RefuseDevice
// sends the native view to the CPU.
//
// With RasterOptions::gpu_timestamps (native_gpu_timestamps), timestamps are
// written at each part boundary (gpu_timing_model.h) straight into SDL's
// command list, reached through SDL 3.4.14's private command buffer layout
// (checked once by CheckTimingOnce, pointers at every command buffer), and
// resolved by the frame's last command buffer (GpuStats::gpu_ms). They change
// no state SDL tracks, so the picture and sampler batches (kSamplerBatch) are
// unaffected. Where the checks fail, or on Vulkan: no timings (logged once).

namespace band3::render {

struct GpuStats {
    uint32_t draws = 0;
    uint32_t skipped = 0;   // draws it couldn't do (no geometry, or no pipeline)
    uint32_t uploads = 0;   // meshes and textures sent to the GPU this frame
    uint32_t passes = 0;    // texture passes drawn
    uint32_t rt_missing = 0;  // draws that sampled a render target nothing had drawn
    // RenderFrame: the whole frame (uploads, drawing, readback), and of that,
    // submit to picture. RenderFrameToOutput: up to submission only, wait 0
    // (the caller waits via OutputDone).
    double ms = 0;
    double wait_ms = 0;
    // time decoding what the game thread left undecoded (LatestCapture's
    // first-seen textures and meshes), added into ms by the worker
    double decode_ms = 0;
    // For the slow-frame log and per-kind stats (native_view.cpp). Worker
    // milliseconds: world passes before the frame (pre_passes of them,
    // kPreBufferPasses for a refracting world without a kept pre-process
    // buffer), planning, upload, recording, submitting, Evict. submit_ms spans
    // all `submits` (command buffers: one more per texture pass with mips and
    // at the resolve, SubmitAtResolve), overlapping record_ms and wait_ms.
    double pre_ms = 0, plan_ms = 0, upload_ms = 0, record_ms = 0, submit_ms = 0, evict_ms = 0;
    uint32_t pre_passes = 0;
    uint32_t submits = 0;
    // record_ms's SDL_AcquireGPUCommandBuffer calls (the frame's first and
    // one after each submission but the last), their time and the slowest;
    // and each command buffer's first draw, where SDL takes its descriptor
    // heaps (making them if none are free), their time and the slowest
    uint32_t acquires = 0;
    double acquire_ms = 0, acquire_max_ms = 0;
    double first_draw_ms = 0, first_draw_max_ms = 0;
    // later draws over 0.05 ms (SDL taking another descriptor heap pair, its
    // sampler heap full), and their time
    uint32_t draw_slow = 0;
    double draw_slow_ms = 0;
    // record_ms's pipeline binds and uniform pushes that took over 0.05 ms
    // (SDL making a 32 KB uniform buffer: its pool had none free), and their
    // time; draws' and post-processing's
    uint32_t uniform_slow = 0;
    double uniform_slow_ms = 0;
    // the slowest of submit_ms's submissions
    double submit_max_ms = 0;
    // pre_ms's parts, over its world passes (each a Render of its own, whose
    // first meets the world's new content): plan, and of it the walk and the
    // render targets and texture arrays made in it; upload, record (and its
    // acquires, first draws and slow uniforms), submit, waiting for the GPU,
    // Evict
    double pre_plan_ms = 0, pre_walk_ms = 0, pre_targets_ms = 0, pre_arrays_ms = 0;
    double pre_upload_ms = 0, pre_record_ms = 0, pre_acquire_ms = 0, pre_first_draw_ms = 0;
    double pre_uniform_slow_ms = 0, pre_draw_slow_ms = 0, pre_submit_ms = 0;
    uint32_t pre_uniform_slow = 0;
    double pre_wait_ms = 0, pre_evict_ms = 0;
    uint32_t pre_targets_made = 0, pre_arrays_grown = 0, pre_submits = 0;
    double pre_arrays_mb = 0;
    // plan_ms's parts (world passes' are in pre_ms): targets, outputs and
    // kept buffers made ready (setup); the walk placing meshes and textures
    // and making targets and arrays (walk); new meshes placed in the arena
    // (arena); pool and bone buffers grown (reserve). post_plan_ms is
    // post::PlanPost's, within record_ms.
    double plan_setup_ms = 0, plan_walk_ms = 0, plan_arena_ms = 0, plan_reserve_ms = 0;
    double post_plan_ms = 0;
    // In the walk: render targets made (colour + depth each): new (none kept
    // by its DxTex), resized (other size or mip count), returning (new, but
    // its DxTex had one before), and their time; textures and meshes drawn
    // for the first time since placed; texture arrays made or doubled, their
    // time (copy listing included) and megabytes (mips a third more)
    uint32_t targets_made = 0, targets_new = 0, targets_resized = 0, targets_returning = 0;
    double targets_ms = 0;
    // native_view_premake_targets (target_premake.h): targets the walk found
    // made ahead (not in targets_made); made ahead between the last frame
    // and this one (GpuRenderer::Idle), and their ms (not in ms); made ahead
    // and not yet used, and the shared depth textures, after the frame
    uint32_t premade_used = 0, targets_premade = 0, premade_unused = 0, shared_depths = 0;
    double premake_ms = 0;
    // native_view_premake_arrays: texture arrays made ahead between the
    // last frame and this one (their ms in premake_ms), and those the walk
    // placed a first texture in
    uint32_t arrays_premade = 0, arrays_premade_used = 0;
    uint32_t textures_first = 0, meshes_first = 0;
    uint32_t arrays_grown = 0;
    double arrays_ms = 0, arrays_mb = 0;
    // the rebuilt arena's new buffers' megabytes
    double arena_new_mb = 0;
    // buffers reserve grew, and each one's KB before and after (0, 0 if not)
    uint32_t reserve_grew = 0;
    uint32_t pool_verts_kb[2] = {}, pool_indices_kb[2] = {}, bones_kb[2] = {};
    // showed the kept post buffer in place of its world
    // (RasterOptions::post_buffer); back-buffer world draws drawn (before
    // post_boundary; none when it showed the buffer)
    bool shows_kept = false;
    uint32_t world_draws = 0;
    // native_world_ahead. World frame: ms drawing its world ahead
    // (RenderWorldAhead, not in ms), or gated (AheadChooser). Post frame:
    // whether it used that scene, and world texture passes it still drew
    // itself (0 expected)
    double ahead_ms = 0;
    uint32_t ahead_gated = 0, ahead_used = 0, ahead_fallback_passes = 0;
    // meshes sent from the CPU into this frame's pool, moved from the last
    // frame's pool into the arena on the GPU, and sent from the CPU into the
    // arena (on rebuild); bytes sent of meshes, textures and bones
    uint32_t pool_meshes = 0, arena_moved = 0, arena_sent = 0;
    bool arena_rebuilt = false;
    uint64_t mesh_bytes = 0;
    uint32_t textures_sent = 0;
    uint64_t texture_bytes = 0, bone_bytes = 0;
    // a rebuild's meshes copied from the old arena on the GPU
    uint32_t arena_copied = 0;
    // let go early for room: textures so their array needn't grow, meshes
    // where a rebuild would exceed the arena cap
    uint32_t textures_pressured = 0, meshes_pressured = 0;
    // device objects made (buffers include the upload buffer; textures
    // include arrays, targets, outputs, kept buffers), and what Evict let go:
    // render targets forgotten (at kEvictAfter frames) and of those, the ones
    // whose textures were released
    uint32_t pipelines_made = 0, buffers_made = 0, textures_made = 0;
    uint32_t evicted_meshes = 0, evicted_textures = 0, evicted_rts = 0, rts_released = 0;
    // resident after the frame: meshes, textures, render targets (forgotten
    // included); MB of texture arrays, arena and render targets (colour and
    // depth 4 bytes a pixel, mips a third more); meshes and textures kept by
    // the clock alone (undrawn kEvictAfter frames, not yet kKeepSeconds)
    uint32_t resident_meshes = 0, resident_textures = 0, resident_rts = 0;
    uint32_t meshes_by_time = 0, textures_by_time = 0;
    double texture_array_mb = 0, arena_mb = 0, rts_mb = 0;
    // native_gpu_timestamps, D3D12 only, once the GPU has finished the frame
    // (gpu_timed): GPU ms by part (gpu_timing_model.h), world passes and
    // world-ahead included; total excludes kIdle. Marks dropped by a full
    // ladder (time charged to the part before), spans unwritten or backwards.
    bool gpu_timed = false;
    double gpu_ms[gpu_timing::kParts] = {};
    double gpu_total_ms = 0;
    uint32_t gpu_marks_dropped = 0, gpu_bad_spans = 0;
};

// Whether a frame submits at its resolve and continues in a new command
// buffer (submit_points >= 1), so the GPU draws the world while the CPU
// records post, overlay and gamma. Not for `aside` work (a world pass before
// the frame, world-ahead), which is submitted at once anyway, nor with no
// draws since the last submission (nothing for the GPU to start on).
inline bool SubmitAtResolve(uint32_t submit_points, bool aside, uint32_t draws_since_submit) {
    return submit_points >= 1 && !aside && draws_since_submit > 0;
}

// GPU residency between frames, by frame serial (one per frame drawn and per
// world pass before a frame, kPreBufferPasses).
//
// Geometry and textures drawn in two frames live in the arena / their array
// until undrawn for kEvictAfter frames and, in a song (loading screen to
// results), when drawn by frames of more than one world (ClockKeep), for
// kKeepSeconds as well (WithinKeep). RB3 stops drawing a character while
// it's out of shot, and without the clock the cut back re-sent it all (one
// cut: 27 MB of textures, 10 MB of meshes, ~9 ms). Seconds, so the keep
// doesn't shrink as the frame rate rises. Song only because arrays never
// shrink: menu textures kept into a song stopped the menus' arrays emptying,
// and each menu-to-song trip could double a size class (744 to 935 MB of
// VRAM). The clock's keep also holds decoded data in memory (~100 MB).
//
// What's drawn in one frame only (particles, mutable meshes, movie frames, a
// render target's guest pixels) goes after `keep` undrawn frames
// (ResidencyKeepFrames), never by the clock. Under even/odd rendering the
// world is drawn once per world period, so a shorter keep would re-send the
// world each post frame (30 to 50 MB a frame in arena_04).
//
// What the clock keeps is let go early for room once undrawn kEvictAfter
// frames: a full texture array evicts its own before it grows. The arena is
// appended to and, when full, rebuilt at twice what it keeps (copied on the
// GPU), bounded by kMaxArenaBytes (ArenaRebuildKeep).
//
// A texture pass's render target is forgotten (drawn and drawn_in reset, so
// pictures match a release) once undrawn and unsampled for kEvictAfter
// frames, but its textures are kept kKeepSeconds since last use: re-making a
// character's 13 to 15 targets at a cut cost ~0.9 ms each. Past kMaxRts the
// least recently used forgotten ones are released at once.

// frames something drawn in one frame is kept undrawn, with the world drawn
// every `world_period` frames: the period plus room for a frame drawn twice
// (new options, a screenshot) and its world passes; 0 with the world drawn
// every frame or never
inline uint64_t ResidencyKeepFrames(uint32_t world_period) {
    return world_period > 1 ? world_period + 1 + kPreBufferPasses : 0;
}
// something drawn in more than one frame goes once both `evict_after` frames
// and `keep_seconds` have passed; keep_seconds 0 (short of room): frames alone
inline bool WithinKeep(uint64_t used, uint64_t serial, uint64_t evict_after, double idle_seconds,
                       double keep_seconds) {
    return used + evict_after >= serial || (keep_seconds > 0 && idle_seconds <= keep_seconds);
}
// arena meshes by WithinKeep; pool meshes by `keep` frames alone
inline bool KeepMesh(uint64_t used, bool in_arena, uint64_t serial, uint64_t keep,
                     uint64_t evict_after, double idle_seconds, double keep_seconds) {
    if (used == serial) return true;
    return in_arena ? WithinKeep(used, serial, evict_after, idle_seconds, keep_seconds)
                    : used + keep >= serial;
}
// as KeepMesh, drawn in more than one frame counting as in the arena
inline bool KeepTexture(uint64_t first, uint64_t used, uint64_t serial, uint64_t keep,
                        uint64_t evict_after, double idle_seconds, double keep_seconds) {
    return KeepMesh(used, used != first, serial, keep, evict_after, idle_seconds, keep_seconds);
}
// KeepMesh's keep_seconds: only in a song (RasterOptions::clock_keep), for
// things last drawn in it (so menu textures aren't kept into it) by frames
// of more than one world (FrameCapture::world_frame; what the capture makes
// anew each game frame is redrawn only within one world); else 0
inline double ClockKeep(bool in_song, bool drawn_in_song, bool across_worlds,
                        double keep_seconds) {
    return in_song && drawn_in_song && across_worlds ? keep_seconds : 0;
}
// What a rebuild (at twice the kept size) keeps: everything within its keep
// (kKeep); if twice `keep_bytes` (including the frame's new meshes) exceeds
// `cap` (kMaxArenaBytes), those drawn within kEvictAfter frames (kFrames);
// if that exceeds it too, only the frame's own (kFrame).
enum class ArenaKeep { kKeep, kFrames, kFrame };
inline ArenaKeep ArenaRebuildKeep(uint64_t keep_bytes, uint64_t frames_bytes, uint64_t cap) {
    if (2 * keep_bytes <= cap) return ArenaKeep::kKeep;
    if (2 * frames_bytes <= cap) return ArenaKeep::kFrames;
    return ArenaKeep::kFrame;
}
inline bool KeptByRebuild(ArenaKeep what, uint64_t used, uint64_t serial, uint64_t evict_after,
                          double idle_seconds, double keep_seconds) {
    switch (what) {
        case ArenaKeep::kKeep:
            return KeepMesh(used, true, serial, 0, evict_after, idle_seconds, keep_seconds);
        case ArenaKeep::kFrames: return KeepMesh(used, true, serial, 0, evict_after, 0, 0);
        case ArenaKeep::kFrame: break;
    }
    return used == serial;
}
// a mesh drawn again moves into the arena on the GPU only from the last
// frame's pool (pools alternate); otherwise it's re-sent from the CPU
inline bool MeshFromLastPool(uint64_t first, uint64_t serial) { return first + 1 == serial; }

// a render target is kept within `evict_after` frames; after that forgotten
// (its picture, not its textures), and released once also idle more than
// `keep_seconds`
enum class RtResidency { kKeep, kForget, kRelease };
inline RtResidency KeepRt(uint64_t used, uint64_t serial, uint64_t evict_after,
                          double idle_seconds, double keep_seconds) {
    if (used + evict_after >= serial) return RtResidency::kKeep;
    return idle_seconds > keep_seconds ? RtResidency::kRelease : RtResidency::kForget;
}
// How many of `forgotten` ({used, key}) to release to bring `resident` down
// to `cap` (kMaxRts); they're sorted to the front, oldest first (ties by key,
// independent of map order).
inline size_t RtsOverCap(std::vector<std::pair<uint64_t, uint32_t>>& forgotten, size_t resident,
                         size_t cap) {
    if (resident <= cap) return 0;
    const size_t n = std::min(resident - cap, forgotten.size());
    std::partial_sort(forgotten.begin(), forgotten.begin() + std::ptrdiff_t(n), forgotten.end());
    return n;
}

// one of the presenter's output textures, as RenderFrameToOutput left it
struct GpuOutput {
    // ID3D12Resource for the presenter to sample in place; null if not
    // Windows or CheckZeroCopy failed
    void* d3d12_resource = nullptr;
    uint32_t width = 0, height = 0;
    // bumped when the texture is remade, so older views are known stale
    uint64_t generation = 0;
};

class GpuRenderer {
 public:
    static GpuRenderer& Get();

    // Makes the device once; false from then on if it couldn't (logged once).
    // UI thread only: SDL starts its video subsystem on the main thread.
    bool Init();
    bool Ready();
    // Makes every pipeline (overlay's at 2 samples and overlay_samples) and
    // the upload buffer once, on the UI thread as the native renderer turns
    // on, so the first frames don't wait. Later pipelines are logged
    // ("pipeline made after warm-up").
    void Prewarm(uint32_t overlay_samples);
    // The worker, waiting for a capture, once Prewarm has run. The first
    // time it grows SDL_gpu's Direct3D 12 pools (command buffers and their
    // fences, uniform buffers, descriptor heap pairs) to what a song's
    // biggest frames take, which they otherwise make as they record: 7 ms
    // for a command buffer, ~0.5 for a uniform buffer, ~2 for a heap pair
    // (gpu_view.cpp's Impl::WarmPools; about 130 ms), so neither the UI
    // thread nor a frame waits for them. Then, with
    // RasterOptions::premake_targets and premake_arrays, it makes a few
    // announced render targets and texture arrays ahead (target_premake.h;
    // sized by `options`). Cheap with nothing to do. Whether announcements
    // are left that it could make now, to call again soon.
    bool Idle(const RasterOptions& options);
    // Draws `frame` at options.width x options.height into rgba (R in the low
    // byte, alpha 0xff) via its own output texture and readback. Any thread,
    // one frame at a time. False without a device or if the GPU failed
    // (logged).
    bool RenderFrame(const FrameCapture& frame, const RasterOptions& options,
                     std::vector<uint32_t>& rgba, GpuStats& stats);

    // Draws into presenter output `slot` (remade at options' size if needed)
    // and returns once submitted. The presenter, on the SDK's queue, mustn't
    // sample it until OutputDone: nothing else orders it after SDL's queue
    // (present_model.h's PresentSlots). False as RenderFrame.
    static constexpr int kOutputs = 3;
    bool RenderFrameToOutput(const FrameCapture& frame, const RasterOptions& options, int slot,
                             GpuOutput& out, GpuStats& stats);
    // Whether the GPU has finished `slot`'s last frame; never waits (SDL_gpu's
    // wait has no timeout and the caller watches for hangs), so poll it. True
    // with nothing in flight or no device. Once finished, fills `times` with
    // its GPU timings if it has them.
    bool OutputDone(int slot, GpuStats* times = nullptr);
    // native_world_ahead: draws a world frame's world alone (texture passes,
    // back-buffer draws before post_boundary, kept pre-process buffer) into
    // the scene target, submitted without waiting, so the next composed post
    // frame (RasterOptions::world_ahead) only post-processes it and draws the
    // overlay. Its fence, later on SDL_gpu's one queue, covers this too. Any
    // frame in between, or a size change, and the post frame draws its world
    // itself. False as RenderFrame.
    bool RenderWorldAhead(const FrameCapture& world, const RasterOptions& options,
                          GpuStats& stats);
    // false if no picture or no device
    bool DownloadOutput(int slot, std::vector<uint32_t>& rgba, uint32_t& width,
                        uint32_t& height);
    // RasterOptions::gpu_labels, for a GPU hang's DRED report: the last
    // submitted frame's DrawIndexedInstanced `before` into a command list of
    // `total`, if one of its command buffers had `total`; else why not. Any
    // thread incl. the crash trace: never waits for the lock ("" if busy).
    std::string DescribeIndexedDraw(uint32_t before, uint32_t total);
    // The SDK's ID3D12Device (null if its presenter isn't D3D12), which the
    // outputs must live on. UI thread, before CheckZeroCopy.
    // `timestamp_frequency`: its direct queue's ticks a second (0: no GPU
    // timings), shared by SDL_gpu's direct queue on the same device.
    void SetPresentDevice(void* d3d12_device, uint64_t timestamp_frequency = 0);
    // No device this session (the SDK's GPU mustn't be shared), and why. UI
    // thread, before Init; after it, the device is let go and the native view
    // draws on the CPU from the next frame.
    void RefuseDevice(std::string why);
    // Whether outputs can be sampled in place (GpuOutput::d3d12_resource): on
    // Windows a test texture's SDL internals (create info, container and
    // index, resource's device and description) are checked once, and every
    // later output too; the first failure turns it off for the session. Any
    // thread; the first call waits for a frame being drawn.
    bool CheckZeroCopy(std::string& why);
    // UI thread at shutdown; RenderFrame fails after
    void Shutdown();

 private:
    GpuRenderer();
    ~GpuRenderer();
    // slot -1: RenderFrame's own output, read back into rgba
    bool Draw(const FrameCapture& frame, const RasterOptions& options, int slot,
              std::vector<uint32_t>* rgba, GpuStats& stats);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace band3::render
