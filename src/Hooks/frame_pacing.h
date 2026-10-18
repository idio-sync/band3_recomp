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

// background_fps while a song's music video plays at `video_fps` (0 for none,
// or music_video_full_rate off): the video's rate when that's faster than the
// background would be drawn otherwise (`background_fps`, or with that 0 the
// venue's `venue_fps`), so each of its frames is drawn; else background_fps
inline int32_t BackgroundFpsForVideo(int32_t venue_fps, int32_t background_fps,
                                     double video_fps) {
    if (!(video_fps > 0) || venue_fps <= 0) return background_fps;
    const int32_t would_be = background_fps > 0 ? background_fps : venue_fps;
    const int32_t video = static_cast<int32_t>(std::lround(std::min(video_fps, 240.0)));
    return video > would_be ? video : background_fps;
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
// F8's drain) goes on at once too, and the beats it missed are dropped rather
// than run back to back to catch up: the next frame waits for the first beat
// after it, keeping the beat's phase. Starting a beat of its own from the late
// frame instead put it anywhere against the display's refresh, which
// native_phase_lock had to find again (about half a second of frames missing
// their paint).
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
        // the first beat after now; on one exactly, the one after it
        resets_++;
        deadline_ = due + ((now_ns - due) / period_ + 1) * period_;
        return now_ns;
    }

    // Moves the beat by `ns` (earlier if negative), for PhaseLock; nothing
    // before the beat has started
    void Nudge(int64_t ns) {
        if (started_ && period_ > 0) deadline_ += ns;
    }

    // frames that ended after their beat by less than a period, and the
    // times a later one dropped beats
    uint64_t Late() const { return late_; }
    uint64_t Resets() const { return resets_; }

private:
    int64_t period_ = 0;
    int64_t deadline_ = 0;
    bool started_ = false;
    uint64_t late_ = 0;
    uint64_t resets_ = 0;
};

// kVr: the VR headset's rate while a session runs (vr_game_rate, VrGameCap),
// in place of frame_cap's
enum class FrameCapMode { kOff, kDisplay, kAuto, kFixed, kVr };

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
    case FrameCapMode::kVr: return "vr";
    case FrameCapMode::kOff: break;
    }
    return "off";
}

// Phase lock (native_phase_lock). The cap runs at the display's rate, but on
// its own clock, so where its beat falls against the vertical blanks is
// chance, and drifts with the display's true rate. With the native renderer
// each frame reaches the window (published) some time after the game's
// Present, and the next paint shows it: when that's only a few ms after it's
// published, a frame a little slower than the rest misses its paint, which
// shows the frame before again, and the next paint finds two new frames and
// shows the newer (a repeat and a skip). This steers the beat so frames are
// published a set time before the paint that shows them (a share of the
// refresh, native_phase_lock_slack): the slack, a paint's time less its
// frame's publishing, is measured over kWindow frames, and the beat moved by
// kGain of its median's distance from that target (Correction; half the
// period if none is given), at most a quarter period at a time.
// Times in steady-clock nanoseconds. Not thread-safe.
class PhaseLock {
public:
    static constexpr int kWindow = 30;
    static constexpr int kRecent = 8;  // frames published and not yet shown
    static constexpr double kGain = 0.5;

    // frame `presented_ns` (its Present) reached the window at `now_ns`
    void Published(int64_t presented_ns, int64_t now_ns) {
        recent_[next_++ % kRecent] = {presented_ns, now_ns};
    }

    // A paint at `paint_ns` on a display refreshing every `period_ns` showed
    // frame `serial`, Presented at `presented_ns`. The beat's correction once
    // a window's measured (negative: earlier), else 0. `target_ns`: the slack
    // to steer to, half the period if 0.
    int64_t Shown(uint64_t serial, int64_t presented_ns, int64_t paint_ns, int64_t period_ns,
                  int64_t target_ns = 0) {
        if (!serial || serial == last_serial_ || period_ns <= 0) return 0;
        last_serial_ = serial;
        int64_t published = -1;
        for (const Entry& e : recent_)
            if (e.presented_ns == presented_ns) published = e.published_ns;
        if (published < 0) return 0;
        const int64_t slack = paint_ns - published;
        // a paint can't show a frame before it's published; a long gap is a
        // stall, not the phase
        if (slack < 0 || slack > 3 * period_ns) return 0;
        slack_[count_++] = slack;
        if (count_ < kWindow) return 0;
        count_ = 0;
        return Correction(Median(), period_ns, target_ns);
    }

    // the beat's move for a median slack of `slack_ns`, toward `target_ns`
    // (half the period if 0)
    static int64_t Correction(int64_t slack_ns, int64_t period_ns, int64_t target_ns = 0) {
        const int64_t target = target_ns > 0 ? target_ns : period_ns / 2;
        const double c = kGain * double(slack_ns - target);
        const double most = double(period_ns) / 4;
        return int64_t(std::clamp(c, -most, most));
    }

    void Reset() { *this = PhaseLock{}; }

private:
    struct Entry {
        int64_t presented_ns = -1, published_ns = -1;
    };
    int64_t Median() {
        std::nth_element(slack_, slack_ + kWindow / 2, slack_ + kWindow);
        return slack_[kWindow / 2];
    }

    Entry recent_[kRecent];
    uint64_t next_ = 0;
    uint64_t last_serial_ = 0;
    int64_t slack_[kWindow] = {};
    int count_ = 0;
};

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

// Whether PhaseLock steers the cap (`lock`: native_phase_lock): a fixed rate
// (display, or a number of Hz) at `cap_hz` within 0.5% of the display's
inline bool PhaseLockSteers(bool lock, FrameCapMode mode, double cap_hz, double display_hz) {
    if (!lock || (mode != FrameCapMode::kDisplay && mode != FrameCapMode::kFixed)) return false;
    return display_hz > 0 && std::abs(cap_hz - display_hz) <= 0.005 * display_hz;
}

// Whether the native renderer holds each frame back to publish it on a steady
// delay (native_present_pacing, present_model.h's PublishPacer), for the game
// at `game_hz` on a display at `display_hz`. The hold keeps a fast frame from
// reaching a paint with the slow one before it, which only happens with about
// one frame a refresh: at 1.5 times the refresh or more, each paint shows the
// newest of two or more frames whatever the hold, which only makes it later.
// frame_cap auto declares a variable refresh display, which shows each frame
// as it comes: there the hold only adds lag. So does `phase_locked`
// (PhaseLockSteers) on a display of kLockedHoldlessHz or more: the lock already
// has each frame published a set time before its paint, and a frame that
// misses it came late from the game, which holding the ones around it can't
// make earlier (at 120 Hz the hold cost 1.7 ms of latency and repeated as many
// frames). Below that the lock keeps the hold: at 60 Hz on an integrated GPU,
// whose frames vary by most of a refresh, the window showed 97% of frames
// distinct with it and 83% without. A rate not known (0) keeps it.
inline constexpr double kLockedHoldlessHz = 100;

// The phase lock's slack in percent of a refresh for native_phase_lock_slack
// `setting` (0: by the display's rate) on a display at `display_hz`. At 120 Hz
// 50% (4.2 ms) left post-processed frames, which reach the window about 1.9 ms
// after the median, missing their paint about once a second; 65% (5.4 ms)
// repeated 83% fewer frames for 1.2 ms more lag. Below kLockedHoldlessHz, where
// the hold still paces frames and 50% is already 8.3 ms at 60 Hz, it stays 50.
inline int PhaseLockSlackPercent(int setting, double display_hz) {
    if (setting > 0) return std::clamp(setting, 10, 90);
    return display_hz >= kLockedHoldlessHz ? 65 : 50;
}

inline bool PacePublishing(FrameCapMode mode, double game_hz, double display_hz,
                           bool phase_locked = false) {
    if (mode == FrameCapMode::kAuto) return false;
    if (phase_locked && display_hz >= kLockedHoldlessHz) return false;
    return game_hz <= 0 || display_hz <= 0 || game_hz < 1.5 * display_hz;
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

// vr_game_rate: auto, off, or a number of Hz (kept to kMinCapHz..kMaxCapHz)
inline bool ValidVrGameRate(std::string_view v) {
    if (v == "auto" || v == "off") return true;
    const auto fixed = ParseFrameCap(v);
    return fixed && fixed->mode == FrameCapMode::kFixed;
}

// The cap while a VR session runs whose headset refreshes every `refresh_ns`
// (0 unknown), for vr_game_rate `setting`: auto, the headset's own period
// exactly, so each of its refreshes can have a new game frame; a number, that
// rate; off (or auto with the refresh unknown) none, frame_cap's as without VR
inline FrameCap VrGameCap(std::string_view setting, int64_t refresh_ns) {
    if (setting == "auto") {
        if (refresh_ns <= 0) return {};
        const double hz = 1e9 / double(refresh_ns);
        if (hz < kMinCapHz || hz > kMaxCapHz) return {};
        return FrameCap{FrameCapMode::kVr, hz, refresh_ns};
    }
    const auto fixed = ParseFrameCap(setting);
    if (!fixed || fixed->mode != FrameCapMode::kFixed) return {};
    return FrameCap{FrameCapMode::kVr, fixed->hz, static_cast<int64_t>(std::llround(1e9 / fixed->hz))};
}

// The cap's running half, in graphics.cpp.

// VR's cap (VrGameCap), set by the native renderer's worker as a session
// runs and the headset's refresh is known, cleared (a FrameCap{}) as it
// stops: while set it paces the game instead of frame_cap, turning the
// SDK's vsync off as frame_cap's would. Any thread; logged as it changes.
void SetVrCap(const FrameCap& cap);

// Starts the cap at startup, once the GPU plugin is loaded and before the
// runtime starts its vblank thread (Band3App::StartFrameCap): reads the display under `native_window`, turns the SDK's vsync
// off and sets the guest's refresh rate to the cap's when the cap is on, then
// re-reads the display every couple of seconds and frame_cap whenever it
// changes. Changing vsync later goes through `post_to_ui`, which runs a task
// on the UI thread.
void StartFrameCap(void* native_window, std::function<void(std::function<void()>)> post_to_ui);
void StopFrameCap();

// The player's vsync while the cap holds the SDK's off for the session: what a
// save should keep (the in-game settings' Save); nullopt while it isn't held.
std::optional<bool> VsyncBeforeCap();

// Called by the game thread at the end of each Present (scene_capture.cpp):
// waits for the frame's beat while the cap is on
void PaceFrame();

// PhaseLock's correction, moved into the beat a little each frame by
// PaceFrame (kNudgePerFrameNs at most) so no frame's interval jumps
inline constexpr int64_t kNudgePerFrameNs = 250'000;
void NudgeBeat(int64_t ns);

// The rate the game runs at: the cap's (or VR's) while it's on, else refresh_rate
// (video_mode_refresh_rate, 0 for the console's 60)
double GameHz();

// The refresh rate of the display the window was last seen on (read again
// every couple of seconds while the cap runs, whether it's on or not); 0
// before the cap started or while it can't be told
double DisplayHz();

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
