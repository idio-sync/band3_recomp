// native_fill_window: the game's cameras at the window's shape, the HUD and
// tracks at their size in the middle (aspect_model.h)
//
// Rnd::YRatio gives the window's height over width, and RndCam::UpdateLocal
// widens a perspective camera's vertical field of view for a window taller
// than 16:9 while it builds the camera's projection. A camera keeps its
// projection until something changes it, so RndCam::Select, which the game
// calls before each camera draws, has one built for another shape rebuilt:
// it rebuilds a camera whose Rnd::Aspect (cam+744) isn't TheRnd's, so
// BeforeSelect (from scene_capture.cpp's Select hook) makes it look that
// way. Resizing the window, the setting and F8 then apply at the next frame.

#include <rex/logging.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include "generated/band3_init.h"
#include "src/Hooks/aspect.h"
#include "src/Hooks/aspect_model.h"

extern "C" void __imp__Rnd__YRatio(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__RndCam__UpdateLocal(PPCContext& ctx, uint8_t* base);

namespace {

using namespace band3::aspect;

// ShapeFor's, 0 (16:9) until the native renderer presents
std::atomic<uint64_t> g_shape{0};

// whether the screen up is the song list (SetScreen)
std::atomic<bool> g_song_list{false};

// each camera's shape as UpdateLocal last built it
std::mutex g_built_mutex;
std::unordered_map<uint32_t, uint64_t> g_built;

// RndCam (rb3-xenon rndobj/Cam.h, rnddx9/Cam.cpp): mYFov, the texture it
// draws into (0 the back buffer) and the Rnd::Aspect it was built for
constexpr uint32_t kCam_YFov = 700;
constexpr uint32_t kCam_TargetTex = 740;
constexpr uint32_t kCam_Aspect = 744;
constexpr uint32_t kUpdateLocalAddr = 0x82433628;
constexpr uint32_t kUpdateLocalSize = 0x1C0;

// what UpdateLocal multiplies mYFov by before its tan (a half): read from its
// `lis r11,hi; lfs f0,lo(r11); fmuls f1,f13,f0`, as the constant's address
// differs between the disc's xex and TU5's. 0 if the code isn't that.
float FovScale(uint8_t* base) {
    for (uint32_t a = kUpdateLocalAddr + 8; a < kUpdateLocalAddr + kUpdateLocalSize; a += 4) {
        if (REX_LOAD_U32(a) != 0xEC2D0032u) continue;  // fmuls f1,f13,f0
        const uint32_t lis = REX_LOAD_U32(a - 8), lfs = REX_LOAD_U32(a - 4);
        if ((lis & 0xFFFF0000u) != 0x3D600000u || (lfs & 0xFFFF0000u) != 0xC00B0000u) return 0;
        const uint32_t bits = REX_LOAD_U32((lis << 16) + uint32_t(int32_t(int16_t(lfs & 0xFFFF))));
        float scale;
        std::memcpy(&scale, &bits, 4);
        return scale;
    }
    return 0;
}

}  // namespace

namespace band3::aspect {

void BeforeSelect(uint8_t* base, uint32_t cam) {
    const uint64_t shape = g_shape.load(std::memory_order_relaxed);
    bool stale;
    {
        std::lock_guard<std::mutex> lock(g_built_mutex);
        const auto it = g_built.find(cam);
        // one UpdateLocal never built (a copy) is the game's 16:9
        stale = it == g_built.end() ? shape != 0 : it->second != shape;
    }
    // no Rnd::Aspect, so Select rebuilds it
    if (stale) REX_STORE_U32(cam + kCam_Aspect, 0xFFFFFFFFu);
}

void SetScreen(const char* name) {
    g_song_list.store(name && IsSongList(name), std::memory_order_relaxed);
}

bool SongListShowing() { return g_song_list.load(std::memory_order_relaxed); }

void CurrentOverlayEdge(float out[2]) {
    OverlayEdge(g_shape.load(std::memory_order_relaxed), out);
}

void SetWindow(uint32_t width, uint32_t height, bool fill) {
    if (fill && (!width || !height)) return;
    const uint64_t shape = ShapeFor(width, height, fill);
    if (g_shape.exchange(shape, std::memory_order_relaxed) == shape) return;
    if (shape)
        REXLOG_INFO("fill window: the game's cameras at {}x{}", width, height);
    else
        REXLOG_INFO("fill window: the game's cameras at 16:9");
}

}  // namespace band3::aspect

extern "C" REX_FUNC(Rnd__YRatio)
{
    __imp__Rnd__YRatio(ctx, base);
    const uint64_t shape = g_shape.load(std::memory_order_relaxed);
    if (shape) ctx.f1.f64 = double(float(YRatio(shape)));
}

extern "C" REX_FUNC(RndCam__UpdateLocal)
{
    const uint32_t cam = ctx.r3.u32;
    // before the YRatio the build reads, so a shape that changes meanwhile
    // leaves the camera marked for another rebuild
    const uint64_t shape = g_shape.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_built_mutex);
        g_built[cam] = shape;
    }
    static const float fov_scale = [base] {
        const float scale = FovScale(base);
        if (scale <= 0) REXLOG_WARN("fill window: RndCam::UpdateLocal isn't as expected; "
                                    "a window taller than 16:9 loses its sides");
        return scale;
    }();
    const double tall = TallScale(shape);
    const uint32_t fov_bits = REX_LOAD_U32(cam + kCam_YFov);
    float fov;
    std::memcpy(&fov, &fov_bits, 4);
    // orthographic cameras (fov 0) and those drawing into textures as they are
    if (tall >= 1 || fov_scale <= 0 || fov <= 0 || REX_LOAD_U32(cam + kCam_TargetTex)) {
        __imp__RndCam__UpdateLocal(ctx, base);
        return;
    }
    // for this build only: the camera's own field of view stays the game's
    const float widened = WidenedFov(fov, fov_scale, tall);
    uint32_t widened_bits;
    std::memcpy(&widened_bits, &widened, 4);
    REX_STORE_U32(cam + kCam_YFov, widened_bits);
    __imp__RndCam__UpdateLocal(ctx, base);
    REX_STORE_U32(cam + kCam_YFov, fov_bits);
}
