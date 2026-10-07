#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "src/Render/scene_capture.h"

// Texture sampling as the game's samplers do it (TexSampler, from the fetch
// constants): filtered, mipmapped by footprint LOD, anisotropic, per-axis
// addressing. Both backends share shaders/sample_model.hlsli (mesh.hlsl for
// the GPU, soft_raster.cpp as C++), taking the sampler packed by PackSampler.
// TexSampler::filtered 0 falls back to each backend's Texel(): nearest,
// wrapping, level 0.

namespace band3::render {

// level 0 is w x h RGBA8 (R in the low byte); each mip halves (min 1)
struct TexLevels {
    uint32_t w = 0, h = 0;
    const uint32_t* px = nullptr;
    const std::vector<std::vector<uint32_t>>* mips = nullptr;

    uint32_t Levels() const { return 1 + (mips ? uint32_t(mips->size()) : 0u); }
    const uint32_t* Level(uint32_t l) const { return l ? (*mips)[l - 1].data() : px; }
};

// matches sample_model.hlsli's kSampleFiltered (word x)
inline constexpr uint32_t kSampleFiltered = 1u << 11;

// packs for sample_model.hlsli; `levels` counts level 0 (16 at most)
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

// sample_model.hlsli's SampleTexture; dx/dy are uv's screen derivatives;
// out is RGBA 0..1
void SampleTextureCpu(const TexLevels& t, const uint32_t s[4], const float uv[2],
                      const float dx[2], const float dy[2], float out[4]);

// levels down to 1x1
inline uint32_t FullMipChain(uint32_t w, uint32_t h) {
    uint32_t n = 1;
    for (uint32_t s = std::max(w, h); s > 1; s >>= 1) n++;
    return n;
}

// Levels 1 .. levels-1, each from the one before, matching
// SDL_GenerateMipmapsForGPUTexture's clamped bilinear blit (a 2x2 box average)
void BuildMips(const uint32_t* px, uint32_t w, uint32_t h, uint32_t levels,
               std::vector<std::vector<uint32_t>>& out);

}  // namespace band3::render
