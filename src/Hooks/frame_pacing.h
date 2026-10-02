#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

// The background's frame rate under RB3's even/odd rendering. The venue's
// RndPostProc asks for a rate (emulate_fps, 30 in most venues), and
// ProcCounter::SetEmulateFPS turns it into how many frames apart the world is
// drawn: 120 / fps half-frames, so 2 frames at 30 and 2 and 3 by turns at 24.
// The 120 assumes the game runs at 60, so at refresh_rate 120 a venue's 30
// draws at 60, and at 180 at 90. band3 counts from the rate the game really
// runs at instead, and can ask for its own background rate.

namespace band3::pacing {

// The world's period in half-frames, as SetEmulateFPS stores it (ProcCounter
// +8 holds half of it and +12 the odd half-frame, alternated), for a game
// running at `game_hz` whose venue asks for `venue_fps`, with the background
// at `background_fps` (0: the venue's rate). 0 when there's nothing to draw
// at a lower rate: the venue asks for none (no post-processing), which the
// game keeps drawing every frame. At least 2 (every frame).
inline int32_t WorldHalfFrames(double game_hz, int32_t venue_fps, int32_t background_fps) {
    if (venue_fps <= 0) return 0;
    if (!(game_hz > 0)) game_hz = 60;
    // SetEmulateFPS keeps a venue's rate to 1..60
    const double fps = background_fps > 0 ? background_fps : std::clamp(venue_fps, 1, 60);
    const double half_frames = std::round(2 * game_hz / fps);
    return static_cast<int32_t>(std::clamp(half_frames, 2.0, 2 * game_hz));
}

// The most frames apart the world is drawn now (ProcCounter's period, the
// longer of its two when it alternates), 0 or 1 when it's drawn every frame.
// Set by the game thread every frame (ProcCounter::SetEmulateFPS), for what
// keeps a world frame for the frames after it (scene_capture.cpp).
inline std::atomic<uint32_t> g_world_period{0};

inline uint32_t WorldPeriod() { return g_world_period.load(std::memory_order_relaxed); }

// ProcCounter's period and odd half-frame (+8, +12) as the frames between
// world frames at most: the odd half-frame is added to the period at the
// period's end and negated, so it's -1 while the longer one runs
inline uint32_t MaxPeriod(int32_t period, int32_t odd_half) {
    return static_cast<uint32_t>(std::max({period, period + odd_half, 0}));
}

}  // namespace band3::pacing
