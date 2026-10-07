// Checks the UI thread round trips' buckets (src/Input/ui_round_trip.h): a
// percentile is the top of the quarter millisecond it falls in, never more
// than the longest trip, and trips over 64 ms still count.

#include <doctest/doctest.h>
#include <limits>
#include "src/Input/ui_round_trip.h"

using band3::input::UiRoundTripHistogram;

TEST_CASE("no trips read as 0") {
    UiRoundTripHistogram h;
    CHECK(h.Count() == 0);
    CHECK(h.Percentile(0.5) == 0);
    CHECK(h.Max() == 0);
}

TEST_CASE("a percentile is the top of its bucket, no more than the longest trip") {
    UiRoundTripHistogram h;
    // 90 quick trips (0.1 ms) and 10 that waited for a vertical blank
    for (int i = 0; i < 90; i++) h.Add(0.1);
    for (int i = 0; i < 10; i++) h.Add(16.6);
    CHECK(h.Count() == 100);
    // the quick ones' bucket is 0-0.25 ms
    CHECK(h.Percentile(0.5) == doctest::Approx(0.25));
    CHECK(h.Percentile(0.9) == doctest::Approx(0.25));
    // the 91st: the slow ones' bucket (16.5-16.75 ms) tops out above the
    // longest, which it reads as
    CHECK(h.Percentile(0.95) == doctest::Approx(16.6));
    CHECK(h.Max() == doctest::Approx(16.6));
}

TEST_CASE("trips spread evenly give their percentiles to a quarter millisecond") {
    UiRoundTripHistogram h;
    // 0-16.7 ms, as a pump that waits for the next of 60 vertical blanks
    for (int i = 0; i < 1000; i++) h.Add(16.7 * (i + 0.5) / 1000);
    // the 500th is 8.34 ms, the 950th 15.86
    CHECK(h.Percentile(0.5) == doctest::Approx(8.5));
    CHECK(h.Percentile(0.95) == doctest::Approx(16.0));
    CHECK(h.Max() == doctest::Approx(16.7 * 0.9995));
}

TEST_CASE("trips over 64 ms count, and read as the longest") {
    UiRoundTripHistogram h;
    h.Add(1.0);
    h.Add(200.0);
    h.Add(90.0);
    CHECK(h.Count() == 3);
    CHECK(h.Percentile(0.3) == doctest::Approx(1.25));
    CHECK(h.Percentile(0.5) == doctest::Approx(200.0));
    CHECK(h.Max() == doctest::Approx(200.0));
}

TEST_CASE("a negative or not-a-number time counts as 0, and Reset starts over") {
    UiRoundTripHistogram h;
    h.Add(-1.0);
    h.Add(std::numeric_limits<double>::quiet_NaN());
    CHECK(h.Count() == 2);
    CHECK(h.Max() == 0);
    CHECK(h.Percentile(1.0) == 0);
    h.Reset();
    CHECK(h.Count() == 0);
    h.Add(3.1);
    CHECK(h.Percentile(0.5) == doctest::Approx(3.1));
}
