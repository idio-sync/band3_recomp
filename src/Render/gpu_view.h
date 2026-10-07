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

// Experimental: draws a FrameCapture on the GPU, through SDL_gpu, for the
// native view (native_view_backend = gpu). It renders offscreen and reads the
// picture back, so it hands back the same RGBA as Rasterize() and the native
// view shows it the same way; soft_raster.cpp stays the reference it is checked
// against (tools/native_view_replay --diff) and the fallback.
//
// band3.exe links its own static SDL, separate from the one in rexruntime that
// owns the game window. The device lives in band3's copy, on SDL's offscreen
// video driver, so it makes no windows and leaves the game's alone.
//
// It draws what Rasterize() does, the same way: every blend mode, skinning on
// the GPU, depth as 1/w, and the texture passes the frame samples, each into a
// render target of its own (kept between frames, by DxTex) between stretches of
// the back buffer's draws, with the texture's mips made after it (by
// shaders/mips.hlsl in the frame's command buffer, as SDL's mipmap blits
// would: RasterOptions::inline_mips); the
// materials' textures, mip chains and all, filtered by the game's samplers in
// the shader (sample_model.h; its LOD from ddx_fine/ddy_fine, which the CPU
// works out the same way), not by hardware samplers, which couldn't wrap a
// texture in the corner of a bigger array layer (gpu_view.cpp's SizeClass),
// the block-compressed ones kept as BC1, BC2, BC3 and BC5 in arrays of their
// own (RasterOptions::bc_textures), the rest as RGBA8;
// and the world's draws into a scene target whose colour, alpha and depth
// RB3's post-processing (post_model.h), full-screen passes (shaders/
// post.hlsl), reads into the picture before the overlay's draws. The spotlights' cones
// (spot_model.h) shade by mesh.hlsl's PSSpotCone, reading the scene's depth,
// and the depth volume's blurs blur a copy of it into it with post.hlsl's
// blur; the soft particles (scene_capture.h's IsSoftParticle) by
// PSSoftParticle, which fades them by that depth, and their buffer's blurs
// blur a copy of one surface into the other. A shadow map's pass draws its
// depth (clip z/w) into an R32_FLOAT target of its own by PSShadowDepth, LESS
// against a depth buffer, and the SHADOW_BUFFER draws after it read four of
// its texels (soft_raster.h's RasterOptions::self_shadow); NgLight's shadow,
// its casters' silhouettes blurred twice in place, is a texture pass like
// the depth volume's, which the projected light's draws read as their s5.
// The display's gamma ramp (gamma_ramp.h) goes over the finished picture
// last, by shaders/gamma.hlsl's pass, into an output texture the frame is
// read back from; without a ramp the same pass with an identity lookup,
// which is exact for 8 bits, so every frame ends in it. Every target is the
// size the CPU's is (soft_raster.h's PassTargetSize: the screen's passes in
// proportion to a picture bigger than the game's).
//
// The overlay is multisampled as the CPU's is (soft_raster.h's
// OverlaySamples): into a colour and a depth target of 2 (or 4) samples a
// pixel, which start as the picture (copied into every sample by
// post.hlsl's PSOverlayStart) and a cleared depth, and resolve into the
// picture as each of the overlay's passes ends (SDL_GPU_STOREOP_RESOLVE_AND_
// STORE: the mean of each pixel's samples). Its pipelines are made for that
// sample count; where the device can't draw it, the other of 2 and 4, else 1
// (logged).
//
// For the native renderer's presentation (the native picture shown) that pass writes
// one of kOutputs textures the SDK's presenter samples where they are, no
// readback: on Windows SDL_gpu's Direct3D 12 device is the SDK's own (one
// device per adapter in a process), so the ID3D12Resource behind SDL's
// texture, reached through SDL 3.4.14's private texture layout and checked
// before use (CheckZeroCopy), is a texture the SDK's command list can read.
// Sharing it is also why no device is made on Microsoft's software
// rasterizer (WARP, the Basic Render Driver, vendor 0x1414): there the shared
// device faults inside WARP (an access violation that kills band3) once
// band3's frames and the emulated GPU's run on it together, whether or not
// they're presented in place; native_view.cpp refuses the device
// (RefuseDevice) and the native view draws on the CPU.
//
// With RasterOptions::gpu_timestamps (native_gpu_timestamps) on that device,
// a frame writes a timestamp at each boundary between its parts
// (gpu_timing_model.h) into SDL's command list itself, reached the way the
// textures are, through SDL 3.4.14's private command buffer layout, checked
// once (CheckTimingOnce) and its pointers again at every command buffer; the
// frame's last command buffer resolves them into a readback buffer, read
// once the GPU has finished it (GpuStats::gpu_ms). Timestamps change no
// state SDL tracks (no pipeline, binding, barrier or descriptor heap), so
// the picture and SDL's sampler batches (kSamplerBatch) are as without.
// Where the checks fail, or on Vulkan, there are no timings (logged once)
// and nothing else changes.

namespace band3::render {

struct GpuStats {
    uint32_t draws = 0;
    uint32_t skipped = 0;   // draws it couldn't do (no geometry, or no pipeline)
    uint32_t uploads = 0;   // meshes and textures sent to the GPU this frame
    uint32_t passes = 0;    // texture passes drawn
    uint32_t rt_missing = 0;  // draws that sampled a render target nothing had drawn
    // RenderFrame's whole frame: uploads, drawing and reading back; and of
    // that, from submitting to having the picture. RenderFrameToOutput's
    // frame up to its submission alone, with no wait (0): the GPU's time
    // after is its caller's to wait out (OutputDone).
    double ms = 0;
    double wait_ms = 0;
    // The native renderer's worker adds into ms the time it took the capture
    // decoding what the game's thread left to decode (scene_capture.h's
    // LatestCapture: textures and meshes seen first), before the frame:
    // decode_ms
    double decode_ms = 0;
    // Where a frame's time went and what it had to do, for the native
    // renderer's slow-frame log and its numbers by kind of frame
    // (native_view.cpp). The worker's milliseconds: the world passes before
    // the frame (pre_passes of them, kPreBufferPasses for a refracting world
    // with no pre-process buffer kept), working out what it draws and placing
    // its meshes and textures, filling the upload buffer, recording its
    // passes, submitting them (part of wait_ms, which starts at the
    // submission, when the frame is waited for), and letting go of what no
    // frame draws (Evict), after wait_ms.
    double pre_ms = 0, plan_ms = 0, upload_ms = 0, record_ms = 0, submit_ms = 0, evict_ms = 0;
    uint32_t pre_passes = 0;
    // plan_ms's parts, the frame's own (its world passes' are in pre_ms):
    // its targets, outputs and kept buffers made ready (setup); the walk over
    // its passes and draws, placing each mesh and texture it draws and making
    // the render targets and texture arrays they need (walk); its new meshes
    // placed in the arena (arena, rebuilt or not); the pool's and the bones'
    // buffers grown to hold the frame (reserve). post_plan_ms is
    // post::PlanPost's, in record_ms.
    double plan_setup_ms = 0, plan_walk_ms = 0, plan_arena_ms = 0, plan_reserve_ms = 0;
    double post_plan_ms = 0;
    // In the walk: the render targets made (a colour and a depth texture
    // each), new (none kept by its DxTex: never drawn, or released) or made
    // again at another size or mip count (resized), and of the new, those
    // whose DxTex had one before (released, and back: returning), and the
    // time it took; the textures drawn for the first time since they were
    // placed (or let go), and the meshes; the texture arrays made or doubled
    // to hold them, their time (the old one's layers listed for the copy
    // included) and their megabytes (mips a third more)
    uint32_t targets_made = 0, targets_new = 0, targets_resized = 0, targets_returning = 0;
    double targets_ms = 0;
    uint32_t textures_first = 0, meshes_first = 0;
    uint32_t arrays_grown = 0;
    double arrays_ms = 0, arrays_mb = 0;
    // the arena's new buffers' megabytes, if it was rebuilt (arena_rebuilt)
    double arena_new_mb = 0;
    // the buffers reserve grew, and each one's kilobytes before and after
    // (0 and 0 where it didn't): the pool's vertices and indices, the bones
    uint32_t reserve_grew = 0;
    uint32_t pool_verts_kb[2] = {}, pool_indices_kb[2] = {}, bones_kb[2] = {};
    // it showed the post buffer kept from the last post frame in place of
    // its world (RasterOptions::post_buffer), and the back buffer's world
    // draws it drew (before post_boundary; none when it showed the buffer)
    bool shows_kept = false;
    uint32_t world_draws = 0;
    // native_world_ahead: on a world frame, the worker's milliseconds drawing
    // its world ahead (RenderWorldAhead, not in ms), or that it didn't, as it
    // skipped more captures than not (gated: present_model.h's
    // AheadChooser); on a post frame, whether it post-processed that scene
    // (1), and the world's texture passes it drew itself all the same, the
    // world ahead not having drawn their targets (0 expected)
    double ahead_ms = 0;
    uint32_t ahead_gated = 0, ahead_used = 0, ahead_fallback_passes = 0;
    // meshes sent from the CPU this frame into its pool (new, or not drawn
    // the frame before), moved from the last frame's pool into the arena on
    // the GPU, and sent from the CPU into the arena (rebuilt: arena_rebuilt);
    // their bytes from the CPU, the textures sent and theirs, and the bones'
    uint32_t pool_meshes = 0, arena_moved = 0, arena_sent = 0;
    bool arena_rebuilt = false;
    uint64_t mesh_bytes = 0;
    uint32_t textures_sent = 0;
    uint64_t texture_bytes = 0, bone_bytes = 0;
    // a rebuild's meshes kept from the old arena, copied into the new one on
    // the GPU (those it sent from the CPU are among arena_sent)
    uint32_t arena_copied = 0;
    // let go before their keep was out, for room (the residency below):
    // textures, so that their array wouldn't grow, and meshes, at a rebuild
    // that would have made the arena bigger than its cap
    uint32_t textures_pressured = 0, meshes_pressured = 0;
    // the device objects made for it (pipelines; buffers, the upload buffer
    // included; textures: arrays grown, targets, outputs, kept buffers), and
    // what Evict let go of after it (the meshes past their keep an arena
    // rebuild let go among them): the render targets forgotten (their
    // pictures, at kEvictAfter frames, as when they were released there) and,
    // of those forgotten, the ones whose textures were released (residency
    // below)
    uint32_t pipelines_made = 0, buffers_made = 0, textures_made = 0;
    uint32_t evicted_meshes = 0, evicted_textures = 0, evicted_rts = 0, rts_released = 0;
    // what's on the GPU after it: meshes and textures kept, render targets
    // (forgotten ones included), and the megabytes of the texture arrays
    // (their mips counted as a third more), of the arena and of the render
    // targets (colour and depth, 4 bytes a pixel each, the colour's mips a
    // third more); and of the meshes and textures, those kept by the clock
    // alone (undrawn for kEvictAfter frames, not yet kKeepSeconds)
    uint32_t resident_meshes = 0, resident_textures = 0, resident_rts = 0;
    uint32_t meshes_by_time = 0, textures_by_time = 0;
    double texture_array_mb = 0, arena_mb = 0, rts_mb = 0;
    // native_gpu_timestamps (RasterOptions::gpu_timestamps), Direct3D 12
    // only: the GPU's milliseconds on the frame by part (gpu_timing_model.h),
    // the world passes before it and the world drawn ahead for it included,
    // and all of them but kIdle (the GPU waiting for the CPU between its
    // command buffers): its busy time. Once the GPU has finished it
    // (gpu_timed; else none).
    // The marks a full ladder dropped (their time charged to the part before)
    // and the spans left out as unwritten or backwards.
    bool gpu_timed = false;
    double gpu_ms[gpu_timing::kParts] = {};
    double gpu_total_ms = 0;
    uint32_t gpu_marks_dropped = 0, gpu_bad_spans = 0;
};

// What GpuRenderer keeps on the GPU between frames, by its frame serial (one
// a frame drawn, and one each world pass before a frame, kPreBufferPasses).
// Geometry drawn in two frames lives in the arena, and a texture drawn in two
// frames in its array, until no frame has drawn them for kEvictAfter frames
// and, in a song, if frames of more than one world drew them (ClockKeep),
// kKeepSeconds of the clock as well (WithinKeep): RB3 stops drawing a
// character while it's out of the shot, and when kEvictAfter frames (a second
// at 120 Hz) let it go, the cut back sent it all again (at one cut 51
// textures, 27 MB, and 233 meshes, 10 MB: 3 ms of upload and 5.6 of waiting
// for the GPU). Seconds as well as frames, so the keep doesn't shrink as the
// frame rate goes up. In a song only, from its loading screen to its
// results: arrays never shrink, only go once empty, and the menus' textures
// kept 30 s into a song's loading kept the menus' arrays from emptying, so
// the song's textures filled them and each menu-to-song trip could leave a
// size class an array doubling bigger (744 to 935 MB of video memory). What
// the clock keeps holds its Geometry and Texture, their decoded data, in
// memory too: about 100 MB more. Geometry and textures drawn in one frame
// only (particles, mutable meshes, movie frames, a render target's guest
// pixels) are let go once they've gone undrawn for `keep` frames
// (ResidencyKeepFrames), not kept by the clock; drawn again by the same
// world's frames, by kEvictAfter frames alone, as before. Under even/odd
// rendering the
// world's draws are drawn by one frame in every world period (its post frame:
// the others show the kept post buffer), so a keep shorter than that would
// let them go between, and each post frame would send all of the world's
// geometry and textures again (30 to 50 MB a frame in arena_04).
//
// What the clock keeps is let go for room before its time, those undrawn
// for kEvictAfter frames, as before the clock kept them: a texture array
// that's full lets go of its own before it grows (doubling it for textures
// kept idle would cost more than it saves). The arena is appended to and
// rebuilt when full, made again twice the size of what it keeps, which is
// copied over on the GPU rather than sent again; what it keeps is bounded by
// kMaxArenaBytes (ArenaRebuildKeep).
//
// A texture pass's render target is forgotten once no frame has drawn or
// sampled it for kEvictAfter frames: what it was drawn with is no longer
// there for a frame to sample (drawn and drawn_in reset), exactly as when the
// target was released there, so every frame's picture is the same as it was
// then. Its textures, though, are kept for kKeepSeconds of the clock since
// it was last used: the cut back to a character made each of its targets
// again (13 to 15 at a cut, about 0.9 ms each, most of a 12 to 25 ms plan at
// 120 Hz). At most kMaxRts are kept: past that the forgotten ones least
// recently used are released at once.

// the frames something drawn in one frame is kept undrawn for, with the world
// drawn every `world_period` frames (RasterOptions::world_period): the
// period, and room for a frame drawn twice (new options, a screenshot) and a
// frame's world passes, which each take a serial; 0 (let go once a frame
// hasn't drawn it) with the world drawn every frame or not at all
inline uint64_t ResidencyKeepFrames(uint32_t world_period) {
    return world_period > 1 ? world_period + 1 + kPreBufferPasses : 0;
}
// whether something drawn in more than one frame, last drawn in frame `used`
// and `idle_seconds` ago, is within its keep after frame `serial`: drawn
// within `evict_after` frames (kEvictAfter) or `keep_seconds` (kKeepSeconds),
// so it goes once both have passed, as a render target's textures do
// (KeepRt). With keep_seconds 0 (short of room) the frames alone.
inline bool WithinKeep(uint64_t used, uint64_t serial, uint64_t evict_after, double idle_seconds,
                       double keep_seconds) {
    return used + evict_after >= serial || (keep_seconds > 0 && idle_seconds <= keep_seconds);
}
// whether a mesh last drawn in frame `used`, `idle_seconds` ago, is kept
// after frame `serial`: drawn in it; in the arena and within its keep
// (WithinKeep); or else drawn within `keep` frames, the clock aside
inline bool KeepMesh(uint64_t used, bool in_arena, uint64_t serial, uint64_t keep,
                     uint64_t evict_after, double idle_seconds, double keep_seconds) {
    if (used == serial) return true;
    return in_arena ? WithinKeep(used, serial, evict_after, idle_seconds, keep_seconds)
                    : used + keep >= serial;
}
// whether a texture first drawn in frame `first` and last in `used` is kept
// after frame `serial`: as a mesh, drawn in more than one frame counting as
// in the arena
inline bool KeepTexture(uint64_t first, uint64_t used, uint64_t serial, uint64_t keep,
                        uint64_t evict_after, double idle_seconds, double keep_seconds) {
    return KeepMesh(used, used != first, serial, keep, evict_after, idle_seconds, keep_seconds);
}
// The seconds a mesh or texture drawn in more than one frame is kept by
// (KeepMesh's keep_seconds): `keep_seconds` while the game is in a song
// (`in_song`, RasterOptions::clock_keep), for one last drawn in it
// (`drawn_in_song`, so the menus' last textures aren't kept into it) by
// frames of more than one world (`across_worlds`, FrameCapture::world_frame:
// geometry and textures the capture keeps from one game frame to the next, a
// character's); else none, the frames alone, exactly as before the clock kept
// anything. What the capture makes anew each game frame (particles, DrawRect's
// quads, a movie's frames) is drawn again only by frames of the same world (a
// post frame composed with its world frame, a world pass before a frame).
inline double ClockKeep(bool in_song, bool drawn_in_song, bool across_worlds,
                        double keep_seconds) {
    return in_song && drawn_in_song && across_worlds ? keep_seconds : 0;
}
// What an arena rebuild keeps of the meshes in it (it's full: it's made again,
// twice the size of what goes in, and what it keeps is copied over on the
// GPU): every one within its keep (kKeep, KeepMesh's); if twice
// `keep_bytes`, those and the frame's new ones, is over `cap`
// (kMaxArenaBytes), those drawn within kEvictAfter frames (kFrames,
// `frames_bytes`); and if twice that is over too, only those the frame draws
// (kFrame), as before the clock kept them.
enum class ArenaKeep { kKeep, kFrames, kFrame };
inline ArenaKeep ArenaRebuildKeep(uint64_t keep_bytes, uint64_t frames_bytes, uint64_t cap) {
    if (2 * keep_bytes <= cap) return ArenaKeep::kKeep;
    if (2 * frames_bytes <= cap) return ArenaKeep::kFrames;
    return ArenaKeep::kFrame;
}
// whether a rebuild that keeps `what` keeps a mesh in the arena last drawn in
// frame `used`, `idle_seconds` ago, in frame `serial`
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
// whether a mesh drawn again in frame `serial`, first drawn (into its frame's
// pool) in frame `first`, moves to the arena from that pool on the GPU: only
// from the last frame's, as the pools alternate; else it's sent again from
// the CPU, into the arena
inline bool MeshFromLastPool(uint64_t first, uint64_t serial) { return first + 1 == serial; }

// what becomes of a render target last drawn or sampled in frame `used`, and
// `idle_seconds` ago, after frame `serial`: kept as it is while drawn within
// `evict_after` frames (kEvictAfter); after that forgotten (its picture, not
// its textures), and released once it's been idle more than `keep_seconds`
// (kKeepSeconds) too. Both must have passed: at 2 frames a second, a
// target last used 40 seconds ago is 80 frames back, and stays as it is.
enum class RtResidency { kKeep, kForget, kRelease };
inline RtResidency KeepRt(uint64_t used, uint64_t serial, uint64_t evict_after,
                          double idle_seconds, double keep_seconds) {
    if (used + evict_after >= serial) return RtResidency::kKeep;
    return idle_seconds > keep_seconds ? RtResidency::kRelease : RtResidency::kForget;
}
// With `resident` targets kept, more than `cap` (kMaxRts): of the forgotten
// ones in `forgotten` (their `used` frame, and a key), how many are released,
// least recently used first. They're moved to the front, oldest first (ties
// by key, so it doesn't depend on the map's order), and the count returned.
inline size_t RtsOverCap(std::vector<std::pair<uint64_t, uint32_t>>& forgotten, size_t resident,
                         size_t cap) {
    if (resident <= cap) return 0;
    const size_t n = std::min(resident - cap, forgotten.size());
    std::partial_sort(forgotten.begin(), forgotten.begin() + std::ptrdiff_t(n), forgotten.end());
    return n;
}

// one of the presenter's output textures, as RenderFrameToOutput left it
struct GpuOutput {
    // the ID3D12Resource behind it, which the SDK's presenter can sample in
    // place, or null where it can't (not Windows, or CheckZeroCopy failed)
    void* d3d12_resource = nullptr;
    uint32_t width = 0, height = 0;
    // new each time the texture is made again, so a view made of an earlier
    // one is known stale
    uint64_t generation = 0;
};

class GpuRenderer {
 public:
    static GpuRenderer& Get();

    // Makes the device the first time; false from then on if it couldn't
    // (logged once), and the native view stays on the CPU. Call it on the UI
    // thread: SDL starts its video subsystem on the main thread only.
    bool Init();
    // whether Init made a device
    bool Ready();
    // Makes every pipeline a frame can ask for and the upload buffer's usual
    // size, once, if Init made a device: on the UI thread as the native
    // renderer turns on, so its first frames don't wait for them (a frame
    // drawn before does it itself), the overlay's for the game's 2 samples a
    // pixel and for overlay_samples (RasterOptions::msaa, native_view_msaa).
    // A pipeline made after it is logged ("pipeline made after warm-up").
    void Prewarm(uint32_t overlay_samples);
    // Draws `frame` at options.width x options.height into rgba (R in the low
    // byte, alpha 0xff): into an output texture of its own, then read back.
    // Any thread, one frame at a time. False without a device or if the GPU
    // failed (logged).
    bool RenderFrame(const FrameCapture& frame, const RasterOptions& options,
                     std::vector<uint32_t>& rgba, GpuStats& stats);

    // The presenter's output textures, which RenderFrameToOutput draws into
    // and leaves on the GPU: output `slot` (0 to kOutputs - 1), made again at
    // options' size if it isn't that size. It returns once the frame is
    // submitted, so the caller can record the next while the GPU draws this
    // one, and OutputDone tells when the GPU has finished it: until then the
    // presenter mustn't sample it, as it does on the SDK's queue, which
    // nothing orders after SDL's but that. native_view.cpp keeps the
    // presenter from sampling a slot while it's drawn into or unfinished
    // (present_model.h's PresentSlots). False as RenderFrame.
    static constexpr int kOutputs = 3;
    bool RenderFrameToOutput(const FrameCapture& frame, const RasterOptions& options, int slot,
                             GpuOutput& out, GpuStats& stats);
    // Whether the GPU has finished the frame RenderFrameToOutput last drew
    // into `slot`: never waits (SDL_gpu's own wait has no timeout, and the
    // caller watches for a GPU that never finishes), so the caller polls it.
    // True with no frame in flight there, or no device. Once it's finished,
    // its GPU timings (GpuStats::gpu_ms, if it has them) into `times`; a
    // frame waited for before RenderFrameToOutput returned has them already.
    bool OutputDone(int slot, GpuStats* times = nullptr);
    // native_world_ahead: a world frame's world alone (its texture passes
    // and back-buffer draws before post_boundary, and the pre-process buffer
    // kept from it) into the scene target, submitted and not waited for, so
    // the composed post frame drawn next (RasterOptions::world_ahead) only
    // post-processes it and draws its overlay: the world's GPU time moves
    // into the world frame's half of the pair. That post frame's fence, after
    // it on SDL_gpu's one queue, waits it out too. Any frame drawn in
    // between, or at another size, and the post frame draws its world
    // itself. False as RenderFrame.
    bool RenderWorldAhead(const FrameCapture& world, const RasterOptions& options,
                          GpuStats& stats);
    // reads output `slot` back as RenderFrame's rgba, at its size; false if
    // it has no picture (or no device)
    bool DownloadOutput(int slot, std::vector<uint32_t>& rgba, uint32_t& width,
                        uint32_t& height);
    // With RasterOptions::gpu_labels, the last frame submitted's indexed
    // draws (DrawIndexedInstanced each, in order, a list per command buffer
    // the frame took), for a GPU hang's DRED report: the one `before` such
    // draws into a command list of `total`, and the frame, if one of the last
    // frame's command buffers had `total` of them (the list is likely that
    // one); else why not. Any thread, the crash trace's included: never waits
    // for the lock, "" if it can't have it.
    std::string DescribeIndexedDraw(uint32_t before, uint32_t total);
    // The SDK's ID3D12Device, which the outputs must live on for its presenter
    // to sample them in place, or null when its presenter isn't Direct3D 12.
    // On the UI thread, before CheckZeroCopy. `timestamp_frequency`: the
    // ticks a second of its direct queue's timestamps (0 unknown: no GPU
    // timings), which SDL_gpu's own direct queue on the same device shares.
    void SetPresentDevice(void* d3d12_device, uint64_t timestamp_frequency = 0);
    // No device for the session, and why (Init logs it once; CheckZeroCopy
    // gives it as its why not): the SDK's GPU is one SDL_gpu mustn't share.
    // On the UI thread, before Init; after it, a device made is let go and
    // the native view draws on the CPU from the next frame.
    void RefuseDevice(std::string why);
    // Whether the outputs can be sampled in place (GpuOutput::d3d12_resource):
    // on Windows, once the device has started, SDL's texture behind a test
    // texture is checked once (its create info, its texture's container and
    // index, its resource's device and description); every output made after
    // is checked too, and the first that fails turns this off for the
    // session. False and why not otherwise. Any thread; the first call waits
    // for a frame being drawn.
    bool CheckZeroCopy(std::string& why);
    // releases the device, on the UI thread at shutdown; RenderFrame fails after
    void Shutdown();

 private:
    GpuRenderer();
    ~GpuRenderer();
    // a frame into output `slot` (-1 RenderFrame's own, read back into rgba)
    bool Draw(const FrameCapture& frame, const RasterOptions& options, int slot,
              std::vector<uint32_t>* rgba, GpuStats& stats);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace band3::render
