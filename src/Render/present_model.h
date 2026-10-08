#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// Experimental: the native renderer's presentation bookkeeping
// (native_view.cpp's NativePresentDrawer), kept apart for unit tests:
// letterboxing, output slot ownership, publish pacing, and the harness's
// present_stats.

namespace band3::render {

// in the render target's pixels
struct ImageRect {
    uint32_t x = 0, y = 0, w = 0, h = 0;
};

// The largest centred ar_x:ar_y rectangle in the target, as the SDK's
// presenter letterboxes; the whole target (stretched) with `letterbox` off
// (the SDK's present_letterbox).
inline ImageRect LetterboxRect(uint32_t target_w, uint32_t target_h, bool letterbox,
                               uint32_t ar_x = 16, uint32_t ar_y = 9) {
    ImageRect r{0, 0, target_w, target_h};
    if (!letterbox || !target_w || !target_h || !ar_x || !ar_y) return r;
    // 64 bits: a caller's large aspect could overflow 32
    const uint64_t wide = uint64_t(target_w) * ar_y, tall = uint64_t(target_h) * ar_x;
    if (wide > tall) {
        r.w = uint32_t((tall + ar_y / 2) / ar_y);
        if (r.w == 0) r.w = 1;
        r.x = (target_w - r.w) / 2;
    } else if (wide < tall) {
        r.h = uint32_t((wide + ar_x / 2) / ar_x);
        if (r.h == 0) r.h = 1;
        r.y = (target_h - r.h) / 2;
    }
    return r;
}

// Ownership of the presenter's output textures. The worker publishes a drawn
// slot as newest; each paint samples the newest on the SDK's queue, noting
// its submission index, and reports the newest completed submission. The
// worker draws only into a slot that isn't newest and that no unfinished
// paint sampled. With three, one is always free while paints finish. Not
// thread-safe: native_view.cpp holds its mutex around every call.
//
// A submitted slot stays taken until Finished and isn't published before:
// paints sample on another queue that nothing but the worker's wait orders
// after SDL's.
//
// Each presenting stretch (F8 toggles it) shows only frames drawn for it
// (Publish's `generation`) from captures published after it began or the
// window was restored (Fresh): the capture newest at the switch is seconds
// old (capture stops with the emulated GPU), as is a frame still being drawn
// when presenting stopped.
class PresentSlots {
 public:
    static constexpr int kCount = 3;

    // steady-clock nanoseconds
    void Start(int64_t now_ns) {
        newest_ = -1;
        generation_++;
        since_ns_ = now_ns;
    }
    // presenting stopped; paints' indices are kept (the GPU may still be
    // reading a slot)
    void Forget() {
        newest_ = -1;
        generation_++;
    }
    // Restored after a minimized pause: the stretch goes on, its newest frame
    // repeated until one is drawn from a capture published from now (Fresh).
    void Resume(int64_t now_ns) { since_ns_ = now_ns; }
    // noted by the worker before it draws
    uint64_t Generation() const { return generation_; }
    bool Fresh(int64_t published_ns) const { return published_ns > since_ns_; }

    // -1 if none is free
    int Acquire() {
        for (int i = 0; i < kCount; i++) {
            Slot& s = slots_[i];
            if (i == newest_ || s.drawing || s.used > completed_) continue;
            s.drawing = true;
            return i;
        }
        return -1;
    }
    void Submitted(int slot) {
        if (Valid(slot) && slots_[slot].drawing) slots_[slot].in_flight = true;
    }
    void Finished(int slot) {
        if (Valid(slot)) slots_[slot].in_flight = false;
    }
    // Publish refuses an unfinished slot, and the worker mustn't Abandon it
    // then (the next frame could take it while the GPU still writes it)
    bool Unfinished(int slot) const { return Valid(slot) && slots_[slot].in_flight; }
    int InFlight() const {
        int n = 0;
        for (const Slot& s : slots_) n += s.in_flight ? 1 : 0;
        return n;
    }
    // Makes `slot` the newest; false, and freed, if `generation`'s stretch is
    // over; false and unchanged if unfinished.
    bool Publish(int slot, uint64_t generation) {
        if (!Valid(slot) || slots_[slot].in_flight) return false;
        slots_[slot].drawing = false;
        if (generation != generation_) return false;
        newest_ = slot;
        serial_++;
        return true;
    }
    bool Publish(int slot) { return Publish(slot, generation_); }
    // GPU failed or worker stopped. Safe even if the GPU is still drawing it:
    // nothing samples it unpublished, and the next draw into it follows on
    // SDL's queue.
    void Abandon(int slot) {
        if (!Valid(slot)) return;
        slots_[slot].drawing = false;
        slots_[slot].in_flight = false;
    }

    // the drawer: paint `submission` samples `slot`
    void Shown(int slot, uint64_t submission) {
        if (!Valid(slot)) return;
        Slot& s = slots_[slot];
        if (submission > s.used) s.used = submission;
    }
    void Completed(uint64_t completed) {
        if (completed > completed_) completed_ = completed;
    }
    // for settling all slots at once when paints stop (minimized: no
    // completed index either)
    uint64_t LastUsed() const {
        uint64_t last = 0;
        for (const Slot& s : slots_)
            if (s.used > last) last = s.used;
        return last;
    }

    int Newest() const { return newest_; }
    // published frames so far, to tell a new one
    uint64_t Serial() const { return serial_; }
    uint64_t CompletedIndex() const { return completed_; }

 private:
    struct Slot {
        bool drawing = false;
        bool in_flight = false;  // submitted, not finished (drawing too)
        uint64_t used = 0;       // last paint that sampled it
    };
    static bool Valid(int slot) { return slot >= 0 && slot < kCount; }

    Slot slots_[kCount];
    int newest_ = -1;
    uint64_t serial_ = 0;
    uint64_t completed_ = 0;
    uint64_t generation_ = 0;
    int64_t since_ns_ = 0;
};

// When the worker's frames reach the window. Under even/odd rendering post
// frames take several times longer than world frames, so publishing on
// completion lands frames ~8 and ~25 ms apart and a paint can find two new
// ones, dropping the first. So each frame is published a steady delay after
// the game presented it: the second slowest of recent frames (one hitch
// doesn't hold back the rest), capped at the game's frame interval less 1 ms.
// Slower frames publish when drawn. Worker thread only.
class PublishPacer {
 public:
    static constexpr int kHistory = 8;
    static constexpr int64_t kMs = 1000000;  // a millisecond in nanoseconds

    // When to publish capture `frame`, presented at `presented_ns` and drawn
    // by `now_ns` (steady-clock nanoseconds); never before `now_ns`.
    int64_t Due(uint64_t frame, int64_t presented_ns, int64_t now_ns) {
        if (last_frame_ && frame > last_frame_ && frame - last_frame_ <= 4 &&
            presented_ns > last_presented_ns_) {
            const int64_t step = (presented_ns - last_presented_ns_) / int64_t(frame - last_frame_);
            if (step >= 2 * kMs && step <= 100 * kMs) interval_ns_ += (step - interval_ns_) / 8;
        }
        if (frame > last_frame_) {
            last_frame_ = frame;
            last_presented_ns_ = presented_ns;
        }
        const int64_t own = std::max<int64_t>(0, now_ns - presented_ns);
        own_ns_[next_++ % kHistory] = own;
        if (count_ < kHistory) count_++;
        int64_t first = 0, second = 0;
        for (int i = 0; i < count_; i++) {
            const int64_t v = own_ns_[i];
            if (v > first) {
                second = first;
                first = v;
            } else if (v > second) {
                second = v;
            }
        }
        int64_t delay = count_ >= 2 ? second : own;
        delay = std::min(delay, std::max<int64_t>(0, interval_ns_ - kMs));
        return std::max(now_ns, presented_ns + delay);
    }
    int64_t IntervalNs() const { return interval_ns_; }
    void Reset() { *this = PublishPacer{}; }

 private:
    int64_t own_ns_[kHistory] = {};
    int count_ = 0;
    uint64_t next_ = 0;
    uint64_t last_frame_ = 0;
    int64_t last_presented_ns_ = 0;
    int64_t interval_ns_ = 16666667;  // 60 Hz until measured
};

// Whether native_world_ahead draws the world ahead now. It helps while the
// GPU keeps up and while it's far behind, but in between it feeds itself: a
// slow post frame skips the next world capture, wasting the world drawn
// ahead, so the post frame after draws the whole world and is slow again.
// That depends on the GPU's (changing) load, so the worker measures captures
// skipped per capture over kWindowNs windows, a running mean each for on and
// off, and picks the lower (on within kMargin: it shows frames sooner).
// Every kProbeEvery-th window tries the other, replacing its mean, so load
// changes are seen. On until both are measured. Worker thread only.
class AheadChooser {
 public:
    static constexpr int64_t kWindowNs = 2000000000;
    static constexpr uint32_t kProbeEvery = 8;
    static constexpr double kMargin = 0.01;

    bool On() const { return on_; }
    // `skipped` captures since the last one drawn; steady-clock nanoseconds
    void Drawn(int64_t now_ns, uint64_t skipped) {
        if (!started_) {
            started_ = true;
            start_ns_ = now_ns;
        }
        drawn_++;
        skipped_ += skipped;
        if (now_ns - start_ns_ < kWindowNs) return;
        const double rate = double(skipped_) / double(drawn_ + skipped_);
        Mean& m = on_ ? on_mean_ : off_mean_;
        m.rate = m.seen && !probing_ ? (m.rate + rate) / 2 : rate;
        m.seen = true;
        start_ns_ = now_ns;
        drawn_ = skipped_ = 0;
        windows_++;
        bool best = true;
        if (on_mean_.seen && off_mean_.seen) best = on_mean_.rate <= off_mean_.rate + kMargin;
        else if (on_mean_.seen) best = false;  // off measured next
        probing_ = windows_ % kProbeEvery == 0;
        on_ = probing_ ? !best : best;
    }
    // setting turned on again, or presenting started over
    void Reset() { *this = AheadChooser{}; }

 private:
    struct Mean {
        double rate = 0;
        bool seen = false;
    };
    bool on_ = true;
    bool probing_ = false;  // this window tries the one that skips more
    bool started_ = false;
    int64_t start_ns_ = 0;
    uint64_t drawn_ = 0, skipped_ = 0;
    uint64_t windows_ = 0;
    Mean on_mean_, off_mean_;
};

// Whether a published frame should ask the window to paint
// (NativePresentDrawer::Start); no point while it can't be seen.
inline bool PaintWanted(bool minimized, bool visible, uint32_t client_w, uint32_t client_h) {
    return !minimized && visible && client_w > 0 && client_h > 0;
}

// The worker draws nothing while the window is minimized
// (NativePresentMinimized); the presenter paints a minimized window anyway,
// so holding back paints alone saves nothing. This keeps the pause's numbers
// for `native_view stats`: time paused and captures published meanwhile.
// Times in steady-clock nanoseconds, frames are FrameCapture::frame. Not
// thread-safe: native_view.cpp holds its mutex around every call.
class DrawPause {
 public:
    // false if already paused
    bool Pause(int64_t now_ns, uint64_t frame) {
        if (paused_) return false;
        paused_ = true;
        since_ns_ = began_ns_ = now_ns;
        since_frame_ = began_frame_ = frame;
        return true;
    }
    // false if not paused
    bool Resume(int64_t now_ns, uint64_t frame) {
        if (!paused_) return false;
        paused_ = false;
        ns_ += Since(now_ns, since_ns_);
        captures_ += Since(frame, since_frame_);
        last_ns_ = Since(now_ns, began_ns_);
        last_captures_ = Since(frame, began_frame_);
        return true;
    }
    bool Paused() const { return paused_; }
    // an ongoing pause counts from now
    void Restart(int64_t now_ns, uint64_t frame) {
        ns_ = 0;
        captures_ = 0;
        since_ns_ = now_ns;
        since_frame_ = frame;
    }
    // since Restart, an ongoing pause counted up to `now_ns` / `frame`
    double Ms(int64_t now_ns) const {
        return double(ns_ + (paused_ ? Since(now_ns, since_ns_) : 0)) / 1e6;
    }
    uint64_t Captures(uint64_t frame) const {
        return captures_ + (paused_ ? Since(frame, since_frame_) : 0);
    }
    // the last completed pause in full, regardless of Restart, for the log
    double LastMs() const { return double(last_ns_) / 1e6; }
    uint64_t LastCaptures() const { return last_captures_; }

 private:
    // clamped at 0 (capture numbering can start over)
    template <typename T>
    static T Since(T a, T b) {
        return a > b ? a - b : T(0);
    }

    bool paused_ = false;
    int64_t ns_ = 0;
    uint64_t captures_ = 0;
    // since_*: counting start (Restart moves it); began_*: the pause's start
    int64_t since_ns_ = 0, began_ns_ = 0;
    uint64_t since_frame_ = 0, began_frame_ = 0;
    int64_t last_ns_ = 0;
    uint64_t last_captures_ = 0;
};

// The window's paints since the last Reset, for present_stats: every paint,
// whichever renderer drew it, and on the native renderer which frame each
// showed.
struct PaintLog {
    uint64_t paints = 0;
    std::vector<double> interval_ms;  // since the previous paint
    uint64_t native_paints = 0;
    // of those, new frames and repeats; and frames drawn that no paint showed
    uint64_t shown = 0, repeats = 0, skipped = 0;
    // game's Present to the first paint showing the frame
    std::vector<double> latency_ms;
    // per paint after the first of a run, how far the game's Presents moved on
    // since the paint before: the frame shown's Present less the previous
    // paint's. Steady at the game's interval when every refresh shows the next
    // frame; 0 for a repeat, twice that past a skip (judder)
    std::vector<double> step_ms;
    // game's Present to Published, shown or not: the renderer's own latency,
    // visible even when the window doesn't paint (off every monitor)
    std::vector<double> publish_latency_ms;
};

// Times in steady-clock nanoseconds. Not thread-safe: native_view.cpp holds a
// mutex around every call.
class PaintRecorder {
 public:
    // about 18 minutes at 60 Hz
    static constexpr size_t kMaxSamples = size_t(1) << 16;

    // With `native`, it showed frame `serial` (from 1; 0 none yet) of
    // `source`'s numbering (zero-copy outputs or uploaded pictures).
    void Paint(int64_t now_ns, bool native, int source = 0, uint64_t serial = 0,
               int64_t presented_ns = 0) {
        log_.paints++;
        if (last_ns_ && log_.interval_ms.size() < kMaxSamples)
            log_.interval_ms.push_back(double(now_ns - last_ns_) / 1e6);
        last_ns_ = now_ns;
        // each run of native paints from one source counts on its own
        if (!native || source != source_) {
            last_serial_ = 0;
            last_presented_ns_ = 0;
        }
        source_ = source;
        if (!native) return;
        log_.native_paints++;
        if (!serial) return;
        // a frame numbered over (serial back) or Presented earlier isn't a step
        if (last_serial_ && serial >= last_serial_ && last_presented_ns_ && presented_ns &&
            presented_ns >= last_presented_ns_ && log_.step_ms.size() < kMaxSamples)
            log_.step_ms.push_back(double(presented_ns - last_presented_ns_) / 1e6);
        last_presented_ns_ = presented_ns;
        if (serial == last_serial_) {
            log_.repeats++;
            return;
        }
        if (last_serial_ && serial > last_serial_ + 1) log_.skipped += serial - last_serial_ - 1;
        log_.shown++;
        if (presented_ns) {
            const double ms = double(now_ns - presented_ns) / 1e6;
            if (log_.latency_ms.size() < kMaxSamples) log_.latency_ms.push_back(ms);
            // ~last 30 frames, for the debug overlay
            recent_latency_ms_ = recent_latency_ms_ > 0 ? recent_latency_ms_ * 0.97 + ms * 0.03 : ms;
        }
        last_serial_ = serial;
    }

    void Published(int64_t now_ns, int64_t presented_ns) {
        if (presented_ns && log_.publish_latency_ms.size() < kMaxSamples)
            log_.publish_latency_ms.push_back(double(now_ns - presented_ns) / 1e6);
    }

    // keeps the last paint's time and frame to measure the next against
    void Reset() { log_ = PaintLog{}; }
    const PaintLog& Log() const { return log_; }
    // averaged latency_ms of recent frames, kept through Reset; 0 before any
    double RecentLatencyMs() const { return recent_latency_ms_; }

 private:
    PaintLog log_;
    double recent_latency_ms_ = 0;
    int64_t last_ns_ = 0;
    int source_ = 0;
    uint64_t last_serial_ = 0;
    int64_t last_presented_ns_ = 0;  // the frame last_serial_'s, 0 unknown
};

// For present_stats: a ring of when the newest `kept` DxRnd::Presents ended
// (steady-clock nanoseconds; ~2 minutes at 60 fps), plus the total count,
// which the harness notes to count a longer stretch's frames. Not
// thread-safe: scene_capture.cpp holds a mutex around every call.
class PresentTimes {
 public:
    static constexpr size_t kKept = 8192;

    explicit PresentTimes(size_t kept = kKept) : times_(kept ? kept : 1) {}

    // returns the time since the previous Present, 0 for the first
    int64_t Add(int64_t now_ns) {
        const int64_t since = total_ ? now_ns - times_[(total_ - 1) % times_.size()] : 0;
        times_[total_++ % times_.size()] = now_ns;
        return since;
    }
    uint64_t Total() const { return total_; }
    // kept ones ending at or after `since_ns`, oldest first
    std::vector<int64_t> Since(int64_t since_ns) const {
        std::vector<int64_t> out;
        const uint64_t kept = std::min<uint64_t>(total_, times_.size());
        for (uint64_t i = total_ - kept; i < total_; i++) {
            const int64_t t = times_[i % times_.size()];
            if (t >= since_ns) out.push_back(t);
        }
        return out;
    }

 private:
    std::vector<int64_t> times_;
    uint64_t total_ = 0;
};

}  // namespace band3::render
