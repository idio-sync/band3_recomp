// Checks src/Render/gpu_timing_model.h: the native renderer's GPU timestamps
// as a ladder of parts, and their ticks into milliseconds by part; and
// draw_profile.h's sums of them by mesh and material.

#include <doctest/doctest.h>
#include <cstdint>
#include <string>
#include <vector>
#include "src/Render/draw_profile.h"
#include "src/Render/gpu_timing_model.h"

using namespace band3::render::gpu_timing;

namespace {

// `ladder`'s labels with `ticks`, at `frequency`
Times Timed(const Ladder& ladder, const std::vector<uint64_t>& ticks, uint64_t frequency) {
    REQUIRE(ticks.size() == ladder.Count());
    Times t;
    Accumulate(ladder.Labels().data(), ticks.data(), ticks.size(), frequency, t);
    return t;
}

}  // namespace

TEST_CASE("a mark is a slot each time the part changes, none while it doesn't") {
    Ladder l(16);
    CHECK(l.Mark(kNone) == -1);  // nothing started: nothing to end
    CHECK(l.Mark(kUpload) == 0);
    CHECK(l.Mark(kWorld) == 1);
    CHECK(l.Mark(kWorld) == -1);
    CHECK(l.Open());
    CHECK(l.Mark(kNone) == 2);
    CHECK_FALSE(l.Open());
    CHECK(l.Mark(kNone) == -1);
    CHECK(l.Count() == 3);
    l.Reset();
    CHECK(l.Count() == 0);
    CHECK(l.Mark(kGamma) == 0);
}

TEST_CASE("ticks become milliseconds by part, and the parts but idle add up to the total") {
    Ladder l(16);
    l.Mark(kUpload);
    l.Mark(kWorld);
    l.Mark(kComposite);
    l.Mark(kOverlay);
    l.Mark(kNone);
    // 10 MHz: 1000 ticks a tenth of a millisecond
    const Times t = Timed(l, {1000, 2000, 52000, 62000, 82000}, 10'000'000);
    CHECK(t.ms[kUpload] == doctest::Approx(0.1));
    CHECK(t.ms[kWorld] == doctest::Approx(5.0));
    CHECK(t.ms[kComposite] == doctest::Approx(1.0));
    CHECK(t.ms[kOverlay] == doctest::Approx(2.0));
    CHECK(t.ms[kGamma] == 0);
    CHECK(t.total_ms == doctest::Approx(8.1));
    double sum = 0;
    for (double ms : t.ms) sum += ms;
    CHECK(sum == doctest::Approx(t.total_ms));
    CHECK(t.bad == 0);
}

TEST_CASE("a part met again adds to what it had") {
    Ladder l(16);
    l.Mark(kWorld);
    l.Mark(kPassOther);
    l.Mark(kWorld);
    l.Mark(kNone);
    const Times t = Timed(l, {100, 300, 400, 1000}, 1000);  // 1 kHz: a tick a millisecond
    CHECK(t.ms[kWorld] == doctest::Approx(800));
    CHECK(t.ms[kPassOther] == doctest::Approx(100));
}

TEST_CASE("the mips' command buffer is theirs, the waits around it idle and not in the total") {
    // the pass's buffer ends idle before it's submitted, the mips' starts
    // with kMips and ends idle, and the next goes back to the world: the
    // gaps between are the GPU waiting for the CPU's next submission
    Ladder l(16);
    l.Mark(kWorld);
    l.Mark(kPassOther);
    l.Mark(kIdle);
    l.Mark(kMips);
    l.Mark(kIdle);
    l.Mark(kWorld);
    l.Mark(kNone);
    const Times t = Timed(l, {10, 20, 50, 52, 55, 80, 83}, 1000);
    CHECK(t.ms[kWorld] == doctest::Approx(13));
    CHECK(t.ms[kPassOther] == doctest::Approx(30));
    CHECK(t.ms[kMips] == doctest::Approx(3));
    CHECK(t.ms[kIdle] == doctest::Approx(27));
    CHECK(t.total_ms == doctest::Approx(46));
}

TEST_CASE("a gap that starts at kNone is no part's") {
    // two world passes before a frame, each a Render of its own, with the CPU
    // between them
    Ladder l(16);
    l.Mark(kPreWorld);
    l.Mark(kNone);
    l.Mark(kPreWorld);
    l.Mark(kNone);
    const Times t = Timed(l, {10, 13, 40, 44}, 1000);
    CHECK(t.ms[kPreWorld] == doctest::Approx(7));
    CHECK(t.total_ms == doctest::Approx(7));
    CHECK(t.bad == 0);
}

TEST_CASE("spans the GPU didn't write, or that run backwards, are left out and counted") {
    Ladder l(16);
    l.Mark(kUpload);
    l.Mark(kWorld);
    l.Mark(kOverlay);
    l.Mark(kGamma);
    l.Mark(kNone);
    // the world's end unwritten (0) spoils the world's span and the overlay's;
    // the gamma's runs backwards
    const Times t = Timed(l, {10, 20, 0, 50, 40}, 1000);
    CHECK(t.ms[kUpload] == doctest::Approx(10));
    CHECK(t.ms[kWorld] == 0);
    CHECK(t.ms[kOverlay] == 0);
    CHECK(t.ms[kGamma] == 0);
    CHECK(t.bad == 3);
    CHECK(t.total_ms == doctest::Approx(10));
}

TEST_CASE("a span longer than any frame is garbage") {
    Ladder l(4);
    l.Mark(kWorld);
    l.Mark(kNone);
    // 20 s at 1 kHz
    const Times t = Timed(l, {1, 20001}, 1000);
    CHECK(t.ms[kWorld] == 0);
    CHECK(t.bad == 1);
}

TEST_CASE("no frequency, no times") {
    Ladder l(4);
    l.Mark(kWorld);
    l.Mark(kNone);
    const Times t = Timed(l, {1, 2}, 0);
    CHECK(t.total_ms == 0);
    CHECK(t.bad == 0);
}

TEST_CASE("a full ladder drops its marks, counts them and still ends") {
    Ladder l(4);
    CHECK(l.Mark(kUpload) == 0);
    CHECK(l.Mark(kWorld) == 1);
    CHECK(l.Mark(kPassOther) == 2);
    // the last slot is the end's
    CHECK(l.Mark(kMips) == -1);
    CHECK(l.Mark(kWorld) == -1);
    CHECK(l.Dropped() == 2);
    CHECK(l.Mark(kNone) == 3);
    CHECK(l.Count() == 4);
    // what didn't fit is charged to the last part that did
    const Times t = Timed(l, {1, 2, 4, 10}, 1000);
    CHECK(t.ms[kPassOther] == doctest::Approx(6));
    CHECK(t.ms[kMips] == 0);
    CHECK(t.total_ms == doctest::Approx(9));
    l.Reset();
    CHECK(l.Dropped() == 0);
}

TEST_CASE("every part has a name of its own") {
    for (int a = 0; a < kParts; a++) {
        CHECK(std::string(PartName(uint8_t(a))) != "none");
        for (int b = a + 1; b < kParts; b++)
            CHECK(std::string(PartName(uint8_t(a))) != PartName(uint8_t(b)));
    }
    CHECK(std::string(PartName(kNone)) == "none");
}

TEST_CASE("keyed marks time each draw on its own") {
    Ladder l(8);
    CHECK(l.Mark(kUpload) == 0);
    // the same part again, each its own span with its draw's key
    CHECK(l.MarkKeyed(kWorld, 0) == 1);
    CHECK(l.MarkKeyed(kWorld, 2) == 2);
    CHECK(l.MarkKeyed(kWorld, 0) == 3);
    CHECK(l.Mark(kComposite) == 4);
    CHECK(l.Mark(kNone) == 5);
    const std::vector<uint64_t> ticks = {1, 2, 5, 6, 10, 11};
    // the parts as before: the world 2 to 10
    const Times t = Timed(l, ticks, 1000);
    CHECK(t.ms[kWorld] == doctest::Approx(8));
    std::vector<double> by_key;
    AccumulateKeyed(l.Labels().data(), l.Keys().data(), ticks.data(), ticks.size(), 1000, by_key);
    REQUIRE(by_key.size() == 3);
    CHECK(by_key[0] == doctest::Approx(3 + 4));
    CHECK(by_key[1] == 0);
    CHECK(by_key[2] == doctest::Approx(1));
    // full: the end's slot stays free
    Ladder small(3);
    CHECK(small.MarkKeyed(kWorld, 0) == 0);
    CHECK(small.MarkKeyed(kWorld, 1) == 1);
    CHECK(small.MarkKeyed(kWorld, 2) == -1);
    CHECK(small.Dropped() == 1);
    CHECK(small.Mark(kNone) == 2);
}

TEST_CASE("the draw profile sums by mesh and material") {
    using band3::render::DrawProfile;
    using band3::render::DrawTime;
    DrawProfile p;
    DrawTime a{}, b{}, c{};
    a.tag.mesh = 1, a.tag.mat = 10, a.tag.tris = 100, a.ms = 2;
    b.tag.mesh = 1, b.tag.mat = 11, b.ms = 1;
    c.tag.mesh = 2, c.tag.mat = 10, c.ms = 0.5;
    p.AddFrame({a, b, c});
    a.ms = 3;
    p.AddFrame({a, c});
    CHECK(p.Frames() == 2);
    CHECK(p.Groups() == 3);
    CHECK(p.TotalMs() == doctest::Approx(7));
    const auto top = p.Top(2);
    REQUIRE(top.size() == 2);
    CHECK(top[0].tag.mesh == 1);
    CHECK(top[0].tag.mat == 10);
    CHECK(top[0].tag.tris == 100);
    CHECK(top[0].ms == doctest::Approx(5));
    CHECK(top[0].draws == 2);
    CHECK(top[1].ms == doctest::Approx(1));
    p.Reset();
    CHECK(p.Frames() == 0);
    CHECK(p.Top(5).empty());
}
