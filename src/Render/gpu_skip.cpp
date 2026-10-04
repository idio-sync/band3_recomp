#include "src/Render/gpu_skip.h"

#include <rex/cvar.h>
#include <rex/logging.h>

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
extern "C" void __imp__rex_sub_828634E0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__rex_sub_82855F18(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__DxRnd__DoPointTests(PPCContext& ctx, uint8_t* base);

namespace band3::render {
namespace {

// the frame being drawn's SkipLevel: written at the end of each Present
// (LatchGpuSkip), read by every emitter
std::atomic<int> g_frame_level{int(SkipLevel::kFull)};
// a texture pass that isn't drawn regularly is open (SetPassWantsDraws)
std::atomic<bool> g_pass_wants_draws{false};
// inside DxRnd::DoPointTests on this thread: its DrawVerticesUP are the
// flares' occlusion-test quads, whose queries the game reads back
thread_local bool t_point_tests = false;

// the settings, kept by their change callbacks (registered at the first
// Present, as scene_capture.cpp's are); the callbacks only store
std::atomic<bool> g_renderer_native{false};
// emulated_gpu_while_native's SkipLevel
std::atomic<int> g_skip_setting{int(SkipLevel::kSkipDraws)};
// compress_character_textures: outfits are read back (DxTex::LockBitmap)
// after they're composed, which swap_only skips
std::atomic<bool> g_compress_textures{false};
std::atomic<bool> g_warned_compress{false};

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
std::atomic<uint64_t> g_kept_pass{0}, g_kept_point_tests{0}, g_passes_dropped{0};
std::atomic<uint64_t> g_frames{0}, g_frames_skipped{0};

// emulated_gpu_while_native's values; anything else (the setting allows
// none) is the default
SkipLevel ParseSkipLevel(std::string_view v) {
    if (v == "full") return SkipLevel::kFull;
    if (v == "swap_only") return SkipLevel::kSwapOnly;
    return SkipLevel::kSkipDraws;
}

void TrackSettings() {
    static std::once_flag tracking;
    std::call_once(tracking, [] {
        auto skip = [](std::string_view v) { g_skip_setting.store(int(ParseSkipLevel(v))); };
        auto compress = [](std::string_view v) {
            g_compress_textures.store(v == "true" || v == "1");
        };
        g_renderer_native.store(rex::cvar::GetFlagByName("renderer") == "native");
        skip(rex::cvar::GetFlagByName("emulated_gpu_while_native"));
        compress(rex::cvar::GetFlagByName("compress_character_textures"));
        rex::cvar::RegisterChangeCallback("renderer", [](std::string_view, std::string_view v) {
            g_renderer_native.store(v == "native");
        });
        rex::cvar::RegisterChangeCallback(
            "emulated_gpu_while_native",
            [skip](std::string_view, std::string_view v) { skip(v); });
        rex::cvar::RegisterChangeCallback(
            "compress_character_textures",
            [compress](std::string_view, std::string_view v) { compress(v); });
    });
}

// Whether a call of `kind` is emitted (EmitDraw), counted either way. The
// frame's level and the open pass are relaxed loads of rarely written
// atomics; the rest is this thread's.
bool Emit(int kind) {
    const auto level = SkipLevel(g_frame_level.load(std::memory_order_relaxed));
    const bool pass = g_pass_wants_draws.load(std::memory_order_relaxed);
    if (!EmitDraw(level, kind, pass, t_point_tests)) {
        g_skipped[kind].fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_emitted[kind].fetch_add(1, std::memory_order_relaxed);
    // a draw a skip_draws frame kept, and why
    if (level == SkipLevel::kSkipDraws &&
        (kind == GpuSkipStats::kIndexed || kind == GpuSkipStats::kInstanced ||
         kind == GpuSkipStats::kUp)) {
        if (kind == GpuSkipStats::kUp && t_point_tests)
            g_kept_point_tests.fetch_add(1, std::memory_order_relaxed);
        else
            g_kept_pass.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

}  // namespace

const char* SkipLevelName(SkipLevel level) {
    switch (level) {
        case SkipLevel::kFull: return "full";
        case SkipLevel::kSkipDraws: return "skip_draws";
        case SkipLevel::kSwapOnly: return "swap_only";
    }
    return "full";
}

void LatchGpuSkip(bool capture_on, bool recording, int proc) {
    TrackSettings();
    const SkipLevel want = WantedLevel(g_renderer_native.load(std::memory_order_relaxed),
                                       SkipLevel(g_skip_setting.load(std::memory_order_relaxed)),
                                       capture_on, recording);
    const int full = g_full_requested.exchange(0);
    bool was_fresh, fresh;
    SkipLevel level;
    int whole;
    {
        std::lock_guard lock(g_latch_mutex);
        was_fresh = g_latch.Fresh();
        level = g_latch.EndFrame(want, full, proc);
        fresh = g_latch.Fresh();
        whole = g_latch.WholeFrames();
    }
    g_whole.store(whole);
    g_frames.fetch_add(1, std::memory_order_relaxed);
    if (level != SkipLevel::kFull) g_frames_skipped.fetch_add(1, std::memory_order_relaxed);
    g_fresh.store(fresh);
    g_frame_level.store(int(level));
    // Both renderers would sample an outfit that was never composed: it's
    // read back to be compressed, and swap_only skipped the draws and the
    // resolve it's read back from
    if (level == SkipLevel::kSwapOnly && g_compress_textures.load(std::memory_order_relaxed) &&
        !g_warned_compress.exchange(true)) {
        REXLOG_WARN("emulated GPU: swap_only with compress_character_textures on: outfits "
                    "composed now aren't drawn or resolved before they're read back to be "
                    "compressed, so both renderers may show them black");
    }
    if (fresh && !was_fresh) {
        std::lock_guard lock(g_callback_mutex);
        if (g_on_fresh) g_on_fresh();
    }
}

void SetPassWantsDraws(bool wants) {
    if (wants && SkipLevel(g_frame_level.load(std::memory_order_relaxed)) == SkipLevel::kSwapOnly)
        g_passes_dropped.fetch_add(1, std::memory_order_relaxed);
    g_pass_wants_draws.store(wants, std::memory_order_relaxed);
}

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
    s.passes_dropped = g_passes_dropped.load(std::memory_order_relaxed);
    s.frames = g_frames.load(std::memory_order_relaxed);
    s.frames_skipped = g_frames_skipped.load(std::memory_order_relaxed);
    s.level = g_renderer_native.load() ? SkipLevel(g_skip_setting.load()) : SkipLevel::kFull;
    s.skip_mode = s.level != SkipLevel::kFull;
    s.skipping = SkipLevel(g_frame_level.load()) != SkipLevel::kFull;
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
    s.passes_dropped -= before.passes_dropped;
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
// the rest, kept inside DoPointTests at skip_draws
extern "C" REX_FUNC(rex_sub_82863B70) {
    if (band3::render::Emit(GpuSkipStats::kUp)) __imp__rex_sub_82863B70(ctx, base);
}

// The clear path under D3DDevice_Clear (once, or once per rect) and
// D3DDevice_BeginTiling (band3_recomp.188.cpp, .176.cpp): it reads the
// render targets' size into its stack and calls the emitter (0x82862E30),
// which flushes the pending state, writes the clear's packets and marks
// what it overwrote pending again. Skipped, only those packets, the ring
// cursor, the pending bits and two of the device's flag bytes (+10941,
// +10943) are left as they were, as a skipped draw leaves them. Both
// callers are done with r3 when it returns: Clear is void, and BeginTiling
// sets r3 again before its next call.
extern "C" REX_FUNC(rex_sub_828634E0) {
    if (band3::render::Emit(GpuSkipStats::kClear)) __imp__rex_sub_828634E0(ctx, base);
}

// D3DDevice_Resolve: every resolve RB3 makes (DxRnd's Save{Pre,Post}Buffer,
// GetCurrentFrameTex, EndTiling, ModalDraw; DxTex's Select, ResolveMipChain
// and LockBitmap; the spotlights' BlurRT) and D3DDevice_EndTiling's per tile.
// It's void, and no caller reads r3 after it. Skipped, what it would have
// left on the CPU side stays as it was: the resolve's shadow registers
// (device +10776..+10820), the pending masks, the device's flag bytes
// (+10941..+10943), the destination texture's fence and its record in the
// buffer-resource space (+13736). Of those only the fence is read by RB3
// (a Lock waits for it), which then doesn't wait: nothing was written.
extern "C" REX_FUNC(rex_sub_82855F18) {
    if (band3::render::Emit(GpuSkipStats::kResolve)) __imp__rex_sub_82855F18(ctx, base);
}

// DxRnd::DoPointTests: the lens flares' occlusion queries, issued and read
// back (BlockOnFence) whatever the frame; their quads drawn but at swap_only
extern "C" REX_FUNC(DxRnd__DoPointTests) {
    band3::render::t_point_tests = true;
    __imp__DxRnd__DoPointTests(ctx, base);
    band3::render::t_point_tests = false;
}
