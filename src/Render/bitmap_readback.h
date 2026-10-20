#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// Reading a render target back into RAM (src/Hooks/bitmap_readback.cpp). RB3
// copies a target to the CPU with DxTex::LockBitmap: it makes a RndBitmap the
// target's size and untiles guest memory's copy of the target into it. Only a
// resolve fills that copy, and only with the SDK's readback_resolve does the
// emulated GPU's resolve reach guest memory; it's off by default, as reading
// every resolve back costs frame time. So the copy was whatever the memory
// held (the Art Library's saved art and characters' portraits were noise).
// - Under renderer native, nothing resolves, and the native renderer's
//   picture of the target is written over the bitmap instead, in the layout
//   the game's untile gave it: a locked D3DFMT_A8R8G8B8 surface's, each
//   texel a big-endian ARGB word (bytes A R G B), as an emulated run with
//   readback_resolve full dumped it. So under renderer both while the native
//   picture shows, as the emulated GPU skips the draws of passes drawn every
//   frame then (gpu_skip.h).
// - With the emulated GPU's picture, band3 turns the SDK's readback on while
//   the targets RB3 copies are drawn (ReadbackWindow), and the game's own
//   untile reads them.

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

// The render targets RB3 copies to the CPU, by object name: the art maker's
// (PatchPanel::CopyToPatch's PatchDir::CacheRenderedTex, as a piece of art is
// saved) and a character's portrait (TourChar::CachePortraitTex, as a
// character is made or leaves the closet)
inline bool IsCopiedTarget(std::string_view name) {
    return name == "patch_preview.tex" || name == "portrait.tex";
}

// Whether the emulated GPU reads its resolves back into guest memory, as the
// SDK's D3D12 command processor decides at each resolve
// (GetReadbackResolveMode): readback_resolve fast, some or full, or else
// d3d12_readback_resolve (which reads back as fast does) while
// readback_resolve is at its default (`mode_default`, none)
inline bool SdkReadsBackResolves(std::string_view readback_resolve, bool mode_default,
                                 bool d3d12_readback_resolve) {
    if (readback_resolve == "fast" || readback_resolve == "some" || readback_resolve == "full")
        return true;
    return d3d12_readback_resolve && mode_default;
}

// When band3 has the emulated GPU read resolves back: from the game frame a
// copied target's pass begins (Drawn) until kFrames frames pass without one
// (Frame, each Present), as the GPU's command processor resolves a frame or
// two after the game draws it and the SDK copies a resolve to guest memory a
// frame later. A target drawn every frame (the art maker's, while it shows)
// keeps it open; a portrait, drawn once, opens it for kFrames frames. Each
// returns what changed.
class ReadbackWindow {
 public:
    static constexpr uint64_t kFrames = 10;
    enum class Change { kNone, kOpen, kClose };

    Change Drawn(uint64_t frame) {
        last_ = frame;
        if (open_) return Change::kNone;
        open_ = true;
        return Change::kOpen;
    }
    Change Frame(uint64_t frame) {
        if (!open_ || frame < last_ + kFrames) return Change::kNone;
        open_ = false;
        return Change::kClose;
    }
    bool open() const { return open_; }

 private:
    bool open_ = false;
    uint64_t last_ = 0;
};

// Whether LockBitmap's bitmap of a target gets the CPU's drawing of it
// rather than guest memory's: under renderer native, and under both while
// the native picture shows
inline bool DrawsCopyOnCpu(bool native_only, bool native_picture) {
    return native_only || native_picture;
}

// From scene_capture's hooks, on the thread that draws (bitmap_readback.cpp):
// DxTex::MakeDrawTarget began a pass into `tex`, and DxRnd::Present ended a
// game frame. Under renderer native they return at once.
void NoteTargetPass(uint8_t* base, uint32_t tex);
void ReadbackWindowFrame();

}  // namespace band3::render
