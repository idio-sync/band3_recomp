#pragma once

#include <cstdint>

// Experimental: the arithmetic and bookkeeping of the native renderer's
// presentation (renderer = native, native_view.cpp's NativePresentDrawer),
// kept apart so the unit tests can check them: where the picture goes in the
// window, and which of the presenter's output textures the renderer's worker
// may draw into while the SDK's presenter samples another.

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
class PresentSlots {
 public:
    static constexpr int kCount = 3;

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
    // drawn: the newest from now on
    void Publish(int slot) {
        if (!Valid(slot)) return;
        slots_[slot].drawing = false;
        newest_ = slot;
        serial_++;
    }
    // not drawn after all (the GPU failed): free again, as it was
    void Abandon(int slot) {
        if (Valid(slot)) slots_[slot].drawing = false;
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

    // the presenter stopped: nothing is newest, and the paints' indices are
    // kept (the GPU may still be reading a slot)
    void Forget() { newest_ = -1; }

 private:
    struct Slot {
        bool drawing = false;
        uint64_t used = 0;  // the last paint that sampled it
    };
    static bool Valid(int slot) { return slot >= 0 && slot < kCount; }

    Slot slots_[kCount];
    int newest_ = -1;
    uint64_t serial_ = 0;
    uint64_t completed_ = 0;
};

}  // namespace band3::render
