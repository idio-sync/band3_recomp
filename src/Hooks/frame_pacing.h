#pragma once

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

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

// The frame cap (frame_cap). The emulated console paces RB3: each Present
// queues a wait for the next vertical blank, which the SDK's "GPU VSync"
// thread raises every 1/refresh_rate on a 1 ms sleep, so frames come late by
// up to a millisecond and never at the display's own rate (119.88 Hz runs at
// 120). With the cap on, band3 waits at the end of each Present instead, to
// the display's rate exactly, and the SDK's vsync is turned off so its
// vblanks (then every 1 ms) don't hold the game as well.

// When each frame may go on: one period apart, on a fixed beat, so a frame
// that ends early waits for its beat and one that ends a little late (by less
// than a period) goes on at once and leaves the next a shorter wait, keeping
// the beat. One later than that (a hitch, a render check holding the game,
// F8's drain) starts the beat again from itself rather than letting the
// frames after it run back to back to catch up.
class FrameCapSchedule {
public:
    // 0 turns the cap off; another period starts the beat again
    void SetPeriod(int64_t period_ns) {
        if (period_ns == period_) return;
        period_ = period_ns;
        started_ = false;
    }
    int64_t Period() const { return period_; }

    // `now_ns` is when a frame ended; returns when the next may start (never
    // before now_ns)
    int64_t Next(int64_t now_ns) {
        if (period_ <= 0) return now_ns;
        if (!started_) {
            started_ = true;
            deadline_ = now_ns + period_;
            return now_ns;
        }
        const int64_t due = deadline_;
        if (now_ns <= due) {
            deadline_ = due + period_;
            return due;
        }
        if (now_ns - due < period_) {
            late_++;
            deadline_ = due + period_;
            return now_ns;
        }
        resets_++;
        deadline_ = now_ns + period_;
        return now_ns;
    }

    // frames that ended after their beat by less than a period, and the
    // times a later one started the beat again
    uint64_t Late() const { return late_; }
    uint64_t Resets() const { return resets_; }

private:
    int64_t period_ = 0;
    int64_t deadline_ = 0;
    bool started_ = false;
    uint64_t late_ = 0;
    uint64_t resets_ = 0;
};

enum class FrameCapMode { kOff, kDisplay, kAuto, kFixed };

struct FrameCapSetting {
    FrameCapMode mode = FrameCapMode::kOff;
    double hz = 0;  // kFixed's rate
};

// a number of Hz is kept to this, the range the SDK's VdQueryVideoMode keeps
// the guest's refresh rate to
inline constexpr double kMinCapHz = 24;
inline constexpr double kMaxCapHz = 240;

// frame_cap's value: display, auto, off, or a number of Hz (kept to
// kMinCapHz..kMaxCapHz); nothing for anything else
inline std::optional<FrameCapSetting> ParseFrameCap(std::string_view v) {
    if (v == "display") return FrameCapSetting{FrameCapMode::kDisplay};
    if (v == "auto") return FrameCapSetting{FrameCapMode::kAuto};
    if (v == "off") return FrameCapSetting{FrameCapMode::kOff};
    double hz = 0;
    const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), hz);
    if (v.empty() || ec != std::errc{} || end != v.data() + v.size() || !std::isfinite(hz) ||
        hz <= 0)
        return std::nullopt;
    return FrameCapSetting{FrameCapMode::kFixed, std::clamp(hz, kMinCapHz, kMaxCapHz)};
}

inline const char* FrameCapModeName(FrameCapMode mode) {
    switch (mode) {
    case FrameCapMode::kDisplay: return "display";
    case FrameCapMode::kAuto: return "auto";
    case FrameCapMode::kFixed: return "fixed";
    case FrameCapMode::kOff: break;
    }
    return "off";
}

// the cap as it runs: off (period 0), or a rate and its period
struct FrameCap {
    FrameCapMode mode = FrameCapMode::kOff;
    double hz = 0;
    int64_t period_ns = 0;
};

// auto's rate: below the display's by 5%, and by at least 4 fps, which keeps
// a VRR display's frames inside its range (each shown as it's made) where a
// cap at its refresh rate would sometimes land past it and wait for a vblank
inline double AutoCapHz(double refresh_hz) {
    return refresh_hz - std::max(4.0, 0.05 * refresh_hz);
}

// `setting` for a display refreshing at refresh_num / refresh_den Hz
// (120000/1001 for 119.88), or one whose rate can't be told (either 0):
// display and auto are then off, and a number of Hz still holds
inline FrameCap ResolveFrameCap(FrameCapSetting setting, uint32_t refresh_num,
                                uint32_t refresh_den) {
    const bool known = refresh_num > 0 && refresh_den > 0;
    auto at = [&](double hz) {
        return FrameCap{setting.mode, hz, static_cast<int64_t>(std::llround(1e9 / hz))};
    };
    switch (setting.mode) {
    case FrameCapMode::kDisplay:
        if (!known) break;
        // the period from the fraction itself, rounded to the nanosecond
        return FrameCap{setting.mode, double(refresh_num) / refresh_den,
                        static_cast<int64_t>((uint64_t(1'000'000'000) * refresh_den +
                                              refresh_num / 2) /
                                             refresh_num)};
    case FrameCapMode::kAuto:
        if (!known) break;
        return at(AutoCapHz(double(refresh_num) / refresh_den));
    case FrameCapMode::kFixed:
        if (setting.hz > 0) return at(setting.hz);
        break;
    case FrameCapMode::kOff: break;
    }
    return {};
}

// The cap's running half, in graphics.cpp.

// Starts the cap at startup, once the GPU plugin is loaded and before the
// runtime starts its vblank thread (Band3App::StartFrameCap): reads the display under `native_window`, turns the SDK's vsync
// off and sets the guest's refresh rate to the cap's when the cap is on, then
// re-reads the display every couple of seconds and frame_cap whenever it
// changes. Changing vsync later goes through `post_to_ui`, which runs a task
// on the UI thread.
void StartFrameCap(void* native_window, std::function<void(std::function<void()>)> post_to_ui);
void StopFrameCap();

// Called by the game thread at the end of each Present (scene_capture.cpp):
// waits for the frame's beat while the cap is on
void PaceFrame();

// The rate the game runs at: the cap's while it's on, else refresh_rate
// (video_mode_refresh_rate, 0 for the console's 60)
double GameHz();

// what the cap has done since it started, for the test harness's
// present_stats; totals, which it subtracts
struct FrameCapStats {
    FrameCapMode mode = FrameCapMode::kOff;
    double hz = 0;
    uint64_t frames = 0;  // frames paced
    uint64_t late = 0, resets = 0;
    double wait_ms = 0;  // waiting for the beat, the spin included
    double spin_ms = 0;  // spinning at the end of each wait
};
FrameCapStats GetFrameCapStats();

}  // namespace band3::pacing
