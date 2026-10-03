#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "src/Render/scene_capture.h"

// Experimental: what the native view draws of a frame under RB3's even/odd
// rendering (out/research/m3_design.md 4). ProcCounter, emulating 30 fps,
// has a frame draw the world (proc_cmds 1: the venue, its crowd, shadows and
// blends, resolved to the pre-process buffer) and the next post-process it
// (2: post-processing on the saved buffer); both then copy the newest
// post-processed picture to the screen (DxRnd::DoPostProcess) and draw their
// own overlay (track, HUD) over it. So a post frame presents the world frame
// before it with its own overlay, which its capture alone doesn't have: its
// draws before post_boundary are no world. Composed, it has.
//
// A world frame's capture is published as it is, its own world with its own
// overlay, but the game presents the post buffer there, the last post
// frame's picture (the world frame before it, post-processed), under the
// world frame's overlay: so does the live view, which keeps that picture
// (soft_raster.h's RasterOptions::post_buffer, ShowsPostBuffer). Drawn
// alone (replay), it's its own world, post-processed as its parameters say.
// Render checks pair the game's picture with a post frame's capture, where
// the two are the same world and the same overlay (PresentsCapturedWorld).
//
// With even/odd rendering off every frame is 7 (world, post and characters),
// and nothing is composed.

namespace band3::render {

// ProcCounter's ProcCmd bits (rb3-xenon rndobj/Rnd.h), as
// ProcCounter::ProcCommands returns them: 7 every frame without even/odd
// rendering (and on the first frame with it), else 1 and 2 by turns, and 0 on
// frames that do neither when it emulates a lower rate
inline constexpr uint32_t kProcWorld = 1;
inline constexpr uint32_t kProcPost = 2;

// whether the frame says what it drew: DxRnd::DoPostProcess ran, which
// recorded post_boundary and proc_cmds (menus' frames don't all run it, nor
// did captures from before)
inline bool ProcKnown(const FrameCapture& fc) { return fc.post_boundary != FrameCapture::kNoPost; }

// whether the frame drew its own world (or might have: unknown)
inline bool DrawsWorld(const FrameCapture& fc) {
    return !ProcKnown(fc) || (fc.proc_cmds & kProcWorld) != 0;
}

// Whether the game's picture of this frame shows the world and overlay its
// capture has, for a frame that says (ProcKnown): one that post-processed
// the world it drew itself (7), or a post frame composed with the world
// before it. A world frame presents the previous world, and a post frame
// left uncomposed has none.
inline bool PresentsCapturedWorld(const FrameCapture& fc) {
    if (!(fc.proc_cmds & kProcPost)) return false;
    return (fc.proc_cmds & kProcWorld) || fc.composed;
}

// Whether the game's picture of the frame is the post buffer as the last
// post frame left it (its picture before the overlay), under the frame's own
// overlay: a frame that says what it drew and post-processed nothing, a world
// frame (1) or one that drew neither (0). DxRnd::DoPostProcess copies the
// post buffer to the screen every frame (CopyPostProcess), and only a post
// frame's FinishPostProcess makes it anew (SavePostBuffer).
inline bool ShowsPostBuffer(const FrameCapture& fc) {
    return ProcKnown(fc) && !(fc.proc_cmds & kProcPost);
}

// The most game frames after the post frame whose picture it is that the
// live view shows the post buffer (RasterOptions::post_buffer): even/odd
// rendering's longest period (frame_pacing.h, 6 at background_fps 20 and
// refresh_rate 120) and frames the live view didn't get to, with room to
// spare; one older is another moment's (the view was off, or the capture),
// and the frame draws its own world instead.
inline constexpr uint64_t kPostBufferFrames = 16;

// whether a post buffer from game frame `kept` (0 none) is for `fc`'s
inline bool PostBufferFor(const FrameCapture& fc, uint64_t kept) {
    return kept && kept < fc.game_frame && fc.game_frame - kept <= kPostBufferFrames;
}

// `frame` (a frame that drew no world) with `world`'s in front of its overlay:
// world's draws before its post_boundary and the passes they're in, then
// frame's texture passes from before its own (carried ones, say, that its
// overlay samples; those world already has are left out), then frame's draws
// from its post_boundary on, which is where the result's is. Its other
// fields are frame's (its post-processing and gamma ramp too, which the game ran on world's
// picture), but composed (1) and world_frame (world's game_frame), the clear
// colour (world's, whose back buffer it shows) and the cameras (world's,
// then frame's others);
// counts of what was skipped, decoded and so on are the two frames' together,
// and the render targets sampled, missing and filtered are counted again over
// what it has. frame's back-buffer draws before its post_boundary are left
// out (a post frame has none of the world's to draw).
std::shared_ptr<FrameCapture> ComposeFrame(const FrameCapture& world, const FrameCapture& frame);

// render targets the frame's draws sample (as their diffuse texture, or as
// s5: the shadow map), by (texture, version), and of those no pass in it
// made, the ones in `left_out` (made by a pass whose draws were all left out:
// FrameCapture::rt_filtered_keys) and how many others: FrameCapture::
// rt_sampled, rt_filtered_keys and rt_missing
void CountRenderTargets(const FrameCapture& fc, const std::vector<uint64_t>& left_out,
                        uint32_t& sampled, uint32_t& missing, std::vector<uint64_t>& filtered);

}  // namespace band3::render
