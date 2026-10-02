// Checks src/Render/present_model.h: where the native picture goes in the
// window (letterboxed to 16:9 as the SDK's presenter does, or stretched), and
// which output texture the native renderer's worker may draw into while the
// SDK's paints sample another.

#include <doctest/doctest.h>
#include <utility>
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
