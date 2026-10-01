#pragma once

#include <cstdint>
#include <vector>

#include "src/Render/scene_capture.h"

// Experimental: draws a FrameCapture on the CPU. It stands in for a real
// GPU backend so the probe tests the captured data, not a graphics API.

namespace band3::render {

struct RasterOptions {
    uint32_t width = 640;
    uint32_t height = 360;
    bool textures = true;
    // RB3's lighting (shade_model.h) from each draw's ShadeState; off draws
    // every material unlit (ambient 1)
    bool lighting = true;
    // the placeholder lighting from before the game's (a fixed directional
    // light on materials that aren't prelit), which captures without shade
    // states always get: to compare the two on one capture
    bool legacy_light = false;
    bool skinning = true;
    bool blending = true;   // off draws every material opaque
    bool clear_depth_per_camera = true;
};

struct RasterStats {
    uint32_t draws = 0;
    uint32_t triangles = 0;
    uint32_t pixels = 0;
    double ms = 0;
};

// rgba is width * height RGBA8, R in the low byte; ids, if given, which of
// frame.draws last wrote each pixel (-1 none), to find what drew something
RasterStats Rasterize(const FrameCapture& frame, const RasterOptions& options,
                      std::vector<uint32_t>& rgba, std::vector<int32_t>* ids = nullptr);

}  // namespace band3::render
