#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Experimental: RGBA8 (R in the low byte) to an uncompressed PNG, so the
// native view needs no image library, and back from one written here (the
// test harness's screenshots). ReadPng takes nothing else: it can't inflate.

namespace band3::render {

bool WritePng(const std::string& path, const std::vector<uint32_t>& rgba, uint32_t w,
              uint32_t h);
bool ReadPng(const std::string& path, std::vector<uint32_t>& rgba, uint32_t& w, uint32_t& h);

}  // namespace band3::render
