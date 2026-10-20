// DxTex::LockBitmap: a render target read back into a RndBitmap gets the
// target's picture, under every renderer (bitmap_readback.h).
//
// The Art Library saves a piece this way: PatchPanel::CopyToPatch has
// PatchDir::CacheRenderedTex lock the art maker's target (a 256x256 kRendered
// texture), copy it to a bitmap and give that to the piece's own texture,
// which is compressed and saved; a character's portrait is copied the same
// way (TourChar::CachePortraitTex). LockBitmap untiles guest memory's copy of
// the target, which only a resolve the emulated GPU reads back fills, so the
// piece was saved as whatever the memory held: colourful noise.
// - Under renderer native (and both, while the native picture shows) the
//   target's last pass is recorded on this thread (scene_capture.h; capture
//   runs then), so it's drawn again on the CPU (soft_raster.h) with the
//   passes it samples and written over the game's bytes. If anything doesn't
//   fit, the game's bytes stay, said once per texture.
// - With the emulated GPU's picture, the SDK's readback is turned on while a
//   target RB3 copies is drawn (ReadbackWindow, through d3d12_readback_resolve
//   unless readback_resolve already reads back), and the game's bytes are the
//   GPU's own.
#include <rex/cvar.h>
#include <rex/logging.h>
#include <atomic>
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
#include "src/Render/renderer_switch.h"
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
// Hmx::Object's name (obj/Object.h), a C string
constexpr uint32_t kObj_Name = 0x18;
constexpr uint32_t kMaxName = 64;
// the SDK's settings the readback window reads and sets
constexpr std::string_view kReadbackMode = "readback_resolve";
constexpr std::string_view kReadbackSwitch = "d3d12_readback_resolve";

std::mutex g_warned_mutex;
std::unordered_set<uint32_t> g_warned;  // textures already said to fail

void WarnOnce(uint32_t tex, std::string_view why) {
    {
        std::lock_guard lock(g_warned_mutex);
        if (!g_warned.insert(tex).second) return;
    }
    REXLOG_WARN("Target readback: DxTex {:08X} keeps guest memory's pixels ({})", tex, why);
}

// a guest object's name, up to kMaxName characters
std::string GuestName(uint8_t* base, uint32_t obj) {
    std::string out;
    const uint32_t p = REX_LOAD_U32(obj + kObj_Name);
    if (p < 0x10000u || p >= 0xF0000000u) return out;
    for (uint32_t i = 0; i < kMaxName; i++) {
        const char c = char(REX_LOAD_U8(p + i));
        if (!c) break;
        out += c;
    }
    return out;
}

// The readback window (bitmap_readback.h), on the drawing thread; the mutex
// as the splash thread can draw too
std::mutex g_window_mutex;
ReadbackWindow g_window;
// whether this window turned d3d12_readback_resolve on, to turn it back off
bool g_window_set = false;
// game frames (Presents) so far, and g_window.open() for Present's early-out
std::atomic<uint64_t> g_frames{0};
std::atomic<bool> g_window_open{false};

// whether the SDK reads resolves back as its settings stand; no settings
// (no emulated GPU) is no
bool SdkReadsBack() {
    if (!rex::cvar::GetFlagInfo(kReadbackSwitch)) return false;
    return SdkReadsBackResolves(rex::cvar::GetFlagByName(kReadbackMode),
                                !rex::cvar::HasNonDefaultValue(kReadbackMode),
                                rex::cvar::Query<bool>(kReadbackSwitch));
}

// Under g_window_mutex, with the window open: has the emulated GPU read back
// while its picture shows, unless it already does or d3d12_readback_resolve
// was set (band3.toml or the command line; false keeps the window shut).
// Turning it on applies from the next resolve its command processor runs (it
// reads the setting at each), which is this pass's or a later one, as it
// runs behind the game.
void TurnReadbackOnIfNeeded(std::string_view name) {
    if (g_window_set || ShowsNativePicture()) return;
    // read before the window first sets it
    static const bool user_set =
        rex::cvar::GetFlagSource(kReadbackSwitch) != rex::cvar::Source::kDefault;
    if (user_set || SdkReadsBack()) return;
    if (!rex::cvar::SetFlagByName(kReadbackSwitch, "true")) return;
    g_window_set = true;
    REXLOG_INFO("Target readback: the emulated GPU reads its resolves back while '{}' is drawn "
                "({} on)",
                name, kReadbackSwitch);
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

namespace band3::render {

void NoteTargetPass(uint8_t* base, uint32_t tex) {
    if (sync_gpu::NativeOnly() || !tex) return;
    const std::string name = GuestName(base, tex);
    if (!IsCopiedTarget(name)) return;
    std::lock_guard lock(g_window_mutex);
    g_window.Drawn(g_frames.load(std::memory_order_relaxed));
    g_window_open.store(true, std::memory_order_relaxed);
    // again at each pass, for an F8 to the emulated GPU's picture meanwhile
    TurnReadbackOnIfNeeded(name);
}

void ReadbackWindowFrame() {
    const uint64_t frame = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!g_window_open.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_window_mutex);
    if (g_window.Frame(frame) != ReadbackWindow::Change::kClose) return;
    g_window_open.store(false, std::memory_order_relaxed);
    if (!g_window_set) return;
    g_window_set = false;
    // back to its default: nothing set it before the window did
    rex::cvar::ResetToDefault(kReadbackSwitch);
    REXLOG_INFO("Target readback: the emulated GPU no longer reads its resolves back ({} off)",
                kReadbackSwitch);
}

}  // namespace band3::render

extern "C" void __imp__DxTex__LockBitmap(PPCContext& ctx, uint8_t* base);
// r3 the DxTex, r4 the RndBitmap it makes and fills, r5 flags. The game's
// runs first, so the bitmap has the size and layout the game gives it.
extern "C" REX_FUNC(DxTex__LockBitmap) {
    const uint32_t tex = ctx.r3.u32, bmp = ctx.r4.u32, flags = ctx.r5.u32;
    __imp__DxTex__LockBitmap(ctx, base);
    namespace render = band3::render;
    const uint32_t type = tex && bmp && (flags & kLockRead) ? REX_LOAD_U32(tex + kTex_Type) : 0;
    if (render::IsPassTargetType(type) && type != render::kTexTypeShadowMap) {
        if (render::DrawsCopyOnCpu(render::sync_gpu::NativeOnly(), render::ShowsNativePicture())) {
            ReadBack(base, tex, bmp);
        } else {
            // the emulated GPU's own resolve, right if it was read back
            std::lock_guard lock(g_window_mutex);
            if (!g_window_set && !SdkReadsBack())
                WarnOnce(tex, std::format("'{}' isn't a target band3 has the emulated GPU read back "
                                          "(bitmap_readback.h); readback_resolve fast would",
                                          GuestName(base, tex)));
        }
    }
}
