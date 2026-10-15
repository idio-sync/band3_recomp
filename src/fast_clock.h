#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#if defined(__x86_64__) || defined(_M_X64)
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif

// A clock for timing code that runs hundreds of times a frame (the capture
// hooks' timers), read from the CPU's timestamp counter. steady_clock::now()
// is two library calls, the performance counter through ntdll and its
// frequency from the C++ runtime: 18 ns a read in a tight loop on a Ryzen 7
// PRO 6850U and more in the game's cold caches, against 8 ns for the counter,
// which is one instruction. On the x86-64-v3 CPUs band3 needs it ticks at one
// rate whatever the core's clock does, so all it needs is a scale to
// nanoseconds, measured against steady_clock the first time NowNs is called
// 20 ms or more after its first call. Until then, and where there is no
// counter to read, NowNs is steady_clock itself, so early frames are timed
// right. Both are on steady_clock's timeline, so a time taken before the
// switch can be subtracted from one taken after it.

namespace band3::fast_clock {

#if defined(__x86_64__) || defined(_M_X64)
inline constexpr bool kHasCounter = true;
inline uint64_t ReadCounter() { return __rdtsc(); }
#else
inline constexpr bool kHasCounter = false;
inline uint64_t ReadCounter() { return 0; }
#endif

// steady_clock's time since its epoch, in nanoseconds
inline uint64_t SteadyNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

// the counter and steady_clock at the same moment
struct Reading {
    uint64_t ticks = 0;
    uint64_t ns = 0;
};

// Counter ticks as steady_clock nanoseconds: `at` is where the scale was
// measured, and each tick after it adds ns_per_tick.
struct Scale {
    Reading at;
    double ns_per_tick = 0;

    // The time at counter reading `ticks`: never before `at`'s (a reading a
    // few ticks behind, from another core, isn't a huge one) and never going
    // back as `ticks` rises.
    uint64_t NsAt(uint64_t ticks) const {
        const int64_t since = int64_t(ticks - at.ticks);
        if (since <= 0) return at.ns;
        return at.ns + uint64_t(int64_t(double(since) * ns_per_tick));
    }
};

// The scale between two readings, `now` the later: the nanoseconds that went
// by over the ticks that did. ns_per_tick is 0 (no scale) if either didn't
// go forward.
inline Scale ScaleBetween(const Reading& first, const Reading& now) {
    Scale scale;
    scale.at = now;
    if (now.ticks > first.ticks && now.ns > first.ns)
        scale.ns_per_tick = double(now.ns - first.ns) / double(now.ticks - first.ticks);
    return scale;
}

// nanoseconds from `from` to `to`, 0 if `to` is the earlier (readings from
// different cores can differ by a tick or two)
inline uint64_t ElapsedNs(uint64_t from, uint64_t to) { return to > from ? to - from : 0; }

namespace detail {

// set once, before g_calibrated is
inline Scale g_scale;
inline std::atomic<bool> g_calibrated{false};

// NowNs's first 20 ms are steady_clock's
inline constexpr uint64_t kCalibrateAfterNs = 20'000'000;

// The counter before steady_clock, so a scale made from the two is a little
// ahead of steady_clock, not behind: a thread that switches to it doesn't go
// back in time.
inline Reading ReadBoth() {
    const uint64_t ticks = ReadCounter();
    return {ticks, SteadyNs()};
}

// steady_clock's time, measuring the scale once 20 ms have passed since the
// first call (by whichever thread gets there first)
inline uint64_t NowNsUncalibrated() {
    static const Reading first = ReadBoth();
    const Reading now = ReadBoth();
    static std::atomic<bool> measuring{false};
    if (now.ns - first.ns >= kCalibrateAfterNs && !measuring.exchange(true)) {
        const Scale scale = ScaleBetween(first, now);
        // a counter that doesn't tick leaves the clock on steady_clock
        if (scale.ns_per_tick > 0) {
            g_scale = scale;
            g_calibrated.store(true, std::memory_order_release);
        }
    }
    return now.ns;
}

}  // namespace detail

// steady_clock's time since its epoch, in nanoseconds, from the counter once
// calibrated
inline uint64_t NowNs() {
    if constexpr (kHasCounter) {
        if (detail::g_calibrated.load(std::memory_order_acquire))
            return detail::g_scale.NsAt(ReadCounter());
        return detail::NowNsUncalibrated();
    } else {
        return SteadyNs();
    }
}

// whether NowNs reads the counter now
inline bool Calibrated() { return detail::g_calibrated.load(std::memory_order_acquire); }

}  // namespace band3::fast_clock
