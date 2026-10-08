#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>

// The samples an occlusion query's draws pass when nothing is in front of
// them: the pixels they cover in their target. RB3's only queries are its lens
// flares' (DxRnd::DoPointTests, rb3-xenon rnddx9/Rnd_Xbox.cpp): a point and
// the flare's rect as a 4-vertex strip, by D3DDevice_DrawVerticesUP with the
// viewport off, so vertices are in the target's pixels, and HalfPixelOffset
// on, which on Xenos is D3D9's convention (PA_SU_VTX_CNTL pix_center 0;
// Xenia's draw_util.cc undoes it): pixel (i, j)'s centre is (i, j), so a
// point at whole (x, y) lands on one and a whole rect covers w x h. A pixel
// is covered when its centre is inside a triangle by the top-left rule (left
// and top edges in, right and bottom out, so a quad's two triangles share no
// pixel), or inside a point's 1x1 square (point size 1) the same way. One
// sample a pixel: the world target the tests draw into is 1x (soft_raster.h;
// rb3-xenon's back buffer is D3DMULTISAMPLE_NONE, only the overlay's 2x).
// query_answers.cpp answers the game's queries with these counts.

namespace band3::render {

// D3DPRIMITIVETYPE (Xbox 360) of the kinds counted
inline constexpr uint32_t kPrimPointList = 1;
inline constexpr uint32_t kPrimTriangleList = 4;
inline constexpr uint32_t kPrimTriangleFan = 5;
inline constexpr uint32_t kPrimTriangleStrip = 6;

// a vertex's position in the target's pixels
struct QueryVertex {
    float x = 0, y = 0;
};

// pixels i in 0..n-1 whose centre i is in [x0, x1)
inline uint64_t SpanPixels(double x0, double x1, uint32_t n) {
    if (!(x0 < x1)) return 0;  // NaN too
    const double lo = std::clamp(std::ceil(x0), 0.0, double(n));
    const double hi = std::clamp(std::ceil(x1), 0.0, double(n));
    return hi > lo ? uint64_t(hi - lo) : 0;
}

// an axis-aligned rect at (x, y), w x h, in a width x height target
inline uint64_t RectPixels(double x, double y, double w, double h, uint32_t width,
                           uint32_t height) {
    return SpanPixels(x, x + w, width) * SpanPixels(y, y + h, height);
}

inline uint64_t PointPixels(const QueryVertex& v, uint32_t width, uint32_t height) {
    return RectPixels(double(v.x) - 0.5, double(v.y) - 0.5, 1, 1, width, height);
}

// A triangle, either winding, row by row: each row whose centre y is in the
// triangle's [top, bottom) covers the pixels whose centres are in its span
// there, [left, right). Each edge's x at a row is computed from its endpoints
// in one order, so two triangles sharing it agree on where it is.
inline uint64_t TrianglePixels(const QueryVertex& a, const QueryVertex& b, const QueryVertex& c,
                               uint32_t width, uint32_t height) {
    const QueryVertex* v[3] = {&a, &b, &c};
    double top = v[0]->y, bottom = v[0]->y;
    for (const QueryVertex* p : v) {
        top = std::min(top, double(p->y));
        bottom = std::max(bottom, double(p->y));
    }
    if (!(top < bottom)) return 0;
    const double first = std::clamp(std::ceil(top), 0.0, double(height));
    const double end = std::clamp(std::ceil(bottom), 0.0, double(height));
    uint64_t pixels = 0;
    for (double row = first; row < end; row++) {
        const double yc = row;
        double left = 0, right = 0;
        int crossings = 0;
        for (int e = 0; e < 3; e++) {
            const QueryVertex* p = v[e];
            const QueryVertex* q = v[(e + 1) % 3];
            if (p->y == q->y) continue;  // a horizontal edge spans no row
            if (p->y > q->y) std::swap(p, q);
            // the top endpoint's row is the edge's, the bottom's isn't
            if (yc < p->y || yc >= q->y) continue;
            const double x = p->x + (yc - p->y) * (double(q->x) - p->x) / (double(q->y) - p->y);
            left = crossings ? std::min(left, x) : x;
            right = crossings ? std::max(right, x) : x;
            crossings++;
        }
        if (crossings >= 2) pixels += SpanPixels(left, right, width);
    }
    return pixels;
}

// The pixels a draw of `prim` covers, overlaps counted twice as the GPU
// counts them; nullopt for a primitive type not counted here (lines, rects,
// quads)
inline std::optional<uint64_t> DrawPixels(uint32_t prim, std::span<const QueryVertex> v,
                                          uint32_t width, uint32_t height) {
    uint64_t pixels = 0;
    switch (prim) {
        case kPrimPointList:
            for (const QueryVertex& p : v) pixels += PointPixels(p, width, height);
            return pixels;
        case kPrimTriangleList:
            for (size_t i = 0; i + 2 < v.size(); i += 3)
                pixels += TrianglePixels(v[i], v[i + 1], v[i + 2], width, height);
            return pixels;
        case kPrimTriangleStrip:
            for (size_t i = 0; i + 2 < v.size(); i++)
                pixels += TrianglePixels(v[i], v[i + 1], v[i + 2], width, height);
            return pixels;
        case kPrimTriangleFan:
            for (size_t i = 1; i + 1 < v.size(); i++)
                pixels += TrianglePixels(v[0], v[i], v[i + 1], width, height);
            return pixels;
        default: return std::nullopt;
    }
}

}  // namespace band3::render
