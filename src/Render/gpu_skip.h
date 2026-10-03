#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>

#include "src/Render/frame_compose.h"

// Experimental (N3): while the native renderer draws the window (renderer =
// native), the emulated GPU stops drawing what nobody sees. gpu_skip.cpp
// overrides the guest's D3D draw emitters (the generated functions are weak,
// so a strong definition replaces them): DrawIndexedVertices, the instanced
// draw and DrawVerticesUP return without writing their packets while a frame
// is skipped. Everything else still reaches the emulated GPU: clears,
// resolves, fences, swaps, BeginIndexedVertices (its caller writes through
// the pointers it returns: out/n3/experiment.md 0) and Begin/EndVertices.
// Capture doesn't need the packets: it reads guest objects and the device's
// CPU shadow (scene_capture.cpp), which skipping leaves as it was.
//
// What still draws, so the emulated GPU's picture is right again as soon as
// it draws whole frames (F8 back to emulated):
// - a texture pass that isn't drawn regularly (scene_capture.cpp's
//   BeginPass: RB3 composes outfits, portraits and the like once and samples
//   them for the session), and the first two of any pass;
// - the flares' occlusion queries (DxRnd::DoPointTests), whose results the
//   game reads back; with no scene depth drawn every flare is visible.
//
// Frames are skipped whole or not at all: whether the next one is is decided
// at the end of the game's DxRnd::Present (scene_capture.cpp's hook, after
// the frame was captured), from emulated_gpu_while_native (Band3 ->
// Graphics, skip_draws by default, full draws everything), renderer, and
// capture being on with texture passes recorded, so draws are skipped only
// in frames the native renderer has whole.

namespace band3::render {

// The per-frame decision and whether the emulated GPU's picture is fresh,
// kept apart from the game so the unit tests can check them. Not
// thread-safe: gpu_skip.cpp calls it on the game's render thread at each
// Present.
class SkipLatch {
 public:
    // The frame being drawn has just been swapped (its Present ran): `want`,
    // whether the next would be skipped as things stand; `full`, whole frames
    // asked for since the last call (RequestFullFrames), which go first;
    // `proc`, what the swapped frame drew (its ProcCommands, frame_compose.h's
    // kProcWorld and kProcPost), or -1 if it didn't say (no DoPostProcess).
    // Returns whether the next frame is skipped.
    bool EndFrame(bool want, int full, int proc = -1) {
        if (skipping_) {
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
            skipping_ = false;
        } else {
            skipping_ = want;
        }
        return skipping_;
    }
    // whether the frame being drawn now is skipped
    bool Skipping() const { return skipping_; }
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
    bool skipping_ = false;
    int whole_ = 2;
    int full_ = 0;
    // a world frame was swapped whole since the last skipped one; and the
    // frames swapped in a row whose pictures are the game's (Fresh)
    bool world_ = true;
    int game_frames_ = kPresenterLag + 1;
};

// The draw emitters' calls since the game started, by kind: BeginIndexedVertices
// (never skipped, counted for what the emulated GPU still does),
// DrawIndexedVertices, the instanced draw and DrawVerticesUP; and of the
// emitted ones, those emitted in a skipped frame because a one-shot texture
// pass wanted them or they were DoPointTests' occlusion queries.
struct GpuSkipStats {
    enum Kind { kBeginIndexed, kIndexed, kInstanced, kUp, kNumKinds };
    static constexpr const char* kKindNames[kNumKinds] = {"begin_indexed", "indexed",
                                                          "instanced", "up"};
    uint64_t emitted[kNumKinds] = {};
    uint64_t skipped[kNumKinds] = {};
    uint64_t kept_pass = 0;
    uint64_t kept_point_tests = 0;
    // the game's frames (Presents), and of those the ones skipped
    uint64_t frames = 0;
    uint64_t frames_skipped = 0;
    // now: emulated_gpu_while_native skips (and renderer is native), the
    // frame being drawn is skipped, and the emulated picture is fresh
    bool skip_mode = false;
    bool skipping = false;
    bool fresh = true;
};
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
// regularly, so its draws reach the emulated GPU; false when it ends or is
// dropped
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
