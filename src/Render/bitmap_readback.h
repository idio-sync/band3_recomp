#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Reading a render target back into RAM under renderer native
// (src/Hooks/bitmap_readback.cpp). RB3 copies a target to the CPU with
// DxTex::LockBitmap: it makes a RndBitmap the target's size and untiles guest
// memory's copy of the target into it. With no emulated GPU nothing ever
// resolves the target into guest memory, so that copy is whatever the memory
// held (the Art Library's saved art was noise). The native renderer's picture
// of the target is written over the bitmap instead, in the layout the game's
// untile gave it: a locked D3DFMT_A8R8G8B8 surface's, each texel a big-endian
// ARGB word (bytes A R G B), as an emulated run with readback_resolve full
// dumped it.

namespace band3::render {

// The RndBitmap LockBitmap made (rb3-xenon rndobj/Bitmap.h): width, height,
// row bytes, bits per pixel and pixel order
struct BitmapShape {
    uint32_t width = 0, height = 0, rowbytes = 0, bpp = 0, order = 0;
};

// Writes `rgba` (width * height RGBA8, R in the low byte, rows top first)
// into `pixels`, a bitmap of `shape`: 32 bits a texel, order 0, the
// picture's size, rows `shape.rowbytes` apart (the bytes past each row's
// width left as they were). Alpha is kept. False, with `pixels` untouched,
// for a bitmap of any other shape or a picture that isn't width * height.
inline bool WriteTargetBitmap(const std::vector<uint32_t>& rgba, uint32_t width, uint32_t height,
                              const BitmapShape& shape, uint8_t* pixels) {
    if (!pixels || width == 0 || height == 0) return false;
    if (rgba.size() != size_t(width) * height) return false;
    if (shape.bpp != 32 || shape.order != 0) return false;
    if (shape.width != width || shape.height != height) return false;
    if (shape.rowbytes < 4 * width) return false;
    for (uint32_t y = 0; y < height; y++) {
        uint8_t* row = pixels + size_t(y) * shape.rowbytes;
        const uint32_t* from = rgba.data() + size_t(y) * width;
        for (uint32_t x = 0; x < width; x++) {
            const uint32_t c = from[x];
            row[4 * x + 0] = uint8_t(c >> 24);  // A
            row[4 * x + 1] = uint8_t(c);        // R
            row[4 * x + 2] = uint8_t(c >> 8);   // G
            row[4 * x + 3] = uint8_t(c >> 16);  // B
        }
    }
    return true;
}

}  // namespace band3::render
