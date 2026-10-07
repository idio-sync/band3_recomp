#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "src/Render/scene_capture.h"

// RB3's even/odd rendering (out/research/m3_design.md 4): ProcCounter,
// emulating 30 fps, has one frame draw the world (proc_cmds 1, resolved to
// the pre-process buffer) and the next post-process it (2). Both copy the
// newest post-processed picture to the screen (DxRnd::DoPostProcess) and
// draw their own overlay over it. A post frame's capture has no world, so it
// is composed with the world frame before it.
//
// The game shows a world frame as the last post buffer under its own
// overlay; so does the live view (RasterOptions::post_buffer,
// ShowsPostBuffer). Replay draws it with its own world, post-processed.
//
// With even/odd rendering off every frame is 7 and nothing is composed.

namespace band3::render {

// ProcCounter::ProcCommands' bits (rb3-xenon rndobj/Rnd.h): 7 without
// even/odd rendering (and on its first frame), else 1 and 2 by turns, and 0
// on frames doing neither at lower emulated rates
inline constexpr uint32_t kProcWorld = 1;
inline constexpr uint32_t kProcPost = 2;

// DxRnd::DoPostProcess ran and recorded post_boundary and proc_cmds (not all
// menu frames run it, nor did older captures)
inline bool ProcKnown(const FrameCapture& fc) { return fc.post_boundary != FrameCapture::kNoPost; }

// or might have: unknown
inline bool DrawsWorld(const FrameCapture& fc) {
    return !ProcKnown(fc) || (fc.proc_cmds & kProcWorld) != 0;
}

// Whether the game's picture shows the capture's world and overlay: a 7
// frame, or a post frame composed with the world before it. Render checks
// compare only these.
inline bool PresentsCapturedWorld(const FrameCapture& fc) {
    if (!(fc.proc_cmds & kProcPost)) return false;
    return (fc.proc_cmds & kProcWorld) || fc.composed;
}

// Whether the game shows the last post buffer under this frame's overlay
// (frames 1 and 0): DoPostProcess copies it to the screen every frame
// (CopyPostProcess), and only a post frame's FinishPostProcess remakes it
// (SavePostBuffer).
inline bool ShowsPostBuffer(const FrameCapture& fc) {
    return ProcKnown(fc) && !(fc.proc_cmds & kProcPost);
}

// For native_view.cpp's per-kind stats: kWorld 1, kPost 2, kBetween 0, kFull 7
// or unknown (menus)
enum class FrameKind { kWorld, kPost, kBetween, kFull };
inline constexpr int kFrameKinds = 4;
inline FrameKind KindOf(const FrameCapture& fc) {
    if (!ProcKnown(fc)) return FrameKind::kFull;
    const bool world = (fc.proc_cmds & kProcWorld) != 0, post = (fc.proc_cmds & kProcPost) != 0;
    if (world && post) return FrameKind::kFull;
    if (world) return FrameKind::kWorld;
    return post ? FrameKind::kPost : FrameKind::kBetween;
}
inline const char* FrameKindName(FrameKind k) {
    switch (k) {
    case FrameKind::kWorld: return "world";
    case FrameKind::kPost: return "post";
    case FrameKind::kBetween: return "between";
    case FrameKind::kFull: break;
    }
    return "full";
}

// Whether the capture alone draws the game's whole picture, so the native
// view (F8) can start showing frames from it
inline bool StartsPicture(const FrameCapture& fc) {
    return fc.whole && (!ProcKnown(fc) || PresentsCapturedWorld(fc));
}

// How old a kept post buffer may be and still be shown: even/odd rendering's
// longest period (6, frame_pacing.h) plus dropped frames, with room to spare.
// Older means the view was off; the frame draws its own world instead.
inline constexpr uint64_t kPostBufferFrames = 16;

// `kept` is a game frame, 0 none
inline bool PostBufferFor(const FrameCapture& fc, uint64_t kept) {
    return kept && kept < fc.game_frame && fc.game_frame - kept <= kPostBufferFrames;
}

inline uint64_t WorldFrameOf(const FrameCapture& fc) {
    return fc.composed && fc.world_frame ? fc.world_frame : fc.game_frame;
}

// Whether the pre-process buffer kept from game frame `kept` (0 none,
// RasterOptions::pre_buffer) is what `fc`'s world reads. The same frame
// counts, for a redraw: the game reads its own last frame, the same on a
// still screen.
inline bool PreBufferFor(const FrameCapture& fc, uint64_t kept) {
    const uint64_t world = WorldFrameOf(fc);
    return kept && kept <= world && world - kept <= kPostBufferFrames;
}

// `frame` (which drew no world) with `world`'s world before its overlay:
// world's draws before its post_boundary and their passes, then frame's
// texture passes before its boundary (minus those world has), then frame's
// draws from its boundary, the result's boundary. Other fields are frame's
// (its post-processing and gamma ran on world's picture), except composed,
// world_frame, the clear colour (world's) and cameras (world's, then
// frame's others). Counts are summed; the render target counts are redone.
std::shared_ptr<FrameCapture> ComposeFrame(const FrameCapture& world, const FrameCapture& frame);

// FrameCapture::rt_sampled, rt_filtered_keys and rt_missing: render targets
// sampled (as diffuse or s5, the shadow map) by (texture, version); of those
// no pass made, the ones in `left_out` and the count of others
void CountRenderTargets(const FrameCapture& fc, const std::vector<uint64_t>& left_out,
                        uint32_t& sampled, uint32_t& missing, std::vector<uint64_t>& filtered);

}  // namespace band3::render
