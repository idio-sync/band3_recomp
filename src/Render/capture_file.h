#pragma once

#include <memory>
#include <string>

#include "src/Render/scene_capture.h"

// Experimental: a FrameCapture on disk, so the rasterizer can be worked on
// offline (tools/native_view_replay) without running the game each time.
// Textures larger than 512 are downsampled to keep the file small.

namespace band3::render {

bool SaveCapture(const std::string& path, const FrameCapture& frame);
std::shared_ptr<FrameCapture> LoadCapture(const std::string& path);

}  // namespace band3::render
