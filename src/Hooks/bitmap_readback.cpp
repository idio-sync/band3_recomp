// DxTex::LockBitmap under renderer native: a render target read back into a
// RndBitmap gets the native renderer's picture of it (bitmap_readback.h).
//
// The Art Library saves a piece this way: PatchPanel::CopyToPatch has
// PatchDir::CacheRenderedTex lock the art maker's target (a 256x256 kRendered
// texture), copy it to a bitmap and give that to the piece's own texture,
// which is compressed and saved. LockBitmap untiles guest memory's copy of the
// target, and with no emulated GPU nothing resolves the target there, so the
// piece was saved as whatever the memory held: colourful noise. The target's
// last pass is recorded on this thread (scene_capture.h; under renderer native
// capture always runs), so it's drawn again on the CPU (soft_raster.h) with
// the passes it samples and written over the game's bytes. If anything
// doesn't fit, the game's bytes stay, said once per texture.
#include <rex/logging.h>
#include <chrono>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include "generated/band3_init.h"
#include "src/Render/bitmap_readback.h"
#include "src/Render/deferred_decode.h"
#include "src/Render/scene_capture.h"
#include "src/Render/soft_raster.h"
#include "src/Render/sync_gpu/native_only.h"

namespace {

using namespace band3::render;

// RndTex::Type (tex+0x48)
constexpr uint32_t kTex_Type = 0x48;
// RndBitmap (rb3-xenon rndobj/Bitmap.h): u16 width, height and row bytes, u8
// bits per pixel, u32 order, then the pixels' address
constexpr uint32_t kBitmap_Width = 0;
constexpr uint32_t kBitmap_Height = 2;
constexpr uint32_t kBitmap_RowBytes = 4;
constexpr uint32_t kBitmap_Bpp = 6;
constexpr uint32_t kBitmap_Order = 8;
constexpr uint32_t kBitmap_Pixels = 12;
// LockBitmap's flags: the bitmap is read from the texture
constexpr uint32_t kLockRead = 1;

std::mutex g_warned_mutex;
std::unordered_set<uint32_t> g_warned;  // textures already said to fail

void WarnOnce(uint32_t tex, std::string_view why) {
    {
        std::lock_guard lock(g_warned_mutex);
        if (!g_warned.insert(tex).second) return;
    }
    REXLOG_WARN("Target readback: DxTex {:08X} keeps guest memory's pixels ({}), which renderer "
                "native never drew",
                tex, why);
}

// the target drawn on the CPU into the bitmap; false, said why, if not
bool ReadBack(uint8_t* base, uint32_t tex, uint32_t bmp) {
    const auto start = std::chrono::steady_clock::now();
    FrameCapture frame;
    uint32_t version = 0;
    if (!BuildTargetCapture(tex, frame, version)) {
        WarnOnce(tex, "no recorded pass made its current version");
        return false;
    }
    if (frame.rt_missing) {
        WarnOnce(tex, "a target its passes sample has no recorded pass");
        return false;
    }
    DecodeDeferred(frame);
    RasterOptions o;
    o.texture_passes = true;
    // guest memory's pixels are what's being replaced
    o.rt_guest_pixels = false;
    o.filtering = true;
    o.target_scale = o.shadow_scale = 1.0f;
    o.post = false;
    std::vector<uint32_t> rgba;
    uint32_t width = 0, height = 0;
    RasterStats stats;
    if (!RasterizeTarget(frame, o, tex, version, rgba, width, height, &stats)) {
        WarnOnce(tex, "the CPU drew no pass into it");
        return false;
    }
    BitmapShape shape;
    shape.width = REX_LOAD_U16(bmp + kBitmap_Width);
    shape.height = REX_LOAD_U16(bmp + kBitmap_Height);
    shape.rowbytes = REX_LOAD_U16(bmp + kBitmap_RowBytes);
    shape.bpp = REX_LOAD_U8(bmp + kBitmap_Bpp);
    shape.order = REX_LOAD_U32(bmp + kBitmap_Order);
    const uint32_t pixels = REX_LOAD_U32(bmp + kBitmap_Pixels);
    if (!pixels || !WriteTargetBitmap(rgba, width, height, shape,
                                      reinterpret_cast<uint8_t*>(REX_RAW_ADDR(pixels)))) {
        WarnOnce(tex, std::format("a {}x{} {}-bit bitmap of order {} and {} row bytes, for a {}x{} "
                                  "picture",
                                  shape.width, shape.height, shape.bpp, shape.order,
                                  shape.rowbytes, width, height));
        return false;
    }
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    // the target's own pass is the last appended
    const std::string& name = frame.passes.back().name;
    REXLOG_INFO("Target readback: DxTex {:08X} '{}' version {}, {}x{}, drawn on the CPU from {} "
                "pass(es) and {} draw(s) in {:.1f} ms",
                tex, name, version, width, height, frame.passes.size(), frame.draws.size(), ms);
    return true;
}

}  // namespace

extern "C" void __imp__DxTex__LockBitmap(PPCContext& ctx, uint8_t* base);
// r3 the DxTex, r4 the RndBitmap it makes and fills, r5 flags. The game's
// runs first, so the bitmap has the size and layout the game gives it.
extern "C" REX_FUNC(DxTex__LockBitmap) {
    const uint32_t tex = ctx.r3.u32, bmp = ctx.r4.u32, flags = ctx.r5.u32;
    __imp__DxTex__LockBitmap(ctx, base);
    // under renderer both the emulated GPU resolves targets as it always has
    if (!band3::render::sync_gpu::NativeOnly() || !(flags & kLockRead) || !tex || !bmp) return;
    const uint32_t type = REX_LOAD_U32(tex + kTex_Type);
    if (!band3::render::IsPassTargetType(type) || type == band3::render::kTexTypeShadowMap) return;
    ReadBack(base, tex, bmp);
}
