#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Experimental: RGBA8 (R in the low byte) to an uncompressed PNG, so the
// native view needs no image library.

namespace band3::render {

bool WritePng(const std::string& path, const std::vector<uint32_t>& rgba, uint32_t w,
              uint32_t h);

}  // namespace band3::render
