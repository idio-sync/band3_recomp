#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// Experimental: the arithmetic and bookkeeping of the native renderer's
// presentation (renderer = native, native_view.cpp's NativePresentDrawer),
// kept apart so the unit tests can check them: where the picture goes in the
// window, which of the presenter's output textures the renderer's worker
// may draw into while the SDK's presenter samples another, when each frame
// reaches them, and the paints' pacing as the harness's present_stats
// reports it.

namespace band3::render {

// where the picture goes in a render target, in its pixels
struct ImageRect {
    uint32_t x = 0, y = 0, w = 0, h = 0;
};

// The largest ar_x:ar_y rectangle that fits target_w x target_h, centred (the
// bars split as evenly as whole pixels can), as the SDK's presenter
// letterboxes the emulated picture; all of the target with `letterbox` off
// (the SDK's present_letterbox), which stretches it as the SDK does.
inline ImageRect LetterboxRect(uint32_t target_w, uint32_t target_h, bool letterbox,
                               uint32_t ar_x = 16, uint32_t ar_y = 9) {
    ImageRect r{0, 0, target_w, target_h};
    if (!letterbox || !target_w || !target_h || !ar_x || !ar_y) return r;
    // in 64 bits: 7680 * 16 doesn't overflow 32, but a caller's aspect might
    const uint64_t wide = uint64_t(target_w) * ar_y, tall = uint64_t(target_h) * ar_x;
    if (wide > tall) {
        // wider than the picture: bars left and right, the width rounded to
        // the nearest pixel
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

// The presenter's output textures and who may touch which. The worker draws
// a frame into one and publishes it as the newest; each paint samples the
// newest on the SDK's GPU queue, noting the paint's submission index on it,
// and passes on the newest submission the GPU has finished. The worker draws
// only into a slot that isn't the newest (the next paint may sample it) and
// that no unfinished paint sampled (its index <= completed). With three, one is always free while paints keep
// finishing: the newest, the one the last unfinished paint sampled, and the
// other. Not thread-safe: native_view.cpp holds its mutex around every call.
//
// The worker submits a frame and records the next while the GPU draws it
// (native_view.cpp's Renderer::Run), so a slot drawn into can be submitted
// and not yet finished (Submitted): it stays taken, and isn't published until
// the GPU has finished it (Finished), as the paints sample it on another
// queue, which nothing but the worker's wait orders after SDL's.
//
// Presenting comes and goes (F8), and each stretch of it shows only its own
// frames: ones drawn for it (Publish's `generation`) from captures the game
// published after it began (Fresh). The capture newest when F8 turns native
// again is the last one from before the switch (capture stops with the
// emulated GPU), seconds old, and a frame the worker was still drawing as
// presenting stopped would otherwise be the newest when it starts again.
class PresentSlots {
 public:
    static constexpr int kCount = 3;

    // presenting starts, at `now_ns` (steady-clock nanoseconds): a stretch
    // with nothing to show until a frame is published for it
    void Start(int64_t now_ns) {
        newest_ = -1;
        generation_++;
        since_ns_ = now_ns;
    }
    // presenting stopped: nothing is newest, a frame being drawn is
    // published to nobody, and the paints' indices are kept (the GPU may
    // still be reading a slot)
    void Forget() {
        newest_ = -1;
        generation_++;
    }
    // the stretch now, for the worker to note before it draws
    uint64_t Generation() const { return generation_; }
    // whether a capture the game published at `published_ns` may be drawn
    // for this stretch: one published after it started
    bool Fresh(int64_t published_ns) const { return published_ns > since_ns_; }

    // the worker: the slot to draw the next frame into, marked as being
    // drawn, or -1 if none is free
    int Acquire() {
        for (int i = 0; i < kCount; i++) {
            Slot& s = slots_[i];
            if (i == newest_ || s.drawing || s.used > completed_) continue;
            s.drawing = true;
            return i;
        }
        return -1;
    }
    // the slot being drawn is submitted, and the GPU may still be drawing it:
    // taken still, and not published until Finished
    void Submitted(int slot) {
        if (Valid(slot) && slots_[slot].drawing) slots_[slot].in_flight = true;
    }
    // the GPU has finished drawing the slot Submitted
    void Finished(int slot) {
        if (Valid(slot)) slots_[slot].in_flight = false;
    }
    // whether `slot` is submitted and the GPU hasn't finished it: Publish
    // refuses it, and the worker mustn't Abandon it as refused (the next
    // frame could take it while the GPU still writes it)
    bool Unfinished(int slot) const { return Valid(slot) && slots_[slot].in_flight; }
    // slots submitted that the GPU hasn't finished
    int InFlight() const {
        int n = 0;
        for (const Slot& s : slots_) n += s.in_flight ? 1 : 0;
        return n;
    }
    // Drawn for stretch `generation`: the newest from now on, or, if that
    // stretch is over, free again and false. A slot the GPU hasn't finished
    // (Submitted) is never published: false, and it stays as it was.
    bool Publish(int slot, uint64_t generation) {
        if (!Valid(slot) || slots_[slot].in_flight) return false;
        slots_[slot].drawing = false;
        if (generation != generation_) return false;
        newest_ = slot;
        serial_++;
        return true;
    }
    bool Publish(int slot) { return Publish(slot, generation_); }
    // not drawn after all (the GPU failed, or the worker stopped): free
    // again, as it was. The GPU may still be drawing it if it was Submitted;
    // nothing samples a slot until it's published, and the next frame drawn
    // into it goes after that on SDL's queue.
    void Abandon(int slot) {
        if (!Valid(slot)) return;
        slots_[slot].drawing = false;
        slots_[slot].in_flight = false;
    }

    // the drawer: the paint numbered `submission` samples `slot`
    void Shown(int slot, uint64_t submission) {
        if (!Valid(slot)) return;
        Slot& s = slots_[slot];
        if (submission > s.used) s.used = submission;
    }
    // the GPU has finished every paint up to `completed`
    void Completed(uint64_t completed) {
        if (completed > completed_) completed_ = completed;
    }
    // the newest paint any slot was sampled in, for settling them all at once
    // when paints stop (minimized: no paints, so no completed index either)
    uint64_t LastUsed() const {
        uint64_t last = 0;
        for (const Slot& s : slots_)
            if (s.used > last) last = s.used;
        return last;
    }

    // the newest frame drawn, or -1 before the first
    int Newest() const { return newest_; }
    // published frames so far, to tell a new one
    uint64_t Serial() const { return serial_; }
    uint64_t CompletedIndex() const { return completed_; }

 private:
    struct Slot {
        bool drawing = false;
        bool in_flight = false;  // submitted, not finished (drawing too)
        uint64_t used = 0;       // the last paint that sampled it
    };
    static bool Valid(int slot) { return slot >= 0 && slot < kCount; }

    Slot slots_[kCount];
    int newest_ = -1;
    uint64_t serial_ = 0;
    uint64_t completed_ = 0;
    uint64_t generation_ = 0;
    int64_t since_ns_ = 0;
};

// When the worker's frames reach the window. A frame takes as long to draw as
// what it draws: with RB3's even/odd rendering a post frame (the world and its
// post-processing) takes several times what a world frame (the post buffer
// under its overlay) does, so frames published as soon as they're drawn come
// out about 8 and 25 ms apart rather than every 16.7, and one paint (one
// refresh, on a monitor) can find two new frames, of which the first is never
// shown. So each frame is published a steady delay after the game presented
// it: about the slowest of the last few frames' own (the second slowest, so
// one hitch doesn't hold the next few back), never more than the game's
// frame interval less a millisecond, by when the next frame is there to
// draw. A frame slower than that is published as soon as it's drawn. Not
// thread-safe: the worker's alone.
class PublishPacer {
 public:
    static constexpr int kHistory = 8;
    static constexpr int64_t kMs = 1000000;  // a millisecond in nanoseconds

    // Capture `frame` (numbered as captured), which the game presented at
    // `presented_ns`, drawn by `now_ns`: when to publish it, from `now_ns`
    // on (steady-clock nanoseconds).
    int64_t Due(uint64_t frame, int64_t presented_ns, int64_t now_ns) {
        // the game's frame interval, from consecutive captures' times
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
        // the second slowest of the last kHistory, or with fewer than two
        // this frame's own
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
    // the game's frame interval as measured, in nanoseconds
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

// With emulated_gpu off each frame the worker publishes asks the window to
// paint (NativePresentDrawer::Start), which is GPU work no one sees while the
// window can't be seen: minimized (its client area then 0x0), hidden, or with
// no client area left. The first frame published once it can be seen again
// asks again.
inline bool PaintWanted(bool minimized, bool visible, uint32_t client_w, uint32_t client_h) {
    return !minimized && visible && client_w > 0 && client_h > 0;
}

// The window's paints since the numbers last started over, for the harness's
// present_stats: every paint, whichever renderer drew it, and on the native
// renderer which of its frames each showed.
struct PaintLog {
    uint64_t paints = 0;
    std::vector<double> interval_ms;  // between each paint and the one before
    uint64_t native_paints = 0;       // paints the native renderer drew in
    // of those, the ones showing a frame no paint had shown, and the ones
    // showing the same frame again; and frames drawn that no paint showed
    uint64_t shown = 0, repeats = 0, skipped = 0;
    // from the game's Present of each frame shown to the first paint showing it
    std::vector<double> latency_ms;
    // from the game's Present of each new frame the native renderer handed
    // the window to its handing it over (Published), whether a paint ever
    // showed it or not: the renderer's own latency, which a window that
    // doesn't paint (off every monitor) can't hide
    std::vector<double> publish_latency_ms;
};

// Keeps a PaintLog from each paint's time (steady-clock nanoseconds). Not
// thread-safe: native_view.cpp holds a mutex around every call.
class PaintRecorder {
 public:
    // at most this many intervals and latencies: about 18 minutes at 60 Hz
    static constexpr size_t kMaxSamples = size_t(1) << 16;

    // A paint at `now_ns`. With `native`, it showed native frame `serial`
    // (frames numbered from 1 as they were drawn, 0 none yet) from
    // `source` (the frames' numbering: zero-copy outputs or uploaded
    // pictures), which the game presented at `presented_ns`.
    void Paint(int64_t now_ns, bool native, int source = 0, uint64_t serial = 0,
               int64_t presented_ns = 0) {
        log_.paints++;
        if (last_ns_ && log_.interval_ms.size() < kMaxSamples)
            log_.interval_ms.push_back(double(now_ns - last_ns_) / 1e6);
        last_ns_ = now_ns;
        // a stretch of the native renderer's paints is counted on its own,
        // as is one from the other source
        if (!native || source != source_) last_serial_ = 0;
        source_ = source;
        if (!native) return;
        log_.native_paints++;
        if (!serial) return;
        if (serial == last_serial_) {
            log_.repeats++;
            return;
        }
        if (last_serial_ && serial > last_serial_ + 1) log_.skipped += serial - last_serial_ - 1;
        log_.shown++;
        if (presented_ns && log_.latency_ms.size() < kMaxSamples)
            log_.latency_ms.push_back(double(now_ns - presented_ns) / 1e6);
        last_serial_ = serial;
    }

    // The native renderer handed the window a new frame at `now_ns`, which
    // the game presented at `presented_ns`.
    void Published(int64_t now_ns, int64_t presented_ns) {
        if (presented_ns && log_.publish_latency_ms.size() < kMaxSamples)
            log_.publish_latency_ms.push_back(double(now_ns - presented_ns) / 1e6);
    }

    // The numbers start over; the last paint's time and frame are kept, so
    // the next paint's interval and frame are measured against them.
    void Reset() { log_ = PaintLog{}; }
    const PaintLog& Log() const { return log_; }

 private:
    PaintLog log_;
    int64_t last_ns_ = 0;
    int source_ = 0;
    uint64_t last_serial_ = 0;
};

}  // namespace band3::render
