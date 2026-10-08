#include "src/Video/picture_convert.h"

#include <algorithm>
#include <cmath>

namespace band3::video {

Fit ParseFit(std::string_view name) {
    if (name == "fill") return Fit::kFill;
    if (name == "stretch") return Fit::kStretch;
    return Fit::kFit;
}

FitRects ComputeFit(float picture_aspect, float screen_aspect, Fit fit) {
    FitRects r = {{0, 0, 1, 1}, {0, 0, 1, 1}};
    if (fit == Fit::kStretch || !(picture_aspect > 0) || !(screen_aspect > 0)) return r;
    const bool wider = picture_aspect > screen_aspect;
    // the shorter side's share: of the screen for fit, of the picture for fill
    const float share = wider ? screen_aspect / picture_aspect : picture_aspect / screen_aspect;
    float* rect = fit == Fit::kFit ? r.dst : r.src;
    // fit's bars go above and below a wider picture; fill cuts a wider one's sides
    const bool across = (fit == Fit::kFit) != wider;
    const float margin = (1.0f - share) * 0.5f;
    rect[across ? 0 : 1] = margin;
    rect[across ? 2 : 3] = 1.0f - margin;
    return r;
}

namespace {

// The taps along an axis of `out` texels, of which [d0, d1) show source
// texels [s0, s1) of `in`: a tent as wide as the scale, at least one texel,
// texels past an edge counting as the edge's. Each output texel has `n` taps
// from `first`, zero-weighted past its own, so the loops have no branches.
struct Axis {
    uint32_t n = 0;
    std::vector<uint32_t> first;
    std::vector<float> weights;  // n per output texel, summing to 1
    std::vector<uint8_t> inside;  // 0 outside the picture: black there
};

Axis MakeAxis(uint32_t out, float d0, float d1, uint32_t in, float s0, float s1) {
    Axis a;
    a.first.assign(out, 0);
    a.inside.assign(out, 0);
    if (!in || d1 <= d0) return a;
    const float scale = (s1 - s0) / (d1 - d0);
    const float radius = std::max(1.0f, scale);
    a.n = std::min(in, uint32_t(std::ceil(2.0f * radius)) + 2);
    a.weights.assign(size_t(out) * a.n, 0.0f);
    for (uint32_t i = 0; i < out; i++) {
        const float c = float(i) + 0.5f;
        if (c < d0 || c >= d1) continue;
        a.inside[i] = 1;
        const float u = s0 + (c - d0) * scale;
        const int lo = int(std::floor(u - radius)), hi = int(std::ceil(u + radius));
        // the window, moved inside the picture whole
        const int first = std::clamp(lo, 0, int(in - a.n));
        a.first[i] = uint32_t(first);
        float* w = a.weights.data() + size_t(i) * a.n;
        float sum = 0;
        for (int k = lo; k <= hi; k++) {
            const float wt = std::max(0.0f, 1.0f - std::abs(float(k) + 0.5f - u) / radius);
            const int at = std::clamp(std::clamp(k, 0, int(in) - 1) - first, 0, int(a.n) - 1);
            w[at] += wt;
            sum += wt;
        }
        if (sum > 0) {
            for (uint32_t k = 0; k < a.n; k++) w[k] /= sum;
        } else {
            w[std::clamp(int(u) - first, 0, int(a.n) - 1)] = 1.0f;
        }
    }
    return a;
}

// scratch kept between frames on the decoder's thread
struct Scratch {
    std::vector<float> ring;  // unpacked source rows, R G B floats
    std::vector<int> ring_row;
    std::vector<float> column;  // one output row's source row, filtered down
    std::vector<float> rgb;     // the result
};

// `frame` resampled to w x h as R G B floats (0..255), black outside the
// picture, into s.rgb: down first, each source row unpacked once into a ring
// as the output rows move down it, then across.
void Resample(const RgbFrame& frame, const FitRects& fit, uint32_t w, uint32_t h, Scratch& s) {
    s.rgb.assign(size_t(w) * h * 3, 0.0f);
    const uint32_t sw = frame.width, sh = frame.height;
    if (!sw || !sh || frame.pixels.size() < size_t(sw) * sh) return;
    const Axis xs = MakeAxis(w, fit.dst[0] * w, fit.dst[2] * w, sw, fit.src[0] * sw,
                             fit.src[2] * sw);
    const Axis ys = MakeAxis(h, fit.dst[1] * h, fit.dst[3] * h, sh, fit.src[1] * sh,
                             fit.src[3] * sh);
    if (!xs.n || !ys.n) return;
    // an output row's window and the next's overlap: ring rows enough for two
    const uint32_t ring = ys.n * 2;
    s.ring.resize(size_t(ring) * sw * 3);
    s.ring_row.assign(ring, -1);
    s.column.resize(size_t(sw) * 3);
    auto source_row = [&](uint32_t r) -> const float* {
        const uint32_t slot = r % ring;
        float* dst = s.ring.data() + size_t(slot) * sw * 3;
        if (s.ring_row[slot] != int(r)) {
            const uint32_t* src = frame.pixels.data() + size_t(r) * sw;
            for (uint32_t x = 0; x < sw; x++) {
                const uint32_t p = src[x];
                dst[x * 3] = float((p >> 16) & 0xff);
                dst[x * 3 + 1] = float((p >> 8) & 0xff);
                dst[x * 3 + 2] = float(p & 0xff);
            }
            s.ring_row[slot] = int(r);
        }
        return dst;
    };
    float* column = s.column.data();
    const size_t row_floats = size_t(sw) * 3;
    for (uint32_t y = 0; y < h; y++) {
        if (!ys.inside[y]) continue;
        const float* wy = ys.weights.data() + size_t(y) * ys.n;
        std::fill(column, column + row_floats, 0.0f);
        for (uint32_t k = 0; k < ys.n; k++) {
            const float wt = wy[k];
            if (wt == 0.0f) continue;
            const float* in = source_row(ys.first[y] + k);
            for (size_t i = 0; i < row_floats; i++) column[i] += wt * in[i];
        }
        float* out = s.rgb.data() + size_t(y) * w * 3;
        for (uint32_t x = 0; x < w; x++) {
            if (!xs.inside[x]) continue;
            const float* wx = xs.weights.data() + size_t(x) * xs.n;
            const float* in = column + size_t(xs.first[x]) * 3;
            float r = 0, g = 0, b = 0;
            for (uint32_t k = 0; k < xs.n; k++) {
                r += wx[k] * in[k * 3];
                g += wx[k] * in[k * 3 + 1];
                b += wx[k] * in[k * 3 + 2];
            }
            out[x * 3] = r;
            out[x * 3 + 1] = g;
            out[x * 3 + 2] = b;
        }
    }
}

}

void ToPlanes(const RgbFrame& frame, Fit fit, float screen_aspect, uint32_t w, uint32_t h,
              uint32_t cw, uint32_t ch, PlaneSet& out) {
    const float picture_aspect =
        frame.height ? float(frame.width) * frame.pixel_aspect / float(frame.height) : 0.0f;
    const FitRects rects = ComputeFit(picture_aspect, screen_aspect, fit);
    thread_local Scratch s;
    Resample(frame, rects, w, h, s);
    out.y.Resize(w, h, kBlackY);
    const float* rgb = s.rgb.data();
    for (size_t i = 0; i < size_t(w) * h; i++)
        out.y.texels[i] = RgbToY(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
    out.cr.Resize(cw, ch, kNeutralC);
    out.cb.Resize(cw, ch, kNeutralC);
    // half size, as Bink's are: each 2x2's average, sited at its middle
    if (cw * 2 == w && ch * 2 == h) {
        for (uint32_t y = 0; y < ch; y++) {
            const float* a = rgb + size_t(y * 2) * w * 3;
            const float* b = a + size_t(w) * 3;
            for (uint32_t x = 0; x < cw; x++) {
                float c[3];
                for (int i = 0; i < 3; i++)
                    c[i] = 0.25f * (a[x * 6 + i] + a[x * 6 + 3 + i] + b[x * 6 + i] +
                                    b[x * 6 + 3 + i]);
                out.cr.Row(y)[x] = RgbToCr(c[0], c[1], c[2]);
                out.cb.Row(y)[x] = RgbToCb(c[0], c[1], c[2]);
            }
        }
        return;
    }
    Resample(frame, rects, cw, ch, s);
    rgb = s.rgb.data();
    for (size_t i = 0; i < size_t(cw) * ch; i++) {
        out.cr.texels[i] = RgbToCr(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
        out.cb.texels[i] = RgbToCb(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
    }
}

Plane ResizeNearest(const Plane& src, uint32_t w, uint32_t h) {
    Plane out;
    out.Resize(w, h, 0);
    if (!src.width || !src.height) return out;
    // from texel centres
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* row = src.Row(uint32_t((uint64_t(y) * 2 + 1) * src.height / (2 * h)));
        uint8_t* dst = out.Row(y);
        for (uint32_t x = 0; x < w; x++) dst[x] = row[(uint64_t(x) * 2 + 1) * src.width / (2 * w)];
    }
    return out;
}

}
