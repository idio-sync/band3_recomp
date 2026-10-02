#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "src/Render/scene_capture.h"

// Experimental: how the native view reads a material's textures, the way the
// game's samplers do (scene_capture.h's TexSampler, from the draws' fetch
// constants): filtered, between mip levels by the LOD of the pixel's
// footprint, with anisotropy and per-axis addressing. Both backends sample
// with the same code, src/Render/shaders/sample_model.hlsli, which mesh.hlsl
// compiles for the GPU and soft_raster.cpp as C++ for the CPU, from the
// sampler packed as a uint4 (PackSampler). A sampler that isn't the game's
// (TexSampler::filtered 0: none bound, a capture from before they were kept,
// or RasterOptions::filtering off) is read as before, nearest and wrapping
// at level 0, by each backend's Texel().

namespace band3::render {

// A texture's levels as the CPU samples them: level 0 (w x h RGBA8, R in the
// low byte), then its mips, each half the one before (at least 1)
struct TexLevels {
    uint32_t w = 0, h = 0;
    const uint32_t* px = nullptr;
    const std::vector<std::vector<uint32_t>>* mips = nullptr;

    uint32_t Levels() const { return 1 + (mips ? uint32_t(mips->size()) : 0u); }
    const uint32_t* Level(uint32_t l) const { return l ? (*mips)[l - 1].data() : px; }
};

// sample_model.hlsli's kSampleFiltered: the sampler's word x has it when it's
// the game's
inline constexpr uint32_t kSampleFiltered = 1u << 11;

// TexSampler as sample_model.hlsli reads it, for a texture with `levels`
// levels (level 0 and its mips; 16 at most)
inline void PackSampler(const TexSampler& s, uint32_t levels, uint32_t out[4]) {
    out[0] = uint32_t(s.clamp_x & 7) | uint32_t(s.clamp_y & 7) << 3 |
             uint32_t(s.mag_linear ? 1 : 0) << 6 | uint32_t(s.min_linear ? 1 : 0) << 7 |
             uint32_t(s.mip & 3) << 8 | uint32_t(s.border_white ? 1 : 0) << 10 |
             (s.filtered ? kSampleFiltered : 0u);
    std::memcpy(&out[1], &s.lod_bias, 4);
    const uint32_t last = std::clamp<uint32_t>(levels, 1, 16) - 1;
    out[2] = uint32_t(std::min<uint32_t>(s.mip_min, 15)) |
             uint32_t(std::min<uint32_t>(s.mip_max, 15)) << 4 | last << 8;
    out[3] = std::max<uint32_t>(s.aniso, 1);
}

// `t` at uv, whose derivatives across the screen (one pixel right, one down)
// are dx and dy, by the packed sampler s (sample_model.hlsli's SampleTexture);
// RGBA 0..1
void SampleTextureCpu(const TexLevels& t, const uint32_t s[4], const float uv[2],
                      const float dx[2], const float dy[2], float out[4]);

// how many levels a w x h image's chain has, down to 1x1
inline uint32_t FullMipChain(uint32_t w, uint32_t h) {
    uint32_t n = 1;
    for (uint32_t s = std::max(w, h); s > 1; s >>= 1) n++;
    return n;
}

// Levels 1 .. levels-1 of a w x h RGBA8 image into out, each from the one
// before as SDL_GenerateMipmapsForGPUTexture makes a render target's on the
// GPU: a linear blit, each texel the bilinear sample (clamped) at its centre,
// which halving a side makes the box average of the four under it
void BuildMips(const uint32_t* px, uint32_t w, uint32_t h, uint32_t levels,
               std::vector<std::vector<uint32_t>>& out);

}  // namespace band3::render
