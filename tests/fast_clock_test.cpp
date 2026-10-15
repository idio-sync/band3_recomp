// Checks src/fast_clock.h: a scale measured between two readings turns the
// CPU's timestamp counter into steady_clock's nanoseconds without ever going
// backwards, and the clock, once it has calibrated, stays within 1% of
// steady_clock over 50 ms.

#include <doctest/doctest.h>
#include <chrono>
#include <cstdint>
#include "src/fast_clock.h"

using namespace band3::fast_clock;

TEST_CASE("a scale from two readings turns ticks into nanoseconds") {
    // a 4 GHz counter, read 20 ms apart
    const Reading first{1'000, 5'000'000'000};
    const Reading now{1'000 + 80'000'000, 5'020'000'000};
    const Scale scale = ScaleBetween(first, now);
    CHECK(scale.ns_per_tick == 0.25);
    CHECK(scale.NsAt(now.ticks) == now.ns);
    CHECK(scale.NsAt(now.ticks + 4) == now.ns + 1);
    CHECK(scale.NsAt(now.ticks + 4'000'000'000) == now.ns + 1'000'000'000);
}

TEST_CASE("ticks before the scale's reading give that reading's time") {
    const Reading now{80'000'000, 20'000'000};
    const Scale scale = ScaleBetween({0, 0}, now);
    CHECK(scale.NsAt(now.ticks - 1) == now.ns);
    CHECK(scale.NsAt(0) == now.ns);
}

TEST_CASE("the nanoseconds never go back as the ticks rise") {
    // 1/3 ns a tick, which a double can't hold exactly
    const Reading now{60'000'000, 20'000'000};
    const Scale scale = ScaleBetween({0, 0}, now);
    uint64_t last = 0;
    bool rising = true;
    for (uint64_t ticks = now.ticks - 10; ticks < now.ticks + 100'000; ticks++) {
        const uint64_t ns = scale.NsAt(ticks);
        rising = rising && ns >= last;
        last = ns;
    }
    CHECK(rising);
    // a second on, to the nanosecond or so
    const uint64_t second_on = scale.NsAt(now.ticks + 3'000'000'000);
    CHECK(second_on >= now.ns + 1'000'000'000 - 1);
    CHECK(second_on <= now.ns + 1'000'000'000);
}

TEST_CASE("a counter that hasn't moved forward gives no scale") {
    CHECK(ScaleBetween({500, 100}, {500, 20'000'100}).ns_per_tick == 0);
    CHECK(ScaleBetween({500, 100}, {400, 20'000'100}).ns_per_tick == 0);
}

TEST_CASE("elapsed time is never negative") {
    CHECK(ElapsedNs(100, 350) == 250);
    CHECK(ElapsedNs(350, 350) == 0);
    CHECK(ElapsedNs(350, 100) == 0);
}

TEST_CASE("the clock follows steady_clock to within 1% over 50 ms and never goes back") {
    using Steady = std::chrono::steady_clock;
    const auto steady_ns = [] {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            Steady::now().time_since_epoch())
                            .count());
    };
    // It is steady_clock itself until 20 ms after its first read; spin until
    // the counter takes over
    uint64_t last = NowNs();
    bool rising = true;
    if (kHasCounter) {
        const auto give_up = Steady::now() + std::chrono::seconds(2);
        while (!Calibrated() && Steady::now() < give_up) {
            const uint64_t ns = NowNs();
            rising = rising && ns >= last;
            last = ns;
        }
        REQUIRE(Calibrated());
    }
    // One reading of each clock: the closest of a hundred pairs, since a
    // thread descheduled between the two would put them apart
    struct Pair {
        uint64_t fast, steady;
    };
    const auto pair_now = [&] {
        Pair best{0, 0};
        uint64_t narrowest = ~0ull;
        for (int i = 0; i < 100; i++) {
            const uint64_t before = steady_ns();
            const uint64_t fast = NowNs();
            const uint64_t after = steady_ns();
            if (after - before < narrowest) {
                narrowest = after - before;
                best = {fast, before + (after - before) / 2};
            }
        }
        return best;
    };
    const Pair start = pair_now();
    last = start.fast;
    while (steady_ns() - start.steady < 50'000'000) {
        const uint64_t ns = NowNs();
        rising = rising && ns >= last;
        last = ns;
    }
    const Pair end = pair_now();
    CHECK(rising);
    CHECK(end.fast >= last);
    const double fast_ms = double(end.fast - start.fast) / 1e6;
    const double steady_ms = double(end.steady - start.steady) / 1e6;
    CAPTURE(fast_ms);
    CAPTURE(steady_ms);
    CHECK(fast_ms > steady_ms * 0.99);
    CHECK(fast_ms < steady_ms * 1.01);
}
