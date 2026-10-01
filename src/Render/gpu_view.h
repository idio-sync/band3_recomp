#pragma once

#include <cstdint>
#include <memory>
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
// the GPU, depth as 1/w.

namespace band3::render {

struct GpuStats {
    uint32_t draws = 0;
    uint32_t skipped = 0;   // draws it couldn't do (no geometry, or no pipeline)
    uint32_t uploads = 0;   // meshes and textures sent to the GPU this frame
    double ms = 0;          // the whole frame: uploads, drawing and reading back
    double wait_ms = 0;     // of that, from submitting to having the picture
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
    // byte, alpha 0xff). Any thread, one frame at a time. False without a
    // device or if the GPU failed (logged).
    bool RenderFrame(const FrameCapture& frame, const RasterOptions& options,
                     std::vector<uint32_t>& rgba, GpuStats& stats);
    // releases the device, on the UI thread at shutdown; RenderFrame fails after
    void Shutdown();

 private:
    GpuRenderer();
    ~GpuRenderer();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace band3::render
