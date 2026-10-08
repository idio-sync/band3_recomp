#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "src/Video/movie_planes.h"

// A decoded video picture turned into a movie frame's planes (movie_planes.h)
// for a video venue's background: scaled to the planes' size, which the game
// stretches over the whole screen, so the picture keeps its own shape on the
// screen, not in the planes. Kept apart from the decoder for unit tests.

namespace band3::video {

// a picture as the decoder gives it: rows of 0xAARRGGBB (B first in memory,
// Media Foundation's RGB32), top row first
struct RgbFrame {
    uint32_t width = 0, height = 0;
    // a pixel's width over its height (anamorphic video isn't 1)
    float pixel_aspect = 1.0f;
    // seconds into the video
    double time = 0.0;
    std::vector<uint32_t> pixels;
};

// how a picture whose shape isn't the screen's goes on it
enum class Fit {
    kFit,      // all of it, with black bars
    kFill,     // the whole screen, its edges cut off
    kStretch,  // the whole screen and all of it, out of shape
};

// "fit", "fill" or "stretch"; anything else is fit
Fit ParseFit(std::string_view name);

// The screen's part a picture covers and the picture's part it shows there,
// each 0..1 across and down.
struct FitRects {
    float dst[4];  // x0, y0, x1, y1 on the screen
    float src[4];  // x0, y0, x1, y1 of the picture
};
// for a picture of shape `picture_aspect` (its width over its height, as
// shown) on a screen of shape `screen_aspect`
FitRects ComputeFit(float picture_aspect, float screen_aspect, Fit fit);

// Converts `frame` into planes of these sizes (Y w x h, chroma cw x ch), for
// a screen of shape `screen_aspect` that shows them stretched over it whole:
// a tent filter as wide as the scale (bilinear enlarging, averaging
// shrinking), then BT.601 with offsets, the inverse of shade.hlsli's
// MovieRgb. What the picture doesn't cover is black.
void ToPlanes(const RgbFrame& frame, Fit fit, float screen_aspect, uint32_t w, uint32_t h,
              uint32_t cw, uint32_t ch, PlaneSet& out);

// a plane at another size, nearest texel: for a frame made for planes that
// have since changed size, until the next one is made for theirs
Plane ResizeNearest(const Plane& src, uint32_t w, uint32_t h);

// BT.601 with offsets, from 0..255 R G B
inline uint8_t RgbToY(float r, float g, float b) {
    return uint8_t(16.5f + (65.481f * r + 128.553f * g + 24.966f * b) / 255.0f);
}
inline uint8_t RgbToCb(float r, float g, float b) {
    return uint8_t(128.5f + (-37.797f * r - 74.203f * g + 112.0f * b) / 255.0f);
}
inline uint8_t RgbToCr(float r, float g, float b) {
    return uint8_t(128.5f + (112.0f * r - 93.786f * g - 18.214f * b) / 255.0f);
}

}
