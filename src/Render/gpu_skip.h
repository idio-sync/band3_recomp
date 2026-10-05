#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>

#include "src/Render/frame_compose.h"

// Experimental (N3): while the native renderer draws the window (renderer =
// native), the emulated GPU stops drawing what nobody sees. gpu_skip.cpp
// overrides the guest's D3D emitters (the generated functions are weak, so a
// strong definition replaces them), which return without writing their
// packets while a frame is skipped. Capture doesn't need the packets: it
// reads guest objects and the device's CPU shadow (scene_capture.cpp), which
// skipping leaves as it was. How much is skipped is emulated_gpu_while_native
// (Band3 -> Graphics):
//
// - full: nothing, as under renderer = emulated.
// - skip_draws (the default): DrawIndexedVertices, the instanced draw and
//   DrawVerticesUP. Clears, resolves, fences, swaps, BeginIndexedVertices
//   and Begin/EndVertices still reach the emulated GPU, and so do these, so
//   that its picture is right again as soon as it draws whole frames (F8
//   back to emulated):
//   - the draws of a texture pass that isn't drawn regularly
//     (scene_capture.cpp's BeginPass: RB3 composes outfits, portraits and
//     the like once and samples them for the session), and of the first two
//     of any pass;
//   - the flares' occlusion-test quads (DxRnd::DoPointTests), whose results
//     the game reads back; with no scene depth drawn every flare is visible.
// - swap_only: only what the game waits on. The clears (the ClearRect path
//   under D3DDevice_Clear and BeginTiling) and every D3DDevice_Resolve
//   (EndTiling's per tile included) are skipped too, and so are the one-shot
//   passes' draws and the flares' quads. What still reaches the GPU:
//   BeginIndexedVertices' draws (its caller writes through the pointers it
//   returns: out/n3/experiment.md 0), Begin/EndVertices (the particles'
//   draws: DxParticleSys::DrawParticles calls BeginVertices itself),
//   BeginTiling and EndTiling's own packets, predication, fences, the occlusion queries'
//   Begin/End (the command processor writes their sample count on the end's
//   EVENT_WRITE_ZPD, query_occlusion_fake_sample_count, with or without a
//   draw between), shader and constant uploads the kept draws flush, and
//   the swaps. A test and performance mode: F8 back to emulated shows black
//   outfits, portraits and other pictures RB3 drew once while it was on, and
//   stale render targets, until RB3 draws them again (passes_dropped counts
//   them).
//
// Frames are skipped whole or not at all, at one level for the frame: what
// the next one does is decided at the end of the game's DxRnd::Present
// (scene_capture.cpp's hook, after the frame was captured), from the
// setting, renderer, and capture being on with texture passes recorded, so
// anything is skipped only in frames the native renderer has whole.

namespace band3::render {

// How much of a frame the emulated GPU skips: emulated_gpu_while_native's
// full, skip_draws and swap_only (above)
enum class SkipLevel { kFull = 0, kSkipDraws = 1, kSwapOnly = 2 };

// The level the next frame would be skipped at as things stand: the setting
// only while renderer is native and capture records the frame with its
// texture passes; whole otherwise, so under renderer = emulated nothing is
// ever skipped
inline SkipLevel WantedLevel(bool renderer_native, SkipLevel setting, bool capture_on,
                             bool recording) {
    return renderer_native && capture_on && recording ? setting : SkipLevel::kFull;
}

// The per-frame decision and whether the emulated GPU's picture is fresh,
// kept apart from the game so the unit tests can check them. Not
// thread-safe: gpu_skip.cpp calls it on the game's render thread at each
// Present.
class SkipLatch {
 public:
    // The frame being drawn has just been swapped (its Present ran): `want`,
    // the level the next would be skipped at as things stand (WantedLevel);
    // `full`, whole frames asked for since the last call (RequestFullFrames),
    // which go first; `proc`, what the swapped frame drew (its ProcCommands,
    // frame_compose.h's kProcWorld and kProcPost), or -1 if it didn't say (no
    // DoPostProcess). Returns the next frame's level. A frame skipped at any
    // level counts as stale: neither level's picture is the game's.
    SkipLevel EndFrame(SkipLevel want, int full, int proc = -1) {
        if (Skipping()) {
            whole_ = 0;
            world_ = false;
            game_frames_ = 0;
        } else {
            // a frame drawn while not skipping was drawn whole
            whole_ = std::min(whole_ + 1, kWholeMax);
            const bool world = proc >= 0 && (proc & kProcWorld);
            const bool post = proc >= 0 && (proc & kProcPost);
            world_ |= world;
            // Whether this frame's picture is the game's. With even/odd
            // rendering a post frame shows the world frame before it,
            // post-processed, and a frame that post-processes nothing shows
            // the post buffer the last post frame made (frame_compose.h): the
            // game's once a post frame has followed a whole world frame,
            // whichever came first after skipping stopped. A frame that draws
            // its world and post-processes it (7), or doesn't say, is.
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
    // skip_draws when `want`, else full; returns whether the next frame is
    // skipped
    bool EndFrame(bool want, int full, int proc = -1) {
        return EndFrame(want ? SkipLevel::kSkipDraws : SkipLevel::kFull, full, proc) !=
               SkipLevel::kFull;
    }
    // whether the frame being drawn now is skipped, at whichever level
    bool Skipping() const { return level_ != SkipLevel::kFull; }
    // the frame being drawn now's level
    SkipLevel Level() const { return level_; }
    // The emulated GPU's picture is the game's: a frame whose picture is the
    // game's (EndFrame) was swapped, and kPresenterLag whole frames after it.
    // The emulated GPU swaps a frame after the game's Present returns, so its
    // presenter shows the frame before the one just presented: after F8 back
    // with one frame fewer, the window showed a black world for a frame
    // (out/n5/fix). True before the first frame.
    bool Fresh() const { return game_frames_ > kPresenterLag; }
    // the frames swapped whole since the last skipped one (2 before the
    // first frame, at most kWholeMax)
    int WholeFrames() const { return whole_; }
    // whole frames still to come from a request
    int FullPending() const { return full_; }

 private:
    static constexpr int kWholeMax = 1 << 20;
    static constexpr int kPresenterLag = 1;
    SkipLevel level_ = SkipLevel::kFull;
    int whole_ = 2;
    int full_ = 0;
    // a world frame was swapped whole since the last skipped one; and the
    // frames swapped in a row whose pictures are the game's (Fresh)
    bool world_ = true;
    int game_frames_ = kPresenterLag + 1;
};

// The emitters' calls since the game started, by kind: BeginIndexedVertices
// (never skipped, counted for what the emulated GPU still does),
// DrawIndexedVertices, the instanced draw, DrawVerticesUP, the ClearRect path
// and D3DDevice_Resolve; and of the emitted ones, those emitted in a frame
// skipped at skip_draws because a one-shot texture pass wanted them or they
// were DoPointTests' occlusion-test quads.
struct GpuSkipStats {
    enum Kind { kBeginIndexed, kIndexed, kInstanced, kUp, kClear, kResolve, kNumKinds };
    static constexpr const char* kKindNames[kNumKinds] = {
        "begin_indexed", "indexed", "instanced", "up", "clear", "resolve"};
    uint64_t emitted[kNumKinds] = {};
    uint64_t skipped[kNumKinds] = {};
    uint64_t kept_pass = 0;
    uint64_t kept_point_tests = 0;
    // texture passes that wanted their draws (one-shot, or one of a pass's
    // first two) opened in a frame skipped at swap_only, whose draws the
    // emulated GPU didn't get: what F8 back may show black or stale
    uint64_t passes_dropped = 0;
    // the game's frames (Presents), and of those the ones skipped
    uint64_t frames = 0;
    uint64_t frames_skipped = 0;
    // now: emulated_gpu_while_native's level while renderer is native (full
    // otherwise), skip_mode its level isn't full, the frame being drawn is
    // skipped, and the emulated picture is fresh
    SkipLevel level = SkipLevel::kFull;
    bool skip_mode = false;
    bool skipping = false;
    bool fresh = true;
};

// "full", "skip_draws" or "swap_only"
const char* SkipLevelName(SkipLevel level);

// Whether a call of an emitter of `kind` (GpuSkipStats::Kind) writes its
// packets in a frame at `level`: `pass_wants_draws`, a texture pass that
// isn't drawn regularly is open (SetPassWantsDraws); `point_tests`, the call
// is inside DxRnd::DoPointTests. Full emits everything and swap_only only
// BeginIndexedVertices; skip_draws keeps clears and resolves too, and the
// draws a one-shot pass wants or DoPointTests makes.
inline bool EmitDraw(SkipLevel level, int kind, bool pass_wants_draws, bool point_tests) {
    if (level == SkipLevel::kFull || kind == GpuSkipStats::kBeginIndexed) return true;
    if (level == SkipLevel::kSwapOnly) return false;
    if (kind == GpuSkipStats::kClear || kind == GpuSkipStats::kResolve) return true;
    return pass_wants_draws || (kind == GpuSkipStats::kUp && point_tests);
}
GpuSkipStats GetGpuSkipStats();
// what `now` counted since `before` (the flags are now's)
GpuSkipStats GpuSkipStatsSince(const GpuSkipStats& now, const GpuSkipStats& before);

// At the end of the game's DxRnd::Present hook, once the frame is captured:
// decides whether the next frame is skipped. `capture_on`: capture is on;
// `recording`: texture passes are recorded (native_view_record_targets, or
// renderer has been native); `proc`: the frame's ProcCommands, -1 if its
// DoPostProcess didn't run (SkipLatch::EndFrame).
void LatchGpuSkip(bool capture_on, bool recording, int proc);

// scene_capture.cpp's BeginPass: the texture pass opening isn't one drawn
// regularly, so its draws reach the emulated GPU, except at swap_only, where
// it's counted dropped (GpuSkipStats::passes_dropped); false when it ends or
// is dropped
void SetPassWantsDraws(bool wants);

// renderer is native, as the game's thread sees it (kept by its change
// callback)
bool RendererNative();

// Whether the emulated GPU's picture is the game's now (SkipLatch::Fresh): a
// screenshot of it is, and F8 back to emulated can show it. Any thread.
bool EmulatedPictureFresh();

// SkipLatch::WholeFrames now, for any thread
int EmulatedWholeFrames();

// How many whole frames the emulated GPU must have swapped in a row before
// a render check holds a frame (scene_capture.cpp's HoldIfRequested). Fresh
// (two) is the frame's own picture, but not everything in it: what the GPU
// builds up over frames comes back slowly after skipping. The title's clouds
// took 5 to 20 whole frames to come back (out/n3/soak.md); with fewer, its
// render check compared the native frame with a picture lacking them. Under
// renderer emulated, or emulated_gpu_while_native full, every frame is whole,
// so this never waits there.
inline constexpr int kWholeFramesToHold = 30;

// The next `frames` frames are drawn whole whatever the setting, from the
// next one the game begins: the harness's `capture` asks before holding a
// frame, so the emulated GPU draws the frame it screenshots. Any thread.
void RequestFullFrames(int frames);

// Called on the game's thread when the emulated picture turns fresh after
// being stale (skipping stopped and whole frames were swapped: Fresh): the
// native renderer's drawer, which keeps drawing the window until then after
// F8 back to emulated. Null to clear; any thread.
void SetEmulatedFreshCallback(std::function<void()> callback);

}  // namespace band3::render
