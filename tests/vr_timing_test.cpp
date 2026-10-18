// Checks src/Render/vr_timing.h, VR's frame timing: what a frame's times say
// of it (late as the xr block counts it, over the period, a missed display
// slot, a late wake), the ring of the last frames, the percentiles of each
// part for late and on-time frames, the CSV and the log's breakdown.

#include <doctest/doctest.h>
#include <string>
#include <vector>
#include "src/Render/vr_timing.h"

using namespace band3::render::vr;

namespace {

constexpr int64_t kPeriodNs = 8333333;  // 120 Hz

// frame `index`, woken at `wake_ms`, for the display at `display_ms`; located
// 0.3 ms after waking, ended `ended_ms` after
FrameTiming Frame(uint64_t index, double wake_ms, double display_ms, double ended_ms) {
    FrameTiming t;
    t.index = index;
    t.wake_ns = int64_t(wake_ms * 1e6);
    t.display_time_ns = int64_t(display_ms * 1e6);
    t.period_ns = kPeriodNs;
    t.located_ms = 0.3;
    t.ended_ms = ended_ms;
    return t;
}

bool Has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("a frame is late as the xr block counts it: located to xrEndFrame over the period") {
    FrameTiming prev = Frame(1, 0, 20, 5);
    Classify(prev, nullptr);
    CHECK_FALSE(prev.late);
    CHECK_FALSE(Has(prev.wake_step_ms));  // the first has no step
    CHECK_FALSE(Has(prev.display_step_ms));

    // 8.5 ms from waking, 8.2 of it after locating: within the period
    FrameTiming t = Frame(2, 8.333, 28.333, 8.5);
    Classify(t, &prev);
    CHECK_FALSE(t.late);
    CHECK(t.over_period);
    CHECK(t.wake_step_ms == doctest::Approx(8.333));
    CHECK(t.display_step_ms == doctest::Approx(8.333));
    CHECK_FALSE(t.missed);
    CHECK_FALSE(t.late_wake);
    CHECK_FALSE(t.slow);

    t.ended_ms = 9.0;  // 8.7 after locating
    Classify(t, &prev);
    CHECK(t.late);
}

TEST_CASE("a display step of 1.5 periods or more is a missed slot, counted in refreshes") {
    FrameTiming prev = Frame(1, 0, 20, 4);
    FrameTiming t = Frame(2, 16.667, 36.667, 4);
    Classify(t, &prev);
    CHECK(t.missed);
    CHECK(t.slots_missed == 1);
    CHECK(t.late_wake);
    CHECK(t.slow);  // logged for the step though its own CPU time was short

    FrameTiming three = Frame(2, 8.4, 20 + 4 * 8.333, 4);
    Classify(three, &prev);
    CHECK(three.slots_missed == 3);
    CHECK_FALSE(three.late_wake);

    FrameTiming slow = Frame(2, 8.4, 28.333, 12.6);
    Classify(slow, &prev);
    CHECK_FALSE(slow.missed);
    CHECK(slow.slow);
}

TEST_CASE("without a period nothing is late") {
    FrameTiming t = Frame(1, 0, 0, 50);
    t.period_ns = 0;
    Classify(t, nullptr);
    CHECK_FALSE(t.late);
    CHECK_FALSE(t.over_period);
    CHECK_FALSE(t.slow);
}

TEST_CASE("the ring keeps the last frames, oldest first, from an index on") {
    FrameRing ring(4);
    for (uint64_t i = 1; i <= 6; i++) ring.Push(Frame(i, double(i), 0, 1));
    CHECK(ring.Size() == 4);
    std::vector<FrameTiming> all = ring.Since(0);
    REQUIRE(all.size() == 4);
    CHECK(all[0].index == 3);
    CHECK(all[3].index == 6);
    std::vector<FrameTiming> later = ring.Since(4);
    REQUIRE(later.size() == 2);
    CHECK(later[0].index == 5);
    CHECK(ring.Since(6).empty());

    FrameRing part(4);
    part.Push(Frame(1, 0, 0, 1));
    part.Push(Frame(2, 0, 0, 1));
    REQUIRE(part.Since(0).size() == 2);
    CHECK(part.Since(0)[1].index == 2);
}

TEST_CASE("percentiles are by nearest rank and leave out steps that didn't happen") {
    std::vector<double> v;
    for (int i = 100; i >= 1; i--) v.push_back(i);
    v.push_back(kNone);
    const Spread s = SpreadOf(v);
    CHECK(s.n == 100);
    CHECK(s.p50 == 50);
    CHECK(s.p95 == 95);
    CHECK(s.p99 == 99);
    CHECK(s.max == 100);
    CHECK(SpreadOf({kNone}).n == 0);
    const Spread one = SpreadOf({3.5});
    CHECK(one.p50 == 3.5);
    CHECK(one.p99 == 3.5);
}

TEST_CASE("the timing summary splits late frames from the rest, part by part") {
    std::vector<FrameTiming> frames;
    FrameTiming prev;
    for (uint64_t i = 1; i <= 10; i++) {
        // every fifth frame spends 6 ms in xrEndFrame and runs late
        const bool slow = i % 5 == 0;
        FrameTiming t = Frame(i, double(i) * 8.333, 20 + double(i) * 8.333, slow ? 10 : 5);
        t.end_frame_ms = slow ? 6 : 1;
        t.capture = 100 + i / 2;
        t.repeat = i % 2 == 1;
        t.hud = i == 4;
        t.hud_cpu_ms = 0.25;
        Classify(t, i > 1 ? &prev : nullptr);
        prev = t;
        frames.push_back(t);
    }
    const std::string json = TimingJson(frames);
    CHECK(Has(json, "{\"frames\":10,\"late\":{\"frames\":2,\"repeats\":1,"));
    CHECK(Has(json, "\"on_time\":{\"frames\":8,\"repeats\":4,"));
    // late frames' xrEndFrame against the others'
    CHECK(Has(json, "\"end_frame_ms\":{\"n\":2,\"p50\":6.000,\"p95\":6.000,\"p99\":6.000,"
                    "\"max\":6.000}"));
    CHECK(Has(json, "\"end_frame_ms\":{\"n\":8,\"p50\":1.000,"));
    // the panel's time only for the frame that drew it; no GPU timestamps, no eye_gpu_ms
    CHECK(Has(json, "\"hud_cpu_ms\":{\"n\":1,"));
    CHECK_FALSE(Has(json, "eye_gpu_ms"));
    // the first frame has no step
    CHECK(Has(json, "\"wake_step_ms\":{\"n\":7,"));
}

TEST_CASE("the CSV has a column for every field, empty where a step didn't happen") {
    FrameTiming t = Frame(7, 1000, 1020, 5.25);
    t.capture = 42;
    t.eye_submitted_ms[0] = 1.5;
    Classify(t, nullptr);
    const std::string header = CsvHeader();
    const std::string row = CsvRow(t);
    auto columns = [](const std::string& s) {
        size_t n = 1;
        for (char c : s) n += c == ',';
        return n;
    };
    CHECK(columns(header) == columns(row));
    CHECK(header.back() == '\n');
    CHECK(row.back() == '\n');
    CHECK(row.starts_with("7,1.000000,8.333,0,,,"));
    CHECK(Has(row, ",42,"));
    CHECK(Has(row, ",1.500,,"));  // the right eye never submitted
}

TEST_CASE("a slow frame's breakdown names where its time went") {
    FrameTiming prev = Frame(1, 0, 20, 4);
    FrameTiming t = Frame(2, 16.667, 36.667, 13.2);
    t.wait_ms = 3.1;
    t.capture = 55;
    t.repeat = true;
    t.eye_submitted_ms[0] = 1.2;
    t.eye_submitted_ms[1] = 2.4;
    t.end_frame_ms = 7.5;
    t.present_ms = 5.0;
    t.newer = 1;
    t.window = true;
    t.window_ms = 0.4;
    Classify(t, &prev);
    const std::string line = Breakdown(t);
    CHECK(line.starts_with("frame 2: ended at 13.20 ms of a 8.33 ms period"));
    CHECK(Has(line, "display +16.67 ms (1 missed)"));
    CHECK(Has(line, "capture 55 (repeat)"));
    CHECK(Has(line, "eyes submitted 1.20/2.40"));
    CHECK(Has(line, "present at 5.00"));
    CHECK(Has(line, "xrEndFrame 7.50"));
    CHECK(Has(line, "1 newer game frames"));
    CHECK(Has(line, "window 0.40"));
}

TEST_CASE("the refresh is the shortest period of late, not a throttled frame's own") {
    RefreshPeriod r;
    CHECK(r.Ns() == 0);
    CHECK(r.Add(2 * kPeriodNs) == 2 * kPeriodNs);  // nothing shorter seen yet
    CHECK(r.Add(kPeriodNs) == kPeriodNs);
    CHECK(r.Add(2 * kPeriodNs) == kPeriodNs);  // throttled: still the refresh
    CHECK(r.Add(0) == kPeriodNs);              // no period: ignored
    // a refresh rate changed for good: the old one leaves after the window
    for (size_t i = 0; i < RefreshPeriod::kWindow; i++) r.Add(11111111);
    CHECK(r.Ns() == 11111111);
}

TEST_CASE("a throttled frame's two-refresh step is missed refreshes against the refresh") {
    // SteamVR throttling: the period predicted twice the refresh, display
    // times two refreshes apart
    FrameTiming prev = Frame(1, 0, 20, 4);
    prev.refresh_ns = kPeriodNs;
    Classify(prev, nullptr);
    FrameTiming t = Frame(2, 16.667, 36.667, 4);
    t.period_ns = 2 * kPeriodNs;
    t.refresh_ns = kPeriodNs;
    Classify(t, &prev);
    CHECK(t.throttled);
    CHECK(t.missed);
    CHECK(t.slots_missed == 1);
    // judged by its own period it looked on time, as before
    t.refresh_ns = 0;
    Classify(t, &prev);
    CHECK_FALSE(t.missed);
    CHECK_FALSE(t.throttled);
    // the CSV's last columns: the refresh, throttled, the queue wait
    t.refresh_ns = kPeriodNs;
    t.queue_wait = true;
    t.done_wait_ms = 0.25;
    Classify(t, &prev);
    CHECK(Has(CsvRow(t), ",8.333,1,1,0.250\n"));
}