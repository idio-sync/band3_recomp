// Checks src/Render/present_model.h: where the native picture goes in the
// window (letterboxed to 16:9 as the SDK's presenter does, or stretched), and
// which output texture the native renderer's worker may draw into while the
// SDK's paints sample another, and the paints' numbers for present_stats.

#include <doctest/doctest.h>
#include <utility>
#include <vector>
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

TEST_CASE("a slot submitted to the GPU is never handed out or published before it finishes") {
    PresentSlots s;
    const int a = s.Acquire();
    REQUIRE(a >= 0);
    s.Submitted(a);
    CHECK(s.InFlight() == 1);
    // the worker records the next frame meanwhile, into another slot
    const int b = s.Acquire();
    REQUIRE(b >= 0);
    CHECK(b != a);
    s.Submitted(b);
    CHECK(s.InFlight() == 2);
    // not finished: not published, and still taken
    CHECK_FALSE(s.Publish(a));
    CHECK(s.Newest() == -1);
    CHECK(s.Serial() == 0);
    const int c = s.Acquire();
    REQUIRE(c >= 0);
    CHECK(c != a);
    CHECK(c != b);
    CHECK(s.Acquire() == -1);
    s.Abandon(c);
    // finished: published as any other
    s.Finished(a);
    CHECK(s.InFlight() == 1);
    CHECK(s.Publish(a));
    CHECK(s.Newest() == a);
    CHECK(s.Serial() == 1);
    // a slot submitted for a stretch that has ended is still not published,
    // and once finished it's free again as before
    const uint64_t stretch = s.Generation();
    s.Forget();
    CHECK_FALSE(s.Publish(b, stretch));
    s.Finished(b);
    CHECK_FALSE(s.Publish(b, stretch));
    CHECK(s.Newest() == -1);
    CHECK(s.InFlight() == 0);
    CHECK(s.Acquire() >= 0);
}

TEST_CASE("a refused unfinished slot stays taken until the GPU finishes it") {
    PresentSlots s;
    const int a = s.Acquire();
    REQUIRE(a >= 0);
    CHECK_FALSE(s.Unfinished(a));
    s.Submitted(a);
    CHECK(s.Unfinished(a));
    // refused, for being unfinished rather than for its stretch: the worker
    // tells them apart by Unfinished and leaves this one be, so no other
    // frame is drawn into it while the GPU still writes it
    CHECK_FALSE(s.Publish(a));
    CHECK(s.Unfinished(a));
    for (int i = 0; i < PresentSlots::kCount; i++) CHECK(s.Acquire() != a);
    s.Finished(a);
    CHECK_FALSE(s.Unfinished(a));
    CHECK(s.Publish(a));
    CHECK_FALSE(s.Unfinished(-1));
    CHECK_FALSE(s.Unfinished(PresentSlots::kCount));
}

TEST_CASE("an abandoned submitted slot is free again, and Submitted needs a slot being drawn") {
    PresentSlots s;
    const int a = s.Acquire();
    s.Submitted(a);
    s.Abandon(a);
    CHECK(s.InFlight() == 0);
    // free again, and drawn and published as any other
    CHECK(s.Acquire() == a);
    CHECK(s.Publish(a));
    // the newest isn't being drawn: Submitted leaves it be
    s.Submitted(a);
    CHECK(s.InFlight() == 0);
    s.Submitted(-1);
    s.Finished(PresentSlots::kCount);
    CHECK(s.InFlight() == 0);
}

namespace {
constexpr int64_t kMs = 1000000;  // a millisecond in nanoseconds
}

TEST_CASE("a stretch of presenting shows only frames drawn for it from captures after it began") {
    PresentSlots s;
    s.Start(1000);
    const uint64_t first = s.Generation();
    // a capture from before F8 back to native isn't drawn for the window
    CHECK_FALSE(s.Fresh(999));
    CHECK_FALSE(s.Fresh(1000));
    CHECK(s.Fresh(1001));
    const int a = s.Acquire();
    REQUIRE(a >= 0);
    CHECK(s.Publish(a, first));
    CHECK(s.Newest() == a);
    CHECK(s.Serial() == 1);
    s.Shown(a, 1);
    s.Completed(1);
    // the worker is drawing when presenting stops: that frame is nobody's
    const int b = s.Acquire();
    REQUIRE(b >= 0);
    s.Forget();
    CHECK(s.Newest() == -1);
    CHECK_FALSE(s.Publish(b, first));
    CHECK(s.Newest() == -1);
    CHECK(s.Serial() == 1);
    // nor is it when presenting starts again, and the slot is free again
    s.Start(5000);
    CHECK(s.Generation() != first);
    CHECK_FALSE(s.Fresh(4000));
    CHECK(s.Newest() == -1);
    int got[PresentSlots::kCount];
    for (int& g : got) g = s.Acquire();
    CHECK((got[0] == b || got[1] == b || got[2] == b));
    for (int g : got) s.Abandon(g);
    // the new stretch's first frame is the newest
    const int c = s.Acquire();
    CHECK(s.Publish(c, s.Generation()));
    CHECK(s.Newest() == c);
    CHECK(s.Serial() == 2);
}

TEST_CASE("starting presenting again clears what the last stretch showed") {
    PresentSlots s;
    s.Start(10);
    const int a = s.Acquire();
    s.Publish(a);
    // F8 to native while still native (Start twice) or after a stop: no
    // newest until a frame of the new stretch is published
    s.Start(20);
    CHECK(s.Newest() == -1);
    // the paints' indices are kept: a slot an unfinished paint sampled stays taken
    s.Shown(a, 4);
    s.Start(30);
    for (int i = 0; i < PresentSlots::kCount - 1; i++) CHECK(s.Acquire() >= 0);
    CHECK(s.Acquire() == -1);
    s.Completed(4);
    CHECK(s.Acquire() == a);
}

TEST_CASE("frames are published a steady delay after the game presented them") {
    PublishPacer p;
    constexpr int64_t kFrame = 16666667;
    // even/odd rendering: post frames take 10 ms, world frames 2
    int64_t last_publish = 0;
    std::vector<int64_t> gaps;
    for (uint64_t f = 1; f <= 40; f++) {
        const int64_t presented = int64_t(f) * kFrame;
        const int64_t own = (f % 2) ? 10 * kMs : 2 * kMs;
        const int64_t due = p.Due(f, presented, presented + own);
        CHECK(due >= presented + own);
        if (last_publish && f > 4) gaps.push_back(due - last_publish);
        last_publish = due;
    }
    // once it has seen a post frame, every frame goes out 10 ms after its
    // Present, a frame apart
    for (int64_t g : gaps) CHECK(double(g) == doctest::Approx(double(kFrame)).epsilon(0.001));
    CHECK(double(p.IntervalNs()) == doctest::Approx(double(kFrame)).epsilon(0.001));
}

TEST_CASE("the pacer never holds a frame past the game's next one, and lets a hitch go") {
    PublishPacer p;
    constexpr int64_t kFrame = 16666667;
    uint64_t f = 1;
    for (; f <= 8; f++) {
        const int64_t presented = int64_t(f) * kFrame;
        p.Due(f, presented, presented + 3 * kMs);
    }
    // one slow frame: published when drawn, and the frames after aren't
    // held for it (the second slowest counts)
    int64_t presented = int64_t(f) * kFrame;
    CHECK(p.Due(f, presented, presented + 40 * kMs) == presented + 40 * kMs);
    f++;
    presented = int64_t(f) * kFrame;
    CHECK(p.Due(f, presented, presented + 3 * kMs) == presented + 3 * kMs);
    // slow frames all along: held no later than a frame less a millisecond
    for (int i = 0; i < 8; i++, f++) {
        presented = int64_t(f) * kFrame;
        p.Due(f, presented, presented + 30 * kMs);
    }
    presented = int64_t(f) * kFrame;
    CHECK(p.Due(f, presented, presented + 2 * kMs) == presented + kFrame - kMs);
    // a reset starts over: a frame on its own goes when drawn
    p.Reset();
    CHECK(p.Due(1, kFrame, kFrame + 2 * kMs) == kFrame + 2 * kMs);
}

TEST_CASE("at 120 Hz, pipelined frames go out a frame apart and never past the next frame") {
    // The worker learns its frame is done when a poll of its fence finds it
    // (native_view.cpp's kFencePoll), up to a millisecond after the GPU
    // finished, so each frame's own time has that much jitter on top of
    // even/odd rendering's: post frames 5.5 ms to recording and the GPU's
    // end, world frames 1.5.
    PublishPacer p;
    constexpr int64_t kFrame = 8333333;
    uint32_t seed = 12345;
    auto jitter = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return int64_t(seed >> 8) % kMs;
    };
    int64_t last_publish = 0;
    std::vector<int64_t> gaps;
    for (uint64_t f = 1; f <= 120; f++) {
        const int64_t presented = int64_t(f) * kFrame;
        const int64_t own = ((f % 2) ? 5500 * 1000 : 1500 * 1000) + jitter();
        const int64_t due = p.Due(f, presented, presented + own);
        CHECK(due >= presented + own);
        // once the interval is measured: never held past the next frame
        // less a millisecond
        if (f > 40) CHECK(due - presented <= kFrame - kMs);
        if (last_publish && f > 4) gaps.push_back(due - last_publish);
        last_publish = due;
    }
    CHECK(double(p.IntervalNs()) == doctest::Approx(double(kFrame)).epsilon(0.001));
    // no two publications in one frame's time: each a frame apart, give or
    // take the poll's millisecond
    for (int64_t g : gaps) {
        CHECK(g >= kFrame - kMs);
        CHECK(g <= kFrame + kMs);
    }
}

TEST_CASE("at 120 Hz, a frame slower than the interval goes when done and holds no other") {
    PublishPacer p;
    constexpr int64_t kFrame = 8333333;
    uint64_t f = 1;
    // long enough for the interval measured to settle
    for (; f <= 200; f++) {
        const int64_t presented = int64_t(f) * kFrame;
        p.Due(f, presented, presented + ((f % 2) ? 5 * kMs : 2 * kMs));
    }
    // a post frame the GPU took 12 ms over: published when it's done, and
    // the frames after keep their steady delay (the second slowest counts)
    int64_t presented = int64_t(f) * kFrame;
    CHECK(p.Due(f, presented, presented + 12 * kMs) == presented + 12 * kMs);
    f++;
    presented = int64_t(f) * kFrame;
    CHECK(p.Due(f, presented, presented + 2 * kMs) == presented + 5 * kMs);
    // frames slower than the interval all along: held to the interval less
    // a millisecond at most, so the next frame is drawn and published in its
    // own interval
    for (int i = 0; i < 8; i++, f++) {
        presented = int64_t(f) * kFrame;
        p.Due(f, presented, presented + 10 * kMs);
    }
    presented = int64_t(f) * kFrame;
    // (the interval as measured, which settles within a few nanoseconds of
    // the frames' 8333333)
    const int64_t due = p.Due(f, presented, presented + 2 * kMs);
    CHECK(due >= presented + kFrame - kMs);
    CHECK(due <= presented + kFrame - kMs + 10);
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

TEST_CASE("each frame handed to the window is timed from its Present, painted or not") {
    PaintRecorder r;
    r.Published(110 * kMs, 100 * kMs);
    r.Published(130 * kMs, 117 * kMs);
    r.Published(140 * kMs, 0);  // no Present known: not timed
    const PaintLog& log = r.Log();
    REQUIRE(log.publish_latency_ms.size() == 2);
    CHECK(log.publish_latency_ms[0] == doctest::Approx(10.0));
    CHECK(log.publish_latency_ms[1] == doctest::Approx(13.0));
    // not paints
    CHECK(log.paints == 0);
    CHECK(log.shown == 0);
    r.Reset();
    CHECK(r.Log().publish_latency_ms.empty());
}

TEST_CASE("a published frame asks for a paint only while the window can be seen") {
    CHECK(PaintWanted(false, true, 1280, 720));
    CHECK(PaintWanted(false, true, 1, 1));
    // minimized: Windows reports its client area as 0x0 then, but either
    // alone holds the paint back
    CHECK_FALSE(PaintWanted(true, true, 0, 0));
    CHECK_FALSE(PaintWanted(true, true, 1280, 720));
    CHECK_FALSE(PaintWanted(false, true, 0, 0));
    CHECK_FALSE(PaintWanted(false, true, 1280, 0));
    CHECK_FALSE(PaintWanted(false, true, 0, 720));
    // hidden
    CHECK_FALSE(PaintWanted(false, false, 1280, 720));
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

TEST_CASE("with even/odd rendering, fresh once a post frame follows a whole world frame") {
    constexpr int kWorld = 1, kPost = 2;
    // skipping stops on a world frame: it and the post frame after are the
    // game's picture, which the presenter shows a frame later
    SkipLatch a;
    a.EndFrame(true, 0, kPost);
    a.EndFrame(true, 0, kWorld);
    CHECK_FALSE(a.EndFrame(false, 0, kPost));  // still skipped
    a.EndFrame(false, 0, kWorld);
    CHECK_FALSE(a.Fresh());  // it shows the post buffer from a skipped frame
    a.EndFrame(false, 0, kPost);
    CHECK_FALSE(a.Fresh());  // the game's, but the presenter shows the one before
    a.EndFrame(false, 0, kWorld);
    CHECK(a.Fresh());  // the post buffer that post frame made
    a.EndFrame(false, 0, kPost);
    CHECK(a.Fresh());
    // on a post frame: it post-processes the world drawn while skipped, and
    // the world frame after shows that, so it takes the next post frame
    SkipLatch b;
    b.EndFrame(true, 0, kWorld);
    b.EndFrame(true, 0, kPost);
    CHECK_FALSE(b.EndFrame(false, 0, kWorld));
    b.EndFrame(false, 0, kPost);
    CHECK_FALSE(b.Fresh());
    b.EndFrame(false, 0, kWorld);
    CHECK_FALSE(b.Fresh());  // two whole frames, but no whole world under post
    b.EndFrame(false, 0, 0);
    CHECK_FALSE(b.Fresh());  // a frame that draws neither shows the post buffer too
    b.EndFrame(false, 0, kPost);
    CHECK_FALSE(b.Fresh());
    b.EndFrame(false, 0, kWorld);
    CHECK(b.Fresh());
    // a frame that does both, or doesn't say, is the game's: two whole frames
    SkipLatch c;
    c.EndFrame(true, 0, 7);
    c.EndFrame(false, 0, 7);
    c.EndFrame(false, 0, 7);
    CHECK_FALSE(c.Fresh());
    c.EndFrame(false, 0, -1);
    CHECK(c.Fresh());
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

TEST_CASE("whole frames are counted from the last skipped one, for a capture's hold") {
    SkipLatch l;
    CHECK(l.WholeFrames() == 2);  // before the first frame
    l.EndFrame(false, 0);
    CHECK(l.WholeFrames() == 3);  // never skipping, it only grows
    l.EndFrame(true, 0);
    l.EndFrame(true, 0);
    CHECK(l.WholeFrames() == 0);
    // a capture asks for kWholeFramesToHold and more: the count reaches it
    // while frames are still drawn whole
    l.EndFrame(true, kWholeFramesToHold + 40);
    for (int i = 0; i < kWholeFramesToHold; i++) CHECK_FALSE(l.EndFrame(true, 0));
    CHECK(l.WholeFrames() == kWholeFramesToHold);
    CHECK(l.FullPending() == 39);
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

// emulated_gpu_while_native swap_only: a level beyond skip_draws, latched the
// same way

TEST_CASE("swap_only takes effect at the next frame and goes stale like skip_draws") {
    SkipLatch l;
    CHECK(l.Level() == SkipLevel::kFull);
    CHECK(l.EndFrame(SkipLevel::kSwapOnly, 0) == SkipLevel::kSwapOnly);
    CHECK(l.Level() == SkipLevel::kSwapOnly);
    CHECK(l.Skipping());
    CHECK(l.Fresh());  // the frame swapped was whole
    // a swap_only frame swapped: stale
    CHECK(l.EndFrame(SkipLevel::kSwapOnly, 0) == SkipLevel::kSwapOnly);
    CHECK_FALSE(l.Fresh());
    CHECK(l.WholeFrames() == 0);
}

TEST_CASE("switching between skip_draws and swap_only keeps the picture stale") {
    SkipLatch l;
    l.EndFrame(SkipLevel::kSkipDraws, 0);
    l.EndFrame(SkipLevel::kSkipDraws, 0);
    CHECK_FALSE(l.Fresh());
    // the frame being drawn stays at its level; the next one changes
    CHECK(l.Level() == SkipLevel::kSkipDraws);
    CHECK(l.EndFrame(SkipLevel::kSwapOnly, 0) == SkipLevel::kSwapOnly);
    CHECK(l.WholeFrames() == 0);
    CHECK_FALSE(l.Fresh());
    CHECK(l.EndFrame(SkipLevel::kSkipDraws, 0) == SkipLevel::kSkipDraws);
    CHECK(l.WholeFrames() == 0);
    CHECK_FALSE(l.Fresh());
    CHECK(l.EndFrame(SkipLevel::kSwapOnly, 0) == SkipLevel::kSwapOnly);
    CHECK_FALSE(l.Fresh());
    // full again: fresh after two whole frames, as from skip_draws
    CHECK(l.EndFrame(SkipLevel::kFull, 0) == SkipLevel::kFull);
    CHECK_FALSE(l.Fresh());
    l.EndFrame(SkipLevel::kFull, 0);
    CHECK_FALSE(l.Fresh());
    l.EndFrame(SkipLevel::kFull, 0);
    CHECK(l.Fresh());
}

TEST_CASE("whole frames asked for under swap_only are drawn, then swap_only goes on") {
    SkipLatch l;
    l.EndFrame(SkipLevel::kSwapOnly, 0);
    l.EndFrame(SkipLevel::kSwapOnly, 0);
    CHECK_FALSE(l.Fresh());
    CHECK(l.EndFrame(SkipLevel::kSwapOnly, 3) == SkipLevel::kFull);
    CHECK(l.EndFrame(SkipLevel::kSwapOnly, 0) == SkipLevel::kFull);
    CHECK_FALSE(l.Fresh());
    CHECK(l.EndFrame(SkipLevel::kSwapOnly, 0) == SkipLevel::kFull);
    CHECK(l.Fresh());  // two whole frames swapped: a capture can hold this one
    CHECK(l.EndFrame(SkipLevel::kSwapOnly, 0) == SkipLevel::kSwapOnly);
    CHECK(l.Fresh());
    l.EndFrame(SkipLevel::kSwapOnly, 0);
    CHECK_FALSE(l.Fresh());
}

TEST_CASE("the bool EndFrame is skip_draws") {
    SkipLatch a, b;
    for (bool want : {true, true, false, true, false, false, false}) {
        const bool skipped = a.EndFrame(want, 0);
        const SkipLevel level = b.EndFrame(want ? SkipLevel::kSkipDraws : SkipLevel::kFull, 0);
        CHECK(skipped == (level != SkipLevel::kFull));
        CHECK(a.Level() == level);
        CHECK(a.Level() == (want ? SkipLevel::kSkipDraws : SkipLevel::kFull));
        CHECK(a.Fresh() == b.Fresh());
        CHECK(a.WholeFrames() == b.WholeFrames());
    }
}

TEST_CASE("nothing is skipped unless renderer is native with the frame recorded") {
    for (SkipLevel setting : {SkipLevel::kFull, SkipLevel::kSkipDraws, SkipLevel::kSwapOnly}) {
        CAPTURE(int(setting));
        CHECK(WantedLevel(true, setting, true, true) == setting);
        // renderer emulated, capture off or texture passes not recorded: one
        // of the three bits clear (7 is all set)
        for (int on = 0; on < 7; on++) {
            CHECK(WantedLevel((on & 1) != 0, setting, (on & 2) != 0, (on & 4) != 0) ==
                  SkipLevel::kFull);
        }
    }
}

TEST_CASE("which emitters write their packets at each level") {
    using G = GpuSkipStats;
    for (int kind = 0; kind < G::kNumKinds; kind++) {
        for (int flags = 0; flags < 4; flags++) {
            const bool pass = flags & 1, point_tests = flags & 2;
            CAPTURE(G::kKindNames[kind]);
            CAPTURE(pass);
            CAPTURE(point_tests);
            // full: everything
            CHECK(EmitDraw(SkipLevel::kFull, kind, pass, point_tests));
            // swap_only: only BeginIndexedVertices, whose caller writes
            // through what it returns
            CHECK(EmitDraw(SkipLevel::kSwapOnly, kind, pass, point_tests) ==
                  (kind == G::kBeginIndexed));
            // skip_draws: clears and resolves too, and the draws a one-shot
            // pass wants or the flares' occlusion tests make (quads only)
            bool skip_draws = false;
            switch (kind) {
                case G::kBeginIndexed:
                case G::kClear:
                case G::kResolve: skip_draws = true; break;
                case G::kIndexed:
                case G::kInstanced: skip_draws = pass; break;
                case G::kUp: skip_draws = pass || point_tests; break;
            }
            CHECK(EmitDraw(SkipLevel::kSkipDraws, kind, pass, point_tests) == skip_draws);
        }
    }
}
