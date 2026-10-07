#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// How long the UI thread takes to run something the game's joypad thread asks
// it to: the wait the SDK's SDL input driver puts on every SDL pad's state.
// That driver reads no device when the game asks for one's state: it has the
// UI thread pump SDL's events (CallInUIThread, one request at a time), and
// hands back what the events pumped so far said, so an SDL pad's state is as
// old as the UI thread is slow to get to its queue. The UI thread waits in the
// presenter for the display's vertical blank before each paint it's asked for
// (Presenter::WaitForUITickFromUIThread) while the presenter paces the paints
// itself (vsync off, as with any frame cap), and the SDK's ImGui drawer asks
// for the next paint at the end of each while it has any dialog, which band3's
// FPS overlay always is: so the UI thread can spend most of its time waiting.
//
// The probe does what the SDL driver does, from the same thread: after the
// game reads a player (src/Hooks/input_lock.cpp's XInputGetState), if none of
// its own requests is waiting, it notes the time and asks the UI thread to
// note when it got there. The harness's present_stats has the round trips
// (`ui_round_trip`), and its reset starts them over. It runs only under the
// test harness (test_port), which starts it.

namespace rex::ui {
class WindowedAppContext;
}

namespace band3::input {

// The round trips' times, kept in fixed buckets so recording one costs an
// increment: a quarter of a millisecond wide up to 64 ms, and one bucket over
// that. A percentile is the top of the bucket it falls in (no more than the
// longest trip). Pure, so the unit tests can check it.
class UiRoundTripHistogram {
public:
    static constexpr double kBucketMs = 0.25;
    static constexpr size_t kBuckets = 256;  // up to 64 ms

    void Add(double ms) {
        if (!(ms >= 0)) ms = 0;
        const double bucket = std::min(ms / kBucketMs, double(kBuckets));
        const size_t i = static_cast<size_t>(bucket);
        counts_[i]++;
        count_++;
        max_ = std::max(max_, ms);
    }

    void Reset() { *this = UiRoundTripHistogram{}; }

    uint64_t Count() const { return count_; }
    double Max() const { return max_; }

    // the time `p` (0-1) of the trips took at most; 0 with none
    double Percentile(double p) const {
        if (!count_) return 0;
        const uint64_t rank = std::clamp<uint64_t>(
            static_cast<uint64_t>(std::ceil(p * static_cast<double>(count_))), 1, count_);
        uint64_t seen = 0;
        for (size_t i = 0; i <= kBuckets; i++) {
            seen += counts_[i];
            if (seen >= rank) return i < kBuckets ? std::min((i + 1) * kBucketMs, max_) : max_;
        }
        return max_;
    }

private:
    std::array<uint64_t, kBuckets + 1> counts_{};
    uint64_t count_ = 0;
    double max_ = 0;
};

// the round trips since the probe last started over
struct UiRoundTripStats {
    uint64_t runs = 0;
    double p50 = 0, p95 = 0, max = 0;
};

// The probe itself (src/Hooks/input_lock.cpp). Start hands it the window's
// app context, whose UI thread it times; Stop ends it (requests on their way
// still record). Probe is the joypad thread's, after each read.
void StartUiRoundTripProbe(rex::ui::WindowedAppContext* app_context);
void StopUiRoundTripProbe();
void ProbeUiRoundTrip();
// the round trips so far; `reset` starts them over
UiRoundTripStats GetUiRoundTripStats(bool reset);

}
