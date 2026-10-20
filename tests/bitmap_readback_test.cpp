// Checks src/Render/bitmap_readback.h: the native renderer's picture of a
// render target (RGBA8, R in the low byte) written into the RndBitmap that
// DxTex::LockBitmap made of it, as big-endian ARGB words (A R G B bytes, as
// a locked D3DFMT_A8R8G8B8 surface has them), its rows the bitmap's row
// bytes apart, alpha kept; and that a bitmap of another shape (bits per
// pixel, order, size, short rows) is left as it was.

#include <doctest/doctest.h>
#include <cstdint>
#include <vector>
#include "src/Render/bitmap_readback.h"

using namespace band3::render;

namespace {

// RGBA8, R in the low byte
constexpr uint32_t Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24;
}

BitmapShape Shape(uint32_t w, uint32_t h, uint32_t rowbytes, uint32_t bpp = 32,
                  uint32_t order = 0) {
    return BitmapShape{w, h, rowbytes, bpp, order};
}

}  // namespace

TEST_CASE("each texel becomes A R G B bytes, rows in order") {
    const std::vector<uint32_t> rgba = {Rgba(0x11, 0x22, 0x33, 0xff), Rgba(0x44, 0x55, 0x66, 0x80),
                                        Rgba(0, 0, 0, 0), Rgba(0xfe, 0x01, 0x7f, 0x01)};
    std::vector<uint8_t> out(16, 0xcd);
    REQUIRE(WriteTargetBitmap(rgba, 2, 2, Shape(2, 2, 8), out.data()));
    const std::vector<uint8_t> want = {0xff, 0x11, 0x22, 0x33, 0x80, 0x44, 0x55, 0x66,
                                       0x00, 0x00, 0x00, 0x00, 0x01, 0xfe, 0x01, 0x7f};
    CHECK(out == want);
}

TEST_CASE("rows are the bitmap's row bytes apart, the padding left as it was") {
    const std::vector<uint32_t> rgba = {Rgba(1, 2, 3, 4), Rgba(5, 6, 7, 8)};
    std::vector<uint8_t> out(2 * 8, 0xcd);
    REQUIRE(WriteTargetBitmap(rgba, 1, 2, Shape(1, 2, 8), out.data()));
    const std::vector<uint8_t> want = {4, 1, 2, 3, 0xcd, 0xcd, 0xcd, 0xcd,
                                       8, 5, 6, 7, 0xcd, 0xcd, 0xcd, 0xcd};
    CHECK(out == want);
}

TEST_CASE("a bitmap of another shape is left as it was") {
    const std::vector<uint32_t> rgba(4, Rgba(9, 9, 9, 9));
    std::vector<uint8_t> out(64, 0xcd);
    const std::vector<uint8_t> before = out;
    // 16 or 8 bits, another order, short rows, another size than the picture's
    CHECK_FALSE(WriteTargetBitmap(rgba, 2, 2, Shape(2, 2, 8, 16), out.data()));
    CHECK_FALSE(WriteTargetBitmap(rgba, 2, 2, Shape(2, 2, 8, 8), out.data()));
    CHECK_FALSE(WriteTargetBitmap(rgba, 2, 2, Shape(2, 2, 8, 32, 1), out.data()));
    CHECK_FALSE(WriteTargetBitmap(rgba, 2, 2, Shape(2, 2, 7), out.data()));
    CHECK_FALSE(WriteTargetBitmap(rgba, 2, 2, Shape(4, 1, 16), out.data()));
    CHECK_FALSE(WriteTargetBitmap(rgba, 2, 2, Shape(2, 1, 8), out.data()));
    // a picture whose texels aren't width * height, or empty
    CHECK_FALSE(WriteTargetBitmap(std::vector<uint32_t>(3), 2, 2, Shape(2, 2, 8), out.data()));
    CHECK_FALSE(WriteTargetBitmap({}, 0, 0, Shape(0, 0, 0), out.data()));
    CHECK_FALSE(WriteTargetBitmap(rgba, 2, 2, Shape(2, 2, 8), nullptr));
    CHECK(out == before);
}
