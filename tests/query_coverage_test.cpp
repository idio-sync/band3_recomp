// Checks src/Render/query_coverage.h, the samples an occlusion query's draws
// pass with nothing in front of them: a flare's rect drawn as RB3's
// DoPointTests draws it counts its pixels, cut to the target; edges count by
// pixel centres; a quad's two triangles share no pixel, whichever way it's
// split; any triangle counts what a test of every pixel centre counts; a
// point is one pixel.

#include <doctest/doctest.h>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>
#include "src/Render/query_coverage.h"

using namespace band3::render;

namespace {

constexpr uint32_t kW = 1280, kH = 720;

// DoPointTests' area strip: (x, y), (x, y + h), (x + w, y), (x + w, y + h)
std::vector<QueryVertex> AreaStrip(float x, float y, float w, float h) {
    return {{x, y}, {x, y + h}, {x + w, y}, {x + w, y + h}};
}

uint64_t Strip(const std::vector<QueryVertex>& v, uint32_t w = kW, uint32_t h = kH) {
    const std::optional<uint64_t> n = DrawPixels(kPrimTriangleStrip, v, w, h);
    REQUIRE(n.has_value());
    return *n;
}

// every pixel centre strictly inside: the reference for triangles whose edges
// pass through no centre
uint64_t BruteTriangle(const QueryVertex& a, const QueryVertex& b, const QueryVertex& c,
                       uint32_t w, uint32_t h) {
    auto edge = [](const QueryVertex& p, const QueryVertex& q, double x, double y) {
        return (double(q.x) - p.x) * (y - p.y) - (double(q.y) - p.y) * (x - p.x);
    };
    uint64_t n = 0;
    for (uint32_t j = 0; j < h; j++) {
        for (uint32_t i = 0; i < w; i++) {
            const double x = i, y = j;
            const double e0 = edge(a, b, x, y), e1 = edge(b, c, x, y), e2 = edge(c, a, x, y);
            if ((e0 > 0 && e1 > 0 && e2 > 0) || (e0 < 0 && e1 < 0 && e2 < 0)) n++;
        }
    }
    return n;
}

// deterministic coordinates with fractions that land on no pixel centre
struct Lcg {
    uint32_t s = 12345;
    float Next(float lo, float hi) {
        s = s * 1664525u + 1013904223u;
        return lo + (hi - lo) * float(s >> 8) / float(1 << 24);
    }
};

}  // namespace

TEST_CASE("a flare's area strip counts its pixels, cut to the target") {
    // a 128x128 lamp flare in the open: the whole rect, what the area test
    // divides by
    CHECK(Strip(AreaStrip(100, 50, 128, 128)) == 128 * 128);
    CHECK(RectPixels(100, 50, 128, 128, kW, kH) == 128 * 128);
    // past the left edge, past the bottom right corner, wholly off
    CHECK(Strip(AreaStrip(-32, 50, 128, 128)) == 96 * 128);
    CHECK(Strip(AreaStrip(1250, 700, 128, 128)) == 30 * 20);
    CHECK(Strip(AreaStrip(1300, 50, 128, 128)) == 0);
    CHECK(Strip(AreaStrip(-200, -200, 128, 128)) == 0);
    // a hub flare over the whole screen
    CHECK(Strip(AreaStrip(-400, -300, 2000, 1400)) == uint64_t(kW) * kH);
    // empty and inverted rects
    CHECK(Strip(AreaStrip(10, 10, 0, 64)) == 0);
    CHECK(RectPixels(10, 10, -5, 64, kW, kH) == 0);
}

TEST_CASE("edges count the pixels whose centres they hold") {
    // centres 11 .. 14 lie in [10.3, 14.7)
    CHECK(SpanPixels(10.3, 14.7, kW) == 4);
    // the left edge holds a centre on it, the right doesn't
    CHECK(SpanPixels(10, 11, kW) == 1);
    CHECK(SpanPixels(10.1, 10.9, kW) == 0);
    // columns 11..14, rows 21 and 22
    CHECK(Strip(AreaStrip(10.3f, 20.5f, 4.4f, 2.0f)) == 4 * 2);
    // NaN counts nothing
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(Strip(AreaStrip(nan, 0, 16, 16)) == 0);
    CHECK(SpanPixels(nan, 4, kW) == 0);
}

TEST_CASE("a quad's two triangles share no pixel, whichever way it's split") {
    Lcg r;
    for (int n = 0; n < 500; n++) {
        const float x = r.Next(-50, 1300), y = r.Next(-50, 760);
        const float w = r.Next(0, 300), h = r.Next(0, 300);
        CAPTURE(x);
        CAPTURE(y);
        CAPTURE(w);
        CAPTURE(h);
        // the vertices' edges, rounded to float as theirs are
        const float x1 = x + w, y1 = y + h;
        const uint64_t want = SpanPixels(x, x1, kW) * SpanPixels(y, y1, kH);
        // DoPointTests' strip splits along (x, y + h)-(x + w, y)
        CHECK(Strip(AreaStrip(x, y, w, h)) == want);
        // the other diagonal, as a list and as a fan
        const std::vector<QueryVertex> list = {{x, y},         {x + w, y + h}, {x + w, y},
                                               {x, y},         {x, y + h},     {x + w, y + h}};
        CHECK(DrawPixels(kPrimTriangleList, list, kW, kH) == want);
        const std::vector<QueryVertex> fan = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
        CHECK(DrawPixels(kPrimTriangleFan, fan, kW, kH) == want);
    }
}

TEST_CASE("a triangle counts the pixel centres inside it, cut to the target") {
    constexpr uint32_t w = 64, h = 48;
    Lcg r;
    for (int n = 0; n < 300; n++) {
        const QueryVertex a{r.Next(-20, 84), r.Next(-20, 68)};
        const QueryVertex b{r.Next(-20, 84), r.Next(-20, 68)};
        const QueryVertex c{r.Next(-20, 84), r.Next(-20, 68)};
        CAPTURE(n);
        CHECK(TrianglePixels(a, b, c, w, h) == BruteTriangle(a, b, c, w, h));
        // winding doesn't matter
        CHECK(TrianglePixels(a, c, b, w, h) == TrianglePixels(a, b, c, w, h));
    }
}

TEST_CASE("a point test covers one pixel, none off the target") {
    const std::vector<QueryVertex> centre = {{640, 360}};
    CHECK(DrawPixels(kPrimPointList, centre, kW, kH) == 1);
    // its 1x1 square about the vertex: DoPointTests' whole (x, y), the
    // screen's corners included, is the pixel at (x, y)
    CHECK(PointPixels({0, 0}, kW, kH) == 1);
    CHECK(PointPixels({1279, 719}, kW, kH) == 1);
    // [0.1, 1.1) holds the centre 1
    CHECK(PointPixels({0.6f, 0.6f}, kW, kH) == 1);
    CHECK(PointPixels({-3, 5}, kW, kH) == 0);
    CHECK(PointPixels({1280, 5}, kW, kH) == 0);
    const std::vector<QueryVertex> two = {{100, 100}, {200, 100}};
    CHECK(DrawPixels(kPrimPointList, two, kW, kH) == 2);
}

TEST_CASE("primitive types not counted have no answer") {
    const std::vector<QueryVertex> v = {{0, 0}, {10, 10}};
    CHECK_FALSE(DrawPixels(2, v, kW, kH).has_value());  // a line list
    CHECK_FALSE(DrawPixels(8, v, kW, kH).has_value());  // a rect list
    // too few vertices for a triangle count nothing
    CHECK(DrawPixels(kPrimTriangleStrip, v, kW, kH) == 0);
}
