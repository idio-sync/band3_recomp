#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
// the back buffer's draws, with the texture's mips made after it; the
// materials' textures, mip chains and all, filtered by the game's samplers in
// the shader (sample_model.h; its LOD from ddx_fine/ddy_fine, which the CPU
// works out the same way), not by hardware samplers, which couldn't wrap a
// texture in the corner of a bigger array layer (gpu_view.cpp's SizeClass);
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
    // it showed the post buffer kept from the last post frame in place of
    // its world (RasterOptions::post_buffer), and the back buffer's world
    // draws it drew (before post_boundary; none when it showed the buffer)
    bool shows_kept = false;
    uint32_t world_draws = 0;
    // meshes sent from the CPU this frame into its pool (new, or not drawn
    // the frame before), moved from the last frame's pool into the arena on
    // the GPU, and sent from the CPU into the arena (rebuilt: arena_rebuilt);
    // their bytes from the CPU, the textures sent and theirs, and the bones'
    uint32_t pool_meshes = 0, arena_moved = 0, arena_sent = 0;
    bool arena_rebuilt = false;
    uint64_t mesh_bytes = 0;
    uint32_t textures_sent = 0;
    uint64_t texture_bytes = 0, bone_bytes = 0;
    // the device objects made for it (pipelines; buffers, the upload buffer
    // included; textures: arrays grown, targets, outputs, kept buffers), and
    // what Evict let go of after it
    uint32_t pipelines_made = 0, buffers_made = 0, textures_made = 0;
    uint32_t evicted_meshes = 0, evicted_textures = 0, evicted_rts = 0;
    // what's on the GPU after it: meshes and textures kept, render targets,
    // and the megabytes of the texture arrays (their mips counted as a third
    // more) and of the arena
    uint32_t resident_meshes = 0, resident_textures = 0, resident_rts = 0;
    double texture_array_mb = 0, arena_mb = 0;
};

// What GpuRenderer keeps on the GPU between frames, by its frame serial (one
// a frame drawn, and one each world pass before a frame, kPreBufferPasses).
// Geometry drawn in two frames lives in the arena, and a texture drawn in two
// frames in its array, until no frame has drawn them for kEvictAfter frames;
// geometry and textures drawn in one frame only (particles, mutable meshes,
// movie frames, a render target's guest pixels) are let go once they've gone
// undrawn for `keep` frames (ResidencyKeepFrames). Under even/odd rendering
// the world's draws are drawn by one frame in every world period (its post
// frame: the others show the kept post buffer), so a keep shorter than that
// would let them go between, and each post frame would send all of the
// world's geometry and textures again (30 to 50 MB a frame in arena_04).

// the frames something drawn in one frame is kept undrawn for, with the world
// drawn every `world_period` frames (RasterOptions::world_period): the
// period, and room for a frame drawn twice (new options, a screenshot) and a
// frame's world passes, which each take a serial; 0 (let go once a frame
// hasn't drawn it) with the world drawn every frame or not at all
inline uint64_t ResidencyKeepFrames(uint32_t world_period) {
    return world_period > 1 ? world_period + 1 + kPreBufferPasses : 0;
}
// whether a mesh last drawn in frame `used` is kept after frame `serial`:
// drawn in it, in the arena and drawn within `evict_after` frames
// (kEvictAfter), or else drawn within `keep`
inline bool KeepMesh(uint64_t used, bool in_arena, uint64_t serial, uint64_t keep,
                     uint64_t evict_after) {
    return used == serial || used + (in_arena ? evict_after : keep) >= serial;
}
// whether a texture first drawn in frame `first` and last in `used` is kept
// after frame `serial`: as a mesh, drawn in more than one frame counting as
// in the arena
inline bool KeepTexture(uint64_t first, uint64_t used, uint64_t serial, uint64_t keep,
                        uint64_t evict_after) {
    return KeepMesh(used, used != first, serial, keep, evict_after);
}
// whether a mesh drawn again in frame `serial`, first drawn (into its frame's
// pool) in frame `first`, moves to the arena from that pool on the GPU: only
// from the last frame's, as the pools alternate; else it's sent again from
// the CPU, into the arena
inline bool MeshFromLastPool(uint64_t first, uint64_t serial) { return first + 1 == serial; }

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
    // True with no frame in flight there, or no device.
    bool OutputDone(int slot);
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
    // On the UI thread, before CheckZeroCopy.
    void SetPresentDevice(void* d3d12_device);
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
