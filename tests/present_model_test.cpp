// Checks src/Render/present_model.h: where the native picture goes in the
// window (letterboxed to 16:9 as the SDK's presenter does, or stretched), and
// which output texture the native renderer's worker may draw into while the
// SDK's paints sample another, and the paints' numbers for present_stats.

#include <doctest/doctest.h>
#include <utility>
#include "src/Render/gpu_skip.h"
#include "src/Render/present_model.h"

using namespace band3::render;

namespace {

bool Same(const ImageRect& r, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

}  // namespace

TEST_CASE("a 16:9 target is all picture") {
    for (auto [w, h] : {std::pair{1280u, 720u}, std::pair{1600u, 900u}, std::pair{1920u, 1080u},
                        std::pair{3840u, 2160u}}) {
        CAPTURE(w);
        CHECK(Same(LetterboxRect(w, h, true), 0, 0, w, h));
    }
}

TEST_CASE("a wider target has bars left and right, a taller one above and below") {
    // 64:27: 1920 wide, 320 of bars each side
    CHECK(Same(LetterboxRect(2560, 1080, true), 320, 0, 1920, 1080));
    // 4:3: 1024x576, 96 above and below
    CHECK(Same(LetterboxRect(1024, 768, true), 0, 96, 1024, 576));
    // 16:10: 1680x945, 52 above and 53 below
    CHECK(Same(LetterboxRect(1680, 1050, true), 0, 52, 1680, 945));
    // a pixel too wide: 1600 of it, the spare pixel on the right
    CHECK(Same(LetterboxRect(1601, 900, true), 0, 0, 1600, 900));
    CHECK(Same(LetterboxRect(1602, 900, true), 1, 0, 1600, 900));
    // rounded to the nearest pixel: 1000 tall is 1777.8 wide
    CHECK(Same(LetterboxRect(2000, 1000, true), 111, 0, 1778, 1000));
}

TEST_CASE("without letterboxing the picture stretches over the target") {
    CHECK(Same(LetterboxRect(2560, 1080, false), 0, 0, 2560, 1080));
    CHECK(Same(LetterboxRect(1024, 768, false), 0, 0, 1024, 768));
}

TEST_CASE("a degenerate target is never an empty or overflowing picture") {
    CHECK(Same(LetterboxRect(0, 0, true), 0, 0, 0, 0));
    // a sliver keeps a pixel of picture
    CHECK(Same(LetterboxRect(1, 1000, true), 0, 499, 1, 1));
    CHECK(Same(LetterboxRect(1000, 1, true), 499, 0, 2, 1));
    CHECK(Same(LetterboxRect(7680, 4320, true), 0, 0, 7680, 4320));
}

TEST_CASE("the worker draws into a slot that isn't the newest, then publishes it") {
    PresentSlots s;
    CHECK(s.Newest() == -1);
    const int a = s.Acquire();
    REQUIRE(a >= 0);
    // being drawn: not handed out twice
    const int b = s.Acquire();
    CHECK(b >= 0);
    CHECK(b != a);
    s.Abandon(b);
    s.Publish(a);
    CHECK(s.Newest() == a);
    CHECK(s.Serial() == 1);
    // the newest is the next paint's, never drawn over
    for (int i = 0; i < 10; i++) {
        const int next = s.Acquire();
        REQUIRE(next >= 0);
        CHECK(next != s.Newest());
        s.Publish(next);
    }
    CHECK(s.Serial() == 11);
}

TEST_CASE("a slot a paint sampled is free again only once the GPU finished that paint") {
    PresentSlots s;
    const int a = s.Acquire();
    s.Publish(a);
    s.Shown(a, 10);  // paint 10 samples a
    s.Completed(8);
    const int b = s.Acquire();
    s.Publish(b);  // b is the newest; a waits for paint 10
    s.Shown(b, 11);
    const int c = s.Acquire();
    REQUIRE(c >= 0);
    CHECK(c != a);
    CHECK(c != b);
    s.Publish(c);
    // a (paint 10 unfinished) and c (the newest) are taken, and b was
    // sampled by paint 11: nothing is free
    CHECK(s.Acquire() == -1);
    // completed indices only go forward
    s.Completed(9);
    s.Completed(3);
    CHECK(s.CompletedIndex() == 9);
    CHECK(s.Acquire() == -1);
    s.Completed(10);
    CHECK(s.Acquire() == a);
    s.Abandon(a);
    s.Completed(11);
    // both free now: the first one found
    const int next = s.Acquire();
    CHECK((next == a || next == b));
}

TEST_CASE("paints that keep up always leave the worker a slot") {
    PresentSlots s;
    uint64_t paint = 0;
    for (int frame = 0; frame < 100; frame++) {
        const int slot = s.Acquire();
        REQUIRE(slot >= 0);
        s.Publish(slot);
        // each paint shows the newest, and the GPU is a paint behind
        paint++;
        s.Shown(s.Newest(), paint);
        s.Completed(paint - 1);
    }
    CHECK(s.LastUsed() == paint);
}

TEST_CASE("after the presenter stops, slots the GPU may still read stay taken") {
    PresentSlots s;
    const int a = s.Acquire();
    s.Publish(a);
    s.Shown(a, 5);
    s.Forget();
    CHECK(s.Newest() == -1);
    // a waits for paint 5; the other two are free
    const int b = s.Acquire();
    const int c = s.Acquire();
    CHECK(b != a);
    CHECK(c != a);
    CHECK(c >= 0);
    CHECK(s.Acquire() == -1);
    s.Completed(5);
    CHECK(s.Acquire() == a);
}

namespace {
constexpr int64_t kMs = 1000000;  // a millisecond in nanoseconds
}

TEST_CASE("paints are timed against the one before, whichever renderer drew them") {
    PaintRecorder r;
    r.Paint(100 * kMs, false);
    r.Paint(116 * kMs, false);
    r.Paint(150 * kMs, true);
    const PaintLog& log = r.Log();
    CHECK(log.paints == 3);
    CHECK(log.native_paints == 1);
    REQUIRE(log.interval_ms.size() == 2);
    CHECK(log.interval_ms[0] == doctest::Approx(16.0));
    CHECK(log.interval_ms[1] == doctest::Approx(34.0));
    // nothing drawn yet: neither shown nor repeated
    CHECK(log.shown == 0);
    CHECK(log.repeats == 0);
}

TEST_CASE("a native frame shown again is a repeat, one never shown a skip") {
    PaintRecorder r;
    r.Paint(100 * kMs, true, 0, 1, 90 * kMs);
    r.Paint(116 * kMs, true, 0, 1, 90 * kMs);   // again
    r.Paint(133 * kMs, true, 0, 2, 110 * kMs);
    r.Paint(150 * kMs, true, 0, 5, 140 * kMs);  // 3 and 4 never shown
    const PaintLog& log = r.Log();
    CHECK(log.shown == 3);
    CHECK(log.repeats == 1);
    CHECK(log.skipped == 2);
    // each frame's latency at the first paint showing it
    REQUIRE(log.latency_ms.size() == 3);
    CHECK(log.latency_ms[0] == doctest::Approx(10.0));
    CHECK(log.latency_ms[1] == doctest::Approx(23.0));
    CHECK(log.latency_ms[2] == doctest::Approx(10.0));
}

TEST_CASE("starting over keeps the last paint, so the next is measured against it") {
    PaintRecorder r;
    r.Paint(100 * kMs, true, 0, 7, 95 * kMs);
    r.Reset();
    CHECK(r.Log().paints == 0);
    r.Paint(116 * kMs, true, 0, 7, 95 * kMs);
    r.Paint(133 * kMs, true, 0, 9, 120 * kMs);
    const PaintLog& log = r.Log();
    REQUIRE(log.interval_ms.size() == 2);
    CHECK(log.interval_ms[0] == doctest::Approx(16.0));
    CHECK(log.repeats == 1);  // frame 7, shown before the reset
    CHECK(log.shown == 1);
    CHECK(log.skipped == 1);
}

TEST_CASE("an emulated paint or another source starts the frames' count over") {
    PaintRecorder r;
    r.Paint(100 * kMs, true, 0, 3, 90 * kMs);
    r.Paint(116 * kMs, false);
    // the worker went on drawing while the emulated GPU's picture showed
    r.Paint(133 * kMs, true, 0, 10, 120 * kMs);
    // uploads number their frames on their own
    r.Paint(150 * kMs, true, 1, 2, 140 * kMs);
    const PaintLog& log = r.Log();
    CHECK(log.skipped == 0);
    CHECK(log.shown == 3);
    CHECK(log.repeats == 0);
}

// gpu_skip.h's SkipLatch: which frames the emulated GPU skips the game's draws
// in, and when its picture is the game's again

TEST_CASE("the emulated GPU skips from the frame after skipping is wanted") {
    SkipLatch l;
    CHECK_FALSE(l.Skipping());
    CHECK(l.Fresh());  // nothing skipped yet
    // the frame now being drawn was drawn whole; the next is skipped
    CHECK(l.EndFrame(true, 0));
    CHECK(l.Skipping());
    CHECK(l.Fresh());
    // a skipped frame swapped: stale
    CHECK(l.EndFrame(true, 0));
    CHECK_FALSE(l.Fresh());
}

TEST_CASE("after skipping stops, the picture is fresh once two whole frames are swapped") {
    SkipLatch l;
    l.EndFrame(true, 0);
    l.EndFrame(true, 0);
    // F8 back: the frame being drawn is still skipped
    CHECK_FALSE(l.EndFrame(false, 0));
    CHECK_FALSE(l.Fresh());
    // one whole frame: with even/odd rendering it presents the skipped world
    l.EndFrame(false, 0);
    CHECK_FALSE(l.Fresh());
    l.EndFrame(false, 0);
    CHECK(l.Fresh());
}

TEST_CASE("whole frames asked for are drawn whatever is wanted, then skipping goes on") {
    SkipLatch l;
    l.EndFrame(true, 0);
    l.EndFrame(true, 0);
    CHECK_FALSE(l.Fresh());
    // asked for 3: the next three frames are drawn whole
    CHECK_FALSE(l.EndFrame(true, 3));
    CHECK(l.FullPending() == 2);
    CHECK_FALSE(l.EndFrame(true, 0));
    CHECK_FALSE(l.Fresh());  // one whole frame swapped
    CHECK_FALSE(l.EndFrame(true, 0));
    CHECK(l.Fresh());  // two: a capture can hold this one
    // then skipped again
    CHECK(l.EndFrame(true, 0));
    CHECK(l.Fresh());  // the third whole frame swapped
    l.EndFrame(true, 0);
    CHECK_FALSE(l.Fresh());
}

TEST_CASE("a request while one runs keeps the larger") {
    SkipLatch l;
    l.EndFrame(true, 5);
    CHECK(l.FullPending() == 4);
    l.EndFrame(true, 2);
    CHECK(l.FullPending() == 3);
    l.EndFrame(true, 9);
    CHECK(l.FullPending() == 8);
}
