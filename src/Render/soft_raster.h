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
    bool lighting = true;   // simple directional light on materials that aren't prelit
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

// rgba is width * height RGBA8, R in the low byte
RasterStats Rasterize(const FrameCapture& frame, const RasterOptions& options,
                      std::vector<uint32_t>& rgba);

}  // namespace band3::render
