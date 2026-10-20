// Checks src/Render/bitmap_readback.h: the native renderer's picture of a
// render target (RGBA8, R in the low byte) written into the RndBitmap that
// DxTex::LockBitmap made of it, as big-endian ARGB words (A R G B bytes, as
// a locked D3DFMT_A8R8G8B8 surface has them), its rows the bitmap's row
// bytes apart, alpha kept; and that a bitmap of another shape (bits per
// pixel, order, size, short rows) is left as it was. Then, with an emulated
// GPU: which targets RB3 copies, when the SDK's readback is on as it reads
// its settings, the window that turns it on around a copied target's passes,
// and which picture the bitmap gets under each renderer.

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

TEST_CASE("the art maker's target and the portrait are the targets RB3 copies") {
    CHECK(IsCopiedTarget("patch_preview.tex"));
    CHECK(IsCopiedTarget("portrait.tex"));
    CHECK_FALSE(IsCopiedTarget(""));
    CHECK_FALSE(IsCopiedTarget("portrait"));
    CHECK_FALSE(IsCopiedTarget("bloom.tex"));
}

TEST_CASE("the SDK reads back resolves for a readback_resolve mode, else for the old switch") {
    CHECK(SdkReadsBackResolves("fast", false, false));
    CHECK(SdkReadsBackResolves("some", false, false));
    CHECK(SdkReadsBackResolves("full", false, false));
    CHECK_FALSE(SdkReadsBackResolves("none", true, false));
    // d3d12_readback_resolve counts while readback_resolve is at its default
    CHECK(SdkReadsBackResolves("none", true, true));
    // and not once it's been set, even to a value the SDK doesn't know
    CHECK_FALSE(SdkReadsBackResolves("bogus", false, true));
    CHECK_FALSE(SdkReadsBackResolves("bogus", false, false));
}

TEST_CASE("the readback window opens at a copied target's pass and closes frames after the last") {
    ReadbackWindow w;
    CHECK_FALSE(w.open());
    CHECK(w.Frame(1) == ReadbackWindow::Change::kNone);
    CHECK(w.Drawn(5) == ReadbackWindow::Change::kOpen);
    CHECK(w.open());
    // drawn again: still open, and the count starts over
    CHECK(w.Drawn(6) == ReadbackWindow::Change::kNone);
    for (uint64_t f = 6; f < 6 + ReadbackWindow::kFrames; f++)
        CHECK(w.Frame(f) == ReadbackWindow::Change::kNone);
    CHECK(w.open());
    CHECK(w.Frame(6 + ReadbackWindow::kFrames) == ReadbackWindow::Change::kClose);
    CHECK_FALSE(w.open());
    CHECK(w.Frame(100) == ReadbackWindow::Change::kNone);
    // and opens again at the next one
    CHECK(w.Drawn(200) == ReadbackWindow::Change::kOpen);
}

TEST_CASE("a target drawn every frame keeps the window open") {
    ReadbackWindow w;
    CHECK(w.Drawn(0) == ReadbackWindow::Change::kOpen);
    for (uint64_t f = 1; f < 1000; f++) {
        CHECK(w.Drawn(f) == ReadbackWindow::Change::kNone);
        CHECK(w.Frame(f) == ReadbackWindow::Change::kNone);
    }
    CHECK(w.open());
}

TEST_CASE("the bitmap gets the CPU's drawing only where the emulated GPU's may be missing") {
    // renderer native: nothing resolves into guest memory
    CHECK(DrawsCopyOnCpu(true, true));
    // both, the native picture showing: the emulated GPU skips the draws of
    // passes drawn every frame
    CHECK(DrawsCopyOnCpu(false, true));
    // emulated, or both showing the emulated GPU's picture: guest memory's,
    // which the readback window fills
    CHECK_FALSE(DrawsCopyOnCpu(false, false));
}
