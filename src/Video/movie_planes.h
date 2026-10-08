#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "src/Render/guest_formats.h"

// A movie frame as RB3's Bink player keeps it: three 8-bit planes, Y at full
// size and cR, cB at half, BT.601 with offsets (shade.hlsli's MovieRgb). These
// write such a frame into the plane textures the player draws, laid out as
// their fetch constant says, the inverse of guest_formats.h's DecodeLevel8.
// Kept apart from guest memory for unit tests.

namespace band3::video {

// Bink's black and its neutral chroma, which MovieRgb turns to 0
inline constexpr uint8_t kBlackY = 16;
inline constexpr uint8_t kNeutralC = 128;

struct Plane {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> texels;  // width * height, rows packed

    void Resize(uint32_t w, uint32_t h, uint8_t fill) {
        width = w;
        height = h;
        texels.assign(size_t(w) * h, fill);
    }
    uint8_t* Row(uint32_t y) { return texels.data() + size_t(y) * width; }
    const uint8_t* Row(uint32_t y) const { return texels.data() + size_t(y) * width; }
};

// Y, cR, cB
struct PlaneSet {
    Plane y, cr, cb;
};

// Copies src into a k_8 texture's base level at dst (its base address in
// host memory), as the fetch's layout places it; the part of src outside the
// texture is left out, the texture outside src kept. False, nothing written,
// for any other format.
inline bool WritePlane8(const render::guest_format::FetchLayout& l, uint8_t* dst,
                        const Plane& src) {
    namespace gf = render::guest_format;
    gf::FormatInfo info;
    if (l.format != 2 || l.dimension != 1 || !gf::GetFormatInfo(l.format, info)) return false;
    const gf::LevelPlace p = gf::PlaceLevel(l, info, 0);
    const uint32_t w = std::min(l.width, src.width), h = std::min(l.height, src.height);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* s = src.Row(y);
        if (!l.tiled) {
            std::memcpy(dst + p.offset + size_t(y) * p.row_bytes, s, w);
            continue;
        }
        // x..x+7 are contiguous from a multiple of 8 (DecodeLevel8)
        for (uint32_t x = 0; x < w;) {
            uint8_t* d = dst + p.offset +
                         uint32_t(gf::TiledOffset2D(int32_t(x), int32_t(y), p.pitch_blocks, 0));
            if ((x & 7) == 0 && x + 8 <= w) {
                std::memcpy(d, s + x, 8);
                x += 8;
            } else {
                *d = s[x++];
            }
        }
    }
    return true;
}

// A frame that shows the planes are ours: a 4-texel white border round the
// whole Y plane (so a crop shows), a grey ramp across, a bar that moves with
// `t` (seconds), and the chroma planes' four quadrants red, green, blue and
// neutral. Sized as the textures are, Y w x h and chroma cw x ch.
inline void TestPattern(PlaneSet& out, uint32_t w, uint32_t h, uint32_t cw, uint32_t ch,
                        double t) {
    out.y.Resize(w, h, kBlackY);
    out.cr.Resize(cw, ch, kNeutralC);
    out.cb.Resize(cw, ch, kNeutralC);
    const uint32_t bar = w ? uint32_t(uint64_t(t * 0.25 * w) % w) : 0;
    for (uint32_t y = 0; y < h; y++) {
        uint8_t* row = out.y.Row(y);
        for (uint32_t x = 0; x < w; x++) {
            uint8_t v = uint8_t(16 + 219 * x / std::max(w - 1, 1u));
            if (x >= bar && x < bar + w / 32) v = 235;
            if (x < 4 || y < 4 || x + 4 >= w || y + 4 >= h) v = 235;
            row[x] = v;
        }
    }
    // (cR, cB) per quadrant: red, green, blue, neutral
    static constexpr uint8_t kQuad[4][2] = {{240, 90}, {34, 54}, {110, 240}, {128, 128}};
    for (uint32_t y = 0; y < ch; y++) {
        for (uint32_t x = 0; x < cw; x++) {
            const int q = (y >= ch / 2 ? 2 : 0) + (x >= cw / 2 ? 1 : 0);
            out.cr.Row(y)[x] = kQuad[q][0];
            out.cb.Row(y)[x] = kQuad[q][1];
        }
    }
}

}
