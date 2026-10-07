#pragma once

#include <memory>
#include <string>

#include "src/Render/scene_capture.h"

// Experimental: a FrameCapture on disk, for offline work in
// tools/native_view_replay. Textures over 512 are downsampled, from their mip
// chain where they have one.

namespace band3::render {

bool SaveCapture(const std::string& path, const FrameCapture& frame);
std::shared_ptr<FrameCapture> LoadCapture(const std::string& path);

}  // namespace band3::render
