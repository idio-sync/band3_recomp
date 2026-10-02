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
// For the native renderer's presentation (renderer = native) that pass writes
// one of kOutputs textures the SDK's presenter samples where they are, no
// readback: on Windows SDL_gpu's Direct3D 12 device is the SDK's own (one
// device per adapter in a process), so the ID3D12Resource behind SDL's
// texture, reached through SDL 3.4.14's private texture layout and checked
// before use (CheckZeroCopy), is a texture the SDK's command list can read.

namespace band3::render {

struct GpuStats {
    uint32_t draws = 0;
    uint32_t skipped = 0;   // draws it couldn't do (no geometry, or no pipeline)
    uint32_t uploads = 0;   // meshes and textures sent to the GPU this frame
    uint32_t passes = 0;    // texture passes drawn
    uint32_t rt_missing = 0;  // draws that sampled a render target nothing had drawn
    double ms = 0;          // the whole frame: uploads, drawing and reading back
    double wait_ms = 0;     // of that, from submitting to having the picture
};

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
    // Draws `frame` at options.width x options.height into rgba (R in the low
    // byte, alpha 0xff): into an output texture of its own, then read back.
    // Any thread, one frame at a time. False without a device or if the GPU
    // failed (logged).
    bool RenderFrame(const FrameCapture& frame, const RasterOptions& options,
                     std::vector<uint32_t>& rgba, GpuStats& stats);

    // The presenter's output textures, which RenderFrameToOutput draws into
    // and leaves on the GPU, finished (it waits for the GPU): output `slot`
    // (0 to kOutputs - 1), made again at options' size if it isn't that size.
    // native_view.cpp keeps the presenter from sampling a slot while it's
    // drawn into (present_model.h's PresentSlots). False as RenderFrame.
    static constexpr int kOutputs = 3;
    bool RenderFrameToOutput(const FrameCapture& frame, const RasterOptions& options, int slot,
                             GpuOutput& out, GpuStats& stats);
    // reads output `slot` back as RenderFrame's rgba, at its size; false if
    // it has no picture (or no device)
    bool DownloadOutput(int slot, std::vector<uint32_t>& rgba, uint32_t& width,
                        uint32_t& height);
    // The SDK's ID3D12Device, which the outputs must live on for its presenter
    // to sample them in place, or null when its presenter isn't Direct3D 12.
    // On the UI thread, before CheckZeroCopy.
    void SetPresentDevice(void* d3d12_device);
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
