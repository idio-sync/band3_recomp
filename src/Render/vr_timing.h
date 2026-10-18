#pragma once

// VR's frame timing (the `vr` setting, xr_present.h), to find where a late
// frame's time went: a record of each of the runtime's frames from
// xrWaitFrame's return to xrEndFrame's (and the window's picture after it),
// what it decides of the frame (Classify), a ring of the last kRingFrames
// for the harness's `vr_frames` dump (CSV), and percentiles of each part for
// late and on-time frames (native_view stats' `xr` block's `timing`). No
// OpenXR or Direct3D here: xr_present.cpp and native_view.cpp's VrFrame fill
// the record, so this is the same on every platform and unit-tested.
//
// Times in a record are milliseconds from `wake`, xrWaitFrame's return, or
// kNone where that step didn't happen; durations are milliseconds.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace band3::render::vr {

inline constexpr double kNone = std::numeric_limits<double>::quiet_NaN();
inline bool Has(double v) { return !std::isnan(v); }

// a frame whose CPU time from wake to xrEndFrame, or whose step from the
// frame before's display time, is this many periods or more is logged (and,
// for the step, a missed slot)
inline constexpr double kSlowPeriods = 1.5;
// the ring: about 68 s at 120 Hz
inline constexpr size_t kRingFrames = 8192;

struct FrameTiming {
    uint64_t index = 0;  // the session's frames, from 1
    // xr_present.cpp's WaitFrame: xrWaitFrame's return (steady clock ns),
    // its predictedDisplayTime (XrTime ns) and predictedDisplayPeriod
    int64_t wake_ns = 0, display_time_ns = 0, period_ns = 0;
    // the headset's refresh (RefreshPeriod: the shortest period of late);
    // `period_ns` is twice it or more while the runtime throttles band3 to
    // every other refresh (`throttled`). 0: not known, period_ns stands in
    int64_t refresh_ns = 0;
    bool throttled = false;
    bool should_render = false;
    // since the frame before: its display time and its wake (kNone first)
    double display_step_ms = kNone, wake_step_ms = kNone;
    double wait_ms = 0;  // in xrWaitFrame
    // predictedDisplayTime less the wake, where the runtime converts XrTime
    // (XR_KHR_win32_convert_performance_counter_time)
    double display_in_ms = kNone;
    double begun_ms = kNone;    // xrBeginFrame returned
    double located_ms = kNone;  // the views located: VrFrame takes the frame from here

    // native_view.cpp's VrFrame: the capture the eyes draw
    uint64_t capture = 0;           // FrameCapture::frame, 0 none yet
    double capture_age_ms = kNone;  // from the game's Present to the worker taking it
    double game_ms = kNone;         // the game's frame time ending in it (FrameCapture::cost)
    bool repeat = false;            // the capture the frame before drew
    uint32_t captures_skipped = 0;  // game frames since that one that no eye drew
    // the game frames newer than the eyes' capture as Present began
    uint32_t newer = 0;
    // each eye: RenderFrameToOutput returned, its CPU to submitting
    // (GpuStats::ms), its GPU (native_gpu_timestamps; 0 off) and RePose's copy
    double eye_submitted_ms[2] = {kNone, kNone};
    double eye_cpu_ms[2] = {0, 0}, eye_gpu_ms[2] = {0, 0}, repose_ms[2] = {0, 0};
    // the HUD panel's picture, when one was drawn this frame
    bool hud = false;
    double hud_submitted_ms = kNone, hud_cpu_ms = 0, hud_gpu_ms = 0;
    // waiting for the GPU to finish before Present (OutputDone's loop): the
    // eyes, the panel; and when all were finished (after xrEndFrame with
    // queue_wait)
    double eyes_wait_ms = 0, hud_wait_ms = 0;
    double gpu_done_ms = kNone;
    // the SDK's queue waited on SDL_gpu's fences instead (Present's waits),
    // and after xrEndFrame the CPU waited this long for its copies to finish
    bool queue_wait = false;
    double done_wait_ms = 0;

    // xr_present.cpp's Present
    double present_ms = kNone;  // Present began
    double fence_wait_ms = 0;   // for the frame in flight that had this allocator
    double acquire_ms = 0;      // the eyes' images: acquired and waited for
    double hud_acquire_ms = 0;  // the panel's
    double copy_ms = 0;         // recording and submitting the copies into them
    double release_ms = 0;
    double end_frame_ms = 0;    // in xrEndFrame
    double ended_ms = kNone;    // xrEndFrame returned
    bool layer = false;         // ended with the eyes' layer

    // after xrEndFrame: the left eye published as the window's picture, and
    // the time that took (reading back for a screenshot or F9 included);
    // the next frame's xrWaitFrame starts after it
    bool window = false;
    double window_ms = 0;

    // Classify's
    // `late` as the xr block has always counted it: located to xrEndFrame's
    // return (the CPU's part of the frame, xrWaitFrame's wait left out) over
    // the display period
    bool late = false;
    bool over_period = false;  // wake to xrEndFrame's return over the period
    // the runtime's display time moved on kSlowPeriods periods or more since
    // the frame before: it showed a refresh (or more) without a new frame
    bool missed = false;
    uint32_t slots_missed = 0;
    // xrWaitFrame returned kSlowPeriods periods or more after the frame before's
    bool late_wake = false;
    // logged: ended or stepped kSlowPeriods periods or more
    bool slow = false;
};

// The headset's refresh period: the shortest predictedDisplayPeriod of the
// last kWindow frames. SteamVR predicts twice the refresh (or more) while it
// throttles a late app to every other refresh, so a frame's own period isn't
// the refresh's: judged by it, those frames' steps of two refreshes looked on
// time, and only the step back to one refresh counted as missed (2026-10-18,
// the Steam Frame: 210 missed counted of 310 refreshes missed a minute).
class RefreshPeriod {
 public:
    static constexpr size_t kWindow = 256;
    // the period now, with this frame's
    int64_t Add(int64_t period_ns) {
        if (period_ns > 0) {
            if (seen_.size() < kWindow) seen_.push_back(period_ns);
            else seen_[next_++ % kWindow] = period_ns;
        }
        return Ns();
    }
    int64_t Ns() const {
        return seen_.empty() ? 0 : *std::min_element(seen_.begin(), seen_.end());
    }

 private:
    std::vector<int64_t> seen_;
    size_t next_ = 0;
};

// the refresh's period: refresh_ns, else the frame's own
inline double PeriodMs(const FrameTiming& t) {
    return double(t.refresh_ns > 0 ? t.refresh_ns : t.period_ns) / 1e6;
}
// the measure `late` goes by
inline double LocatedToEndMs(const FrameTiming& t) {
    return Has(t.ended_ms) && Has(t.located_ms) ? t.ended_ms - t.located_ms : kNone;
}

// Fills `t`'s steps from `prev` (null: the session's first frame), then what
// they and its times say: late, over the period, a missed slot, a late wake
inline void Classify(FrameTiming& t, const FrameTiming* prev) {
    if (prev && prev->index) {
        t.wake_step_ms = double(t.wake_ns - prev->wake_ns) / 1e6;
        t.display_step_ms = double(t.display_time_ns - prev->display_time_ns) / 1e6;
    }
    const double period = PeriodMs(t);
    t.late = t.over_period = t.missed = t.late_wake = t.slow = false;
    t.slots_missed = 0;
    if (period <= 0) return;
    t.throttled = double(t.period_ns) / 1e6 >= kSlowPeriods * period;
    const double located_to_end = LocatedToEndMs(t);
    t.late = Has(located_to_end) && located_to_end > period;
    t.over_period = Has(t.ended_ms) && t.ended_ms > period;
    if (Has(t.display_step_ms) && t.display_step_ms >= kSlowPeriods * period) {
        t.missed = true;
        t.slots_missed = uint32_t(std::lround(t.display_step_ms / period) - 1);
    }
    t.late_wake = Has(t.wake_step_ms) && t.wake_step_ms >= kSlowPeriods * period;
    t.slow = t.missed || (Has(t.ended_ms) && t.ended_ms >= kSlowPeriods * period);
}

// The last `capacity` records, oldest first by index
class FrameRing {
 public:
    explicit FrameRing(size_t capacity = kRingFrames) : capacity_(capacity) {}
    void Push(const FrameTiming& t) {
        if (frames_.size() < capacity_) {
            frames_.push_back(t);
        } else {
            frames_[next_] = t;
            next_ = (next_ + 1) % capacity_;
        }
    }
    // those whose index is past `after`, oldest first
    std::vector<FrameTiming> Since(uint64_t after) const {
        std::vector<FrameTiming> out;
        out.reserve(frames_.size());
        for (size_t i = 0; i < frames_.size(); i++) {
            const FrameTiming& t = frames_[(next_ + i) % frames_.size()];
            if (t.index > after) out.push_back(t);
        }
        return out;
    }
    size_t Size() const { return frames_.size(); }

 private:
    size_t capacity_;
    size_t next_ = 0;  // the oldest, once full
    std::vector<FrameTiming> frames_;
};

// nearest rank, of those that aren't kNone
struct Spread {
    uint32_t n = 0;
    double p50 = 0, p95 = 0, p99 = 0, max = 0;
};
inline Spread SpreadOf(std::vector<double> v) {
    v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return !Has(x); }), v.end());
    Spread s;
    s.n = uint32_t(v.size());
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    auto rank = [&](double p) {
        const size_t r = std::clamp<size_t>(size_t(std::ceil(p * double(v.size()))), 1, v.size());
        return v[r - 1];
    };
    s.p50 = rank(0.50);
    s.p95 = rank(0.95);
    s.p99 = rank(0.99);
    s.max = v.back();
    return s;
}

// The parts summarised, by name: a frame's value, or kNone where it has none
// (a frame without a HUD picture has no panel times)
struct Part {
    const char* name;
    double (*of)(const FrameTiming&);
};
inline const std::vector<Part>& Parts() {
    static const std::vector<Part> parts = {
        {"wake_step_ms", [](const FrameTiming& t) { return t.wake_step_ms; }},
        {"display_step_ms", [](const FrameTiming& t) { return t.display_step_ms; }},
        {"wait_ms", [](const FrameTiming& t) { return t.wait_ms; }},
        {"display_in_ms", [](const FrameTiming& t) { return t.display_in_ms; }},
        {"begun_ms", [](const FrameTiming& t) { return t.begun_ms; }},
        {"located_ms", [](const FrameTiming& t) { return t.located_ms; }},
        {"capture_age_ms", [](const FrameTiming& t) { return t.capture_age_ms; }},
        {"game_ms", [](const FrameTiming& t) { return t.game_ms; }},
        {"repose_ms", [](const FrameTiming& t) {
             return t.capture ? t.repose_ms[0] + t.repose_ms[1] : kNone;
         }},
        {"eye_cpu_ms", [](const FrameTiming& t) {
             return t.capture ? t.eye_cpu_ms[0] + t.eye_cpu_ms[1] : kNone;
         }},
        {"eye_gpu_ms", [](const FrameTiming& t) {
             const double gpu = t.eye_gpu_ms[0] + t.eye_gpu_ms[1];
             return gpu > 0 ? gpu : kNone;
         }},
        {"eyes_submitted_ms", [](const FrameTiming& t) { return t.eye_submitted_ms[1]; }},
        {"hud_cpu_ms", [](const FrameTiming& t) { return t.hud ? t.hud_cpu_ms : kNone; }},
        {"hud_gpu_ms",
         [](const FrameTiming& t) { return t.hud_gpu_ms > 0 ? t.hud_gpu_ms : kNone; }},
        {"hud_submitted_ms", [](const FrameTiming& t) { return t.hud_submitted_ms; }},
        {"eyes_wait_ms", [](const FrameTiming& t) { return t.capture ? t.eyes_wait_ms : kNone; }},
        {"hud_wait_ms", [](const FrameTiming& t) { return t.hud ? t.hud_wait_ms : kNone; }},
        {"gpu_done_ms", [](const FrameTiming& t) { return t.gpu_done_ms; }},
        {"done_wait_ms", [](const FrameTiming& t) { return t.queue_wait ? t.done_wait_ms : kNone; }},
        {"present_ms", [](const FrameTiming& t) { return t.present_ms; }},
        {"fence_wait_ms", [](const FrameTiming& t) { return t.fence_wait_ms; }},
        {"acquire_ms", [](const FrameTiming& t) { return t.acquire_ms; }},
        {"hud_acquire_ms", [](const FrameTiming& t) { return t.hud ? t.hud_acquire_ms : kNone; }},
        {"copy_ms", [](const FrameTiming& t) { return t.copy_ms; }},
        {"release_ms", [](const FrameTiming& t) { return t.release_ms; }},
        {"end_frame_ms", [](const FrameTiming& t) { return t.end_frame_ms; }},
        {"ended_ms", [](const FrameTiming& t) { return t.ended_ms; }},
        {"located_to_end_ms", [](const FrameTiming& t) { return LocatedToEndMs(t); }},
        {"window_ms", [](const FrameTiming& t) { return t.window ? t.window_ms : kNone; }},
    };
    return parts;
}

// One group's counts and each part's Spread, as JSON:
// {"frames":n,"repeats":..,...,"parts":{"wait_ms":{"n":..,"p50":..},...}}
inline std::string GroupJson(const std::vector<const FrameTiming*>& frames) {
    uint64_t repeats = 0, newer = 0, missed = 0, slots = 0, over = 0, late_wakes = 0, hud = 0,
             window = 0, no_layer = 0, throttled = 0, queue_wait = 0;
    for (const FrameTiming* t : frames) {
        throttled += t->throttled;
        queue_wait += t->queue_wait;
        repeats += t->repeat;
        newer += t->newer > 0;
        missed += t->missed;
        slots += t->slots_missed;
        over += t->over_period;
        late_wakes += t->late_wake;
        hud += t->hud;
        window += t->window;
        no_layer += !t->layer;
    }
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "{\"frames\":%zu,\"repeats\":%llu,\"behind_newer\":%llu,\"missed\":%llu,"
                  "\"slots_missed\":%llu,\"over_period\":%llu,\"late_wakes\":%llu,\"hud\":%llu,"
                  "\"window\":%llu,\"no_layer\":%llu,\"throttled\":%llu,\"queue_wait\":%llu,"
                  "\"parts\":{",
                  frames.size(), (unsigned long long)repeats, (unsigned long long)newer,
                  (unsigned long long)missed, (unsigned long long)slots, (unsigned long long)over,
                  (unsigned long long)late_wakes, (unsigned long long)hud,
                  (unsigned long long)window, (unsigned long long)no_layer,
                  (unsigned long long)throttled, (unsigned long long)queue_wait);
    std::string out = buf;
    bool first = true;
    std::vector<double> values;
    for (const Part& p : Parts()) {
        values.clear();
        for (const FrameTiming* t : frames) values.push_back(p.of(*t));
        const Spread s = SpreadOf(values);
        if (!s.n) continue;
        std::snprintf(buf, sizeof(buf),
                      "%s\"%s\":{\"n\":%u,\"p50\":%.3f,\"p95\":%.3f,\"p99\":%.3f,\"max\":%.3f}",
                      first ? "" : ",", p.name, s.n, s.p50, s.p95, s.p99, s.max);
        out += buf;
        first = false;
    }
    return out + "}}";
}

// native_view stats' `xr` block's `timing`: the frames recorded since it
// started measuring, those `late` and the rest
inline std::string TimingJson(const std::vector<FrameTiming>& frames) {
    std::vector<const FrameTiming*> late, on_time;
    for (const FrameTiming& t : frames) (t.late ? late : on_time).push_back(&t);
    return "{\"frames\":" + std::to_string(frames.size()) + ",\"late\":" + GroupJson(late) +
           ",\"on_time\":" + GroupJson(on_time) + "}";
}

// `vr_frames`' CSV: one row a frame, a column a field; kNone is empty
inline std::string CsvHeader() {
    return "index,wake_s,period_ms,should_render,display_step_ms,wake_step_ms,wait_ms,"
           "display_in_ms,begun_ms,located_ms,capture,capture_age_ms,game_ms,repeat,"
           "captures_skipped,newer,eye0_submitted_ms,eye1_submitted_ms,eye0_cpu_ms,eye1_cpu_ms,"
           "eye0_gpu_ms,eye1_gpu_ms,repose0_ms,repose1_ms,hud,hud_submitted_ms,hud_cpu_ms,"
           "hud_gpu_ms,eyes_wait_ms,hud_wait_ms,gpu_done_ms,present_ms,fence_wait_ms,acquire_ms,"
           "hud_acquire_ms,copy_ms,release_ms,end_frame_ms,ended_ms,located_to_end_ms,layer,"
           "window,window_ms,late,over_period,missed,slots_missed,late_wake,refresh_ms,throttled,"
           "queue_wait,done_wait_ms\n";
}

inline std::string CsvRow(const FrameTiming& t) {
    std::string out;
    char buf[64];
    auto num = [&](double v) {
        if (Has(v)) {
            std::snprintf(buf, sizeof(buf), "%.3f", v);
            out += buf;
        }
        out += ',';
    };
    auto whole = [&](uint64_t v) {
        out += std::to_string(v);
        out += ',';
    };
    whole(t.index);
    std::snprintf(buf, sizeof(buf), "%.6f,", double(t.wake_ns) / 1e9);
    out += buf;
    num(double(t.period_ns) / 1e6);  // the runtime's predicted period
    whole(t.should_render);
    num(t.display_step_ms);
    num(t.wake_step_ms);
    num(t.wait_ms);
    num(t.display_in_ms);
    num(t.begun_ms);
    num(t.located_ms);
    whole(t.capture);
    num(t.capture_age_ms);
    num(t.game_ms);
    whole(t.repeat);
    whole(t.captures_skipped);
    whole(t.newer);
    for (double v : t.eye_submitted_ms) num(v);
    for (double v : t.eye_cpu_ms) num(v);
    for (double v : t.eye_gpu_ms) num(v);
    for (double v : t.repose_ms) num(v);
    whole(t.hud);
    num(t.hud_submitted_ms);
    num(t.hud_cpu_ms);
    num(t.hud_gpu_ms);
    num(t.eyes_wait_ms);
    num(t.hud_wait_ms);
    num(t.gpu_done_ms);
    num(t.present_ms);
    num(t.fence_wait_ms);
    num(t.acquire_ms);
    num(t.hud_acquire_ms);
    num(t.copy_ms);
    num(t.release_ms);
    num(t.end_frame_ms);
    num(t.ended_ms);
    num(LocatedToEndMs(t));
    whole(t.layer);
    whole(t.window);
    num(t.window_ms);
    whole(t.late);
    whole(t.over_period);
    whole(t.missed);
    whole(t.slots_missed);
    whole(t.late_wake);
    num(PeriodMs(t));
    whole(t.throttled);
    whole(t.queue_wait);
    std::snprintf(buf, sizeof(buf), "%.3f", t.done_wait_ms);
    out += buf;
    out += '\n';
    return out;
}

// A slow frame's breakdown for the log, one line
inline std::string Breakdown(const FrameTiming& t) {
    std::string out;
    char buf[256];
    auto add = [&](const char* fmt, double v) {
        if (!Has(v)) return;
        std::snprintf(buf, sizeof(buf), fmt, v);
        out += buf;
    };
    std::snprintf(buf, sizeof(buf), "frame %llu: ended at %.2f ms of a %.2f ms period",
                  (unsigned long long)t.index, Has(t.ended_ms) ? t.ended_ms : -1.0, PeriodMs(t));
    out += buf;
    add(", display +%.2f ms", t.display_step_ms);
    if (t.missed) out += " (" + std::to_string(t.slots_missed) + " missed)";
    add(", woke +%.2f ms", t.wake_step_ms);
    std::snprintf(buf, sizeof(buf), "; waited %.2f", t.wait_ms);
    out += buf;
    add(", display in %.2f", t.display_in_ms);
    add(", begun %.2f", t.begun_ms);
    add(", located %.2f", t.located_ms);
    if (t.capture) {
        std::snprintf(buf, sizeof(buf), "; capture %llu%s", (unsigned long long)t.capture,
                      t.repeat ? " (repeat)" : "");
        out += buf;
        add(" age %.2f", t.capture_age_ms);
        add(" game %.2f", t.game_ms);
        std::snprintf(buf, sizeof(buf),
                      ", eyes submitted %.2f/%.2f (cpu %.2f/%.2f, repose %.2f/%.2f",
                      t.eye_submitted_ms[0], t.eye_submitted_ms[1], t.eye_cpu_ms[0],
                      t.eye_cpu_ms[1], t.repose_ms[0], t.repose_ms[1]);
        out += buf;
        if (t.eye_gpu_ms[0] + t.eye_gpu_ms[1] > 0) {
            std::snprintf(buf, sizeof(buf), ", gpu %.2f/%.2f", t.eye_gpu_ms[0], t.eye_gpu_ms[1]);
            out += buf;
        }
        out += ')';
    }
    if (t.hud) {
        std::snprintf(buf, sizeof(buf), ", hud cpu %.2f", t.hud_cpu_ms);
        out += buf;
        add(" submitted %.2f", t.hud_submitted_ms);
    }
    if (t.queue_wait) {
        std::snprintf(buf, sizeof(buf), ", gpu waited on the queue (%.2f after xrEndFrame)",
                      t.done_wait_ms);
    } else {
        std::snprintf(buf, sizeof(buf), ", gpu wait %.2f", t.eyes_wait_ms + t.hud_wait_ms);
    }
    out += buf;
    add(" done %.2f", t.gpu_done_ms);
    add("; present at %.2f", t.present_ms);
    std::snprintf(buf, sizeof(buf),
                  ": fence %.2f, acquire %.2f, hud acquire %.2f, copy %.2f, release %.2f, "
                  "xrEndFrame %.2f",
                  t.fence_wait_ms, t.acquire_ms, t.hud_acquire_ms, t.copy_ms, t.release_ms,
                  t.end_frame_ms);
    out += buf;
    if (t.newer) out += "; " + std::to_string(t.newer) + " newer game frames";
    if (t.window) {
        std::snprintf(buf, sizeof(buf), "; window %.2f", t.window_ms);
        out += buf;
    }
    return out;
}

}  // namespace band3::render::vr
