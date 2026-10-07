#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Experimental: RGBA8 (R in the low byte) to and from uncompressed PNG, with
// no image library. ReadPng can't inflate, so it reads only PNGs written here.

namespace band3::render {

bool WritePng(const std::string& path, const std::vector<uint32_t>& rgba, uint32_t w,
              uint32_t h);
bool ReadPng(const std::string& path, std::vector<uint32_t>& rgba, uint32_t& w, uint32_t& h);

}  // namespace band3::render
