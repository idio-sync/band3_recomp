#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>

#include "src/Render/frame_compose.h"

// While the native picture shows (renderer both), the emulated GPU skips
// drawing what nobody sees. gpu_skip.cpp overrides the guest's D3D emitters
// (the generated functions are weak) to return without writing packets in a
// skipped frame. Capture reads guest objects and the device's CPU shadow,
// which skipping leaves intact. Levels (emulated_gpu_while_native):
//
// - full: nothing skipped.
// - skip_draws (default): DrawIndexedVertices, the instanced draw and
//   DrawVerticesUP, except the draws of a texture pass not drawn regularly
//   (or one of a pass's first two; scene_capture.cpp's BeginPass: outfits,
//   portraits composed once) and DxRnd::DoPointTests' occlusion quads,
//   whose results the game reads back.
// - swap_only: also clears (ClearRect path under D3DDevice_Clear and
//   BeginTiling), every D3DDevice_Resolve, one-shot passes and flare quads.
//   Kept: BeginIndexedVertices (its caller writes through the returned
//   pointers), Begin/EndVertices (DxParticleSys::DrawParticles),
//   Begin/EndTiling's own packets, predication, fences, occlusion query
//   Begin/End (the CP writes the sample count on EVENT_WRITE_ZPD), uploads
//   the kept draws flush, swaps. F8 back to emulated then shows one-shot
//   pictures black and render targets stale until redrawn (passes_dropped).
//
// Frames are skipped whole at one level, decided at the end of
// DxRnd::Present (after capture), so skipping happens only in frames the
// native renderer has whole.

namespace band3::render {

enum class SkipLevel { kFull = 0, kSkipDraws = 1, kSwapOnly = 2 };

// The setting while the native picture shows and capture records texture
// passes; kFull otherwise
inline SkipLevel WantedLevel(bool renderer_native, SkipLevel setting, bool capture_on,
                             bool recording) {
    return renderer_native && capture_on && recording ? setting : SkipLevel::kFull;
}

// The per-frame decision and emulated-picture freshness, unit-testable. Not
// thread-safe: called on the game's render thread at each Present.
class SkipLatch {
 public:
    // At Present: `want` from WantedLevel; `full`, whole frames requested
    // (RequestFullFrames), which take precedence; `proc`, the frame's
    // ProcCommands (kProcWorld/kProcPost), -1 if no DoPostProcess. Returns
    // the next frame's level. A skipped frame at any level is stale.
    SkipLevel EndFrame(SkipLevel want, int full, int proc = -1) {
        if (Skipping()) {
            whole_ = 0;
            world_ = false;
            game_frames_ = 0;
        } else {
            whole_ = std::min(whole_ + 1, kWholeMax);
            const bool world = proc >= 0 && (proc & kProcWorld);
            const bool post = proc >= 0 && (proc & kProcPost);
            world_ |= world;
            // Whether this frame's picture is the game's. With even/odd
            // rendering (frame_compose.h) a post frame shows the previous
            // world frame, so it's the game's only once a whole world frame
            // preceded it since skipping stopped.
            bool game = game_frames_ > 0;
            if (proc < 0 || (world && post)) game = true;
            else if (post) game = world_;
            game_frames_ = game ? std::min(game_frames_ + 1, kWholeMax) : 0;
        }
        full_ = std::max(full_, full);
        if (full_ > 0) {
            full_--;
            level_ = SkipLevel::kFull;
        } else {
            level_ = want;
        }
        return level_;
    }
    // returns whether the next frame is skipped
    bool EndFrame(bool want, int full, int proc = -1) {
        return EndFrame(want ? SkipLevel::kSkipDraws : SkipLevel::kFull, full, proc) !=
               SkipLevel::kFull;
    }
    bool Skipping() const { return level_ != SkipLevel::kFull; }
    SkipLevel Level() const { return level_; }
    // A game frame was swapped, plus kPresenterLag whole frames after it: the
    // emulated presenter shows the frame before the one just presented.
    // True before the first frame.
    bool Fresh() const { return game_frames_ > kPresenterLag; }
    // frames swapped whole since the last skipped one (2 initially)
    int WholeFrames() const { return whole_; }
    int FullPending() const { return full_; }

 private:
    static constexpr int kWholeMax = 1 << 20;
    static constexpr int kPresenterLag = 1;
    SkipLevel level_ = SkipLevel::kFull;
    int whole_ = 2;
    int full_ = 0;
    // a world frame swapped whole since the last skip; consecutive game
    // frames (Fresh)
    bool world_ = true;
    int game_frames_ = kPresenterLag + 1;
};

// Emitter calls since start, by kind; kept_* are draws a skip_draws frame
// emitted for a one-shot pass or DoPointTests.
struct GpuSkipStats {
    enum Kind { kBeginIndexed, kIndexed, kInstanced, kUp, kClear, kResolve, kNumKinds };
    static constexpr const char* kKindNames[kNumKinds] = {
        "begin_indexed", "indexed", "instanced", "up", "clear", "resolve"};
    uint64_t emitted[kNumKinds] = {};
    uint64_t skipped[kNumKinds] = {};
    uint64_t kept_pass = 0;
    uint64_t kept_point_tests = 0;
    // passes wanting their draws that opened in a swap_only frame: what F8
    // back may show black or stale
    uint64_t passes_dropped = 0;
    uint64_t frames = 0;
    uint64_t frames_skipped = 0;
    // current state, not counts
    SkipLevel level = SkipLevel::kFull;
    bool skip_mode = false;
    bool skipping = false;
    bool fresh = true;
};

const char* SkipLevelName(SkipLevel level);

// Whether an emitter call of `kind` (GpuSkipStats::Kind) writes its packets
// at `level`. `pass_wants_draws`: SetPassWantsDraws; `point_tests`: inside
// DxRnd::DoPointTests.
inline bool EmitDraw(SkipLevel level, int kind, bool pass_wants_draws, bool point_tests) {
    if (level == SkipLevel::kFull || kind == GpuSkipStats::kBeginIndexed) return true;
    if (level == SkipLevel::kSwapOnly) return false;
    if (kind == GpuSkipStats::kClear || kind == GpuSkipStats::kResolve) return true;
    return pass_wants_draws || (kind == GpuSkipStats::kUp && point_tests);
}
GpuSkipStats GetGpuSkipStats();
// what `now` counted since `before` (the flags are now's)
GpuSkipStats GpuSkipStatsSince(const GpuSkipStats& now, const GpuSkipStats& before);

// At the end of the DxRnd::Present hook, after capture: decides the next
// frame's level. `recording`: texture passes are recorded; `proc`: see
// SkipLatch::EndFrame.
void LatchGpuSkip(bool capture_on, bool recording, int proc);

// scene_capture.cpp's BeginPass: a one-shot texture pass is open, so its
// draws reach the emulated GPU (dropped and counted at swap_only)
void SetPassWantsDraws(bool wants);

// renderer is native, as the game's thread sees it
bool RendererNative();

// SkipLatch::Fresh: the emulated picture is the game's. Any thread.
bool EmulatedPictureFresh();

// SkipLatch::WholeFrames, any thread
int EmulatedWholeFrames();

// Consecutive whole frames before a render check holds a frame
// (HoldIfRequested). Fresh isn't enough: state the GPU accumulates over
// frames recovers slowly after skipping (the title's clouds took 5-20).
inline constexpr int kWholeFramesToHold = 30;

// Draw the next `frames` frames whole regardless of the setting (the
// harness's `capture`, so the emulated GPU draws what it screenshots). Any
// thread.
void RequestFullFrames(int frames);

// Called on the game's thread when the emulated picture turns fresh after
// being stale; the native drawer keeps drawing the window until then after
// F8 back. Null to clear; any thread.
void SetEmulatedFreshCallback(std::function<void()> callback);

}  // namespace band3::render
