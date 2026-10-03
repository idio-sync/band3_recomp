#include "src/Render/gpu_skip.h"

#include <rex/cvar.h>

#include <atomic>
#include <mutex>
#include <string_view>
#include <utility>

#include "generated/band3_init.h"

// See gpu_skip.h.

extern "C" void __imp__rex_sub_82863BB8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__rex_sub_828644C0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__rex_sub_828640D0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__rex_sub_82863B70(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxRnd__DoPointTests(PPCContext& ctx, uint8_t* base);

namespace band3::render {
namespace {

// the frame being drawn is skipped: written at the end of each Present
// (LatchGpuSkip), read by every draw
std::atomic<bool> g_frame_skip{false};
// a texture pass that isn't drawn regularly is open (SetPassWantsDraws)
std::atomic<bool> g_pass_wants_draws{false};
// inside DxRnd::DoPointTests on this thread: its DrawVerticesUP are the
// flares' occlusion queries, which the game reads back
thread_local bool t_point_tests = false;

// the settings, kept by their change callbacks (registered at the first
// Present, as scene_capture.cpp's are); the callbacks only store
std::atomic<bool> g_renderer_native{false};
std::atomic<bool> g_skip_setting{true};  // emulated_gpu_while_native is skip_draws

// the latch, on the game's render thread (the splash thread's at boot, the
// main thread's after, never both at once; the lock is for that handover)
std::mutex g_latch_mutex;
SkipLatch g_latch;
// whole frames asked for and not yet handed to the latch
std::atomic<int> g_full_requested{0};
// SkipLatch::Fresh and WholeFrames, for any thread
std::atomic<bool> g_fresh{true};
std::atomic<int> g_whole{2};

std::mutex g_callback_mutex;
std::function<void()> g_on_fresh;

std::atomic<uint64_t> g_emitted[GpuSkipStats::kNumKinds], g_skipped[GpuSkipStats::kNumKinds];
std::atomic<uint64_t> g_kept_pass{0}, g_kept_point_tests{0};
std::atomic<uint64_t> g_frames{0}, g_frames_skipped{0};

void TrackSettings() {
    static std::once_flag tracking;
    std::call_once(tracking, [] {
        auto skip = [](std::string_view v) { g_skip_setting.store(v != "full"); };
        g_renderer_native.store(rex::cvar::GetFlagByName("renderer") == "native");
        skip(rex::cvar::GetFlagByName("emulated_gpu_while_native"));
        rex::cvar::RegisterChangeCallback("renderer", [](std::string_view, std::string_view v) {
            g_renderer_native.store(v == "native");
        });
        rex::cvar::RegisterChangeCallback(
            "emulated_gpu_while_native",
            [skip](std::string_view, std::string_view v) { skip(v); });
    });
}

// Whether a draw of `kind` is emitted, counted either way. Only the frame's
// latch is a shared load; the rest is this thread's or rarely written.
bool Emit(int kind) {
    if (!g_frame_skip.load(std::memory_order_relaxed)) {
        g_emitted[kind].fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (kind == GpuSkipStats::kBeginIndexed) {
        g_emitted[kind].fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (kind == GpuSkipStats::kUp && t_point_tests) {
        g_emitted[kind].fetch_add(1, std::memory_order_relaxed);
        g_kept_point_tests.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (g_pass_wants_draws.load(std::memory_order_relaxed)) {
        g_emitted[kind].fetch_add(1, std::memory_order_relaxed);
        g_kept_pass.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g_skipped[kind].fetch_add(1, std::memory_order_relaxed);
    return false;
}

}  // namespace

void LatchGpuSkip(bool capture_on, bool recording) {
    TrackSettings();
    const bool want = g_renderer_native.load(std::memory_order_relaxed) &&
                      g_skip_setting.load(std::memory_order_relaxed) && capture_on && recording;
    const int full = g_full_requested.exchange(0);
    bool was_fresh, fresh, skip;
    int whole;
    {
        std::lock_guard lock(g_latch_mutex);
        was_fresh = g_latch.Fresh();
        skip = g_latch.EndFrame(want, full);
        fresh = g_latch.Fresh();
        whole = g_latch.WholeFrames();
    }
    g_whole.store(whole);
    g_frames.fetch_add(1, std::memory_order_relaxed);
    if (skip) g_frames_skipped.fetch_add(1, std::memory_order_relaxed);
    g_fresh.store(fresh);
    g_frame_skip.store(skip);
    if (fresh && !was_fresh) {
        std::lock_guard lock(g_callback_mutex);
        if (g_on_fresh) g_on_fresh();
    }
}

void SetPassWantsDraws(bool wants) { g_pass_wants_draws.store(wants, std::memory_order_relaxed); }

bool RendererNative() { return g_renderer_native.load(std::memory_order_relaxed); }

bool EmulatedPictureFresh() { return g_fresh.load(); }

int EmulatedWholeFrames() { return g_whole.load(); }

void RequestFullFrames(int frames) {
    int now = g_full_requested.load();
    while (now < frames && !g_full_requested.compare_exchange_weak(now, frames)) {
    }
}

void SetEmulatedFreshCallback(std::function<void()> callback) {
    std::lock_guard lock(g_callback_mutex);
    g_on_fresh = std::move(callback);
}

GpuSkipStats GetGpuSkipStats() {
    GpuSkipStats s;
    for (int i = 0; i < GpuSkipStats::kNumKinds; i++) {
        s.emitted[i] = g_emitted[i].load(std::memory_order_relaxed);
        s.skipped[i] = g_skipped[i].load(std::memory_order_relaxed);
    }
    s.kept_pass = g_kept_pass.load(std::memory_order_relaxed);
    s.kept_point_tests = g_kept_point_tests.load(std::memory_order_relaxed);
    s.frames = g_frames.load(std::memory_order_relaxed);
    s.frames_skipped = g_frames_skipped.load(std::memory_order_relaxed);
    s.skip_mode = g_renderer_native.load() && g_skip_setting.load();
    s.skipping = g_frame_skip.load();
    s.fresh = g_fresh.load();
    return s;
}

GpuSkipStats GpuSkipStatsSince(const GpuSkipStats& now, const GpuSkipStats& before) {
    GpuSkipStats s = now;
    for (int i = 0; i < GpuSkipStats::kNumKinds; i++) {
        s.emitted[i] -= before.emitted[i];
        s.skipped[i] -= before.skipped[i];
    }
    s.kept_pass -= before.kept_pass;
    s.kept_point_tests -= before.kept_point_tests;
    s.frames -= before.frames;
    s.frames_skipped -= before.frames_skipped;
    return s;
}

}  // namespace band3::render

using band3::render::GpuSkipStats;

// The emitters (out/n3/experiment.md 0; their call sites there). Each writes
// its draw's packets into the ring; skipped, the device's CPU shadow keeps
// the state the draw would have flushed, and the next one emitted flushes it.

// D3DDevice_BeginIndexedVertices: kept, counted. DxMesh::DrawFaces' mutable
// path copies its indices and vertices through the pointers it hands out.
extern "C" REX_FUNC(rex_sub_82863BB8) {
    band3::render::Emit(GpuSkipStats::kBeginIndexed);
    __imp__rex_sub_82863BB8(ctx, base);
}

// D3DDevice_DrawIndexedVertices: DxMesh::DrawFaces' draws
extern "C" REX_FUNC(rex_sub_828644C0) {
    if (band3::render::Emit(GpuSkipStats::kIndexed)) __imp__rex_sub_828644C0(ctx, base);
}

// the instanced draw: DxMultiMesh::DrawBatchedNewGfx's
extern "C" REX_FUNC(rex_sub_828640D0) {
    if (band3::render::Emit(GpuSkipStats::kInstanced)) __imp__rex_sub_828640D0(ctx, base);
}

// D3DDevice_DrawVerticesUP (BeginVertices, a copy, the draw): DrawRect's and
// the rest, kept inside DoPointTests
extern "C" REX_FUNC(rex_sub_82863B70) {
    if (band3::render::Emit(GpuSkipStats::kUp)) __imp__rex_sub_82863B70(ctx, base);
}

// DxRnd::DoPointTests: the lens flares' occlusion queries, drawn and read
// back (BlockOnFence) whatever the frame
extern "C" REX_FUNC(DxRnd__DoPointTests) {
    band3::render::t_point_tests = true;
    __imp__DxRnd__DoPointTests(ctx, base);
    band3::render::t_point_tests = false;
}
