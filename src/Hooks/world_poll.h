#pragma once

#include <cstdint>

// How far apart RB3 polls its worlds. WorldDir::Poll runs every frame but
// polls the venue (its characters, camera, hair) only on frames that aren't
// world-only under even/odd rendering (ProcCounter's proc_cmds 1): at a
// venue's 30 fps every other frame, and with background_fps 60 (proc_cmds 7)
// every frame. Code the poll runs that steps once per poll assumes it runs at
// the venue's emulate_fps (CamShot::Shake, CharHair::GetFPS), so it ran twice
// as fast at 60.

namespace band3::world_poll {

// The game frames (that moved the clock: paused ones don't count) since the
// world being polled now was last polled, while a WorldDir::Poll polls; 0
// outside one
uint32_t FramesSincePoll();

// How many of the steps a per-poll update assumes this poll stands for: the
// time since the last poll (FramesSincePoll at `game_hz`) over the
// 1 / `assumed_fps` it assumes. 1 outside a poll or with nothing known.
inline float StepScale(uint32_t frames, double game_hz, float assumed_fps) {
    if (!frames || !(game_hz > 0) || !(assumed_fps > 0)) return 1.0f;
    return static_cast<float>(assumed_fps * frames / game_hz);
}

}  // namespace band3::world_poll
