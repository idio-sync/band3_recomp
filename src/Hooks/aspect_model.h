#pragma once

// The game's cameras at the window's shape (native_fill_window): the numbers
// src/Hooks/aspect.cpp's hooks give the game, apart from it so the unit
// tests reach them.
//
// RndCam::UpdateLocal (rb3-xenon rndobj/Cam.cpp) builds a camera's
// projection from TheRnd->YRatio(), height over width, RB3's 9/16: x scale
// YRatio / tan(fov/2), y scale 1 / tan(fov/2). The vertical field of view is
// the camera's and the horizontal one follows YRatio, and the frustum the
// game culls with is made from the same two. So a YRatio of the window's
// height over width shows more to the sides of a wider window (Hor+), and
// the HUD and tracks, drawn by cameras like any other, keep their size in
// the middle 16:9. A taller window (16:10) would lose its sides that way, so
// there each perspective camera drawing to the screen also has its vertical
// field of view widened by as much as the window is taller than 16:9: the
// sides stay where 16:9 has them and more shows above and below (Vert+).

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace band3::aspect {

// what the cameras are built for, width << 32 | height: 0 for RB3's own 16:9,
// when not filling the window or when it is 16:9 within half a percent
// (1366x768)
inline uint64_t ShapeFor(uint32_t width, uint32_t height, bool fill) {
    if (!fill || !width || !height) return 0;
    const double aspect = double(width) / double(height);
    if (std::abs(aspect / (16.0 / 9.0) - 1.0) < 0.005) return 0;
    return uint64_t(width) << 32 | height;
}

inline uint32_t ShapeWidth(uint64_t shape) { return uint32_t(shape >> 32); }
inline uint32_t ShapeHeight(uint64_t shape) { return uint32_t(shape); }

// Rnd::YRatio for a shape: its height over width; RB3's 9/16 for 0
inline double YRatio(uint64_t shape) {
    if (!shape) return 9.0 / 16.0;
    return double(ShapeHeight(shape)) / double(ShapeWidth(shape));
}

// how much narrower than 16:9 a shape is, its width over height over 16:9's
// (0.9 for 16:10); 1 for a wider one, or 0
inline double TallScale(uint64_t shape) {
    if (!shape) return 1.0;
    const double aspect = double(ShapeWidth(shape)) / double(ShapeHeight(shape));
    return std::min(1.0, aspect / (16.0 / 9.0));
}

// a camera's vertical field of view (mYFov) widened for a tall shape:
// UpdateLocal takes tan(fov * fov_scale), so tan(widened * fov_scale) =
// tan(fov * fov_scale) / tall, which with the shape's YRatio leaves the x
// scale 16:9's
inline float WidenedFov(float fov, float fov_scale, double tall) {
    return float(std::atan(std::tan(double(fov) * fov_scale) / tall) / fov_scale);
}

// where the game's 16:9 ends in a shape's picture, in clip x and y (1 the
// picture's edge): 0.75 1 for 21:9, 1 0.9 for 16:10, 1 1 for 16:9. The
// native renderer moves an overlay draw's vertices past it out to the edge,
// so menu art drawn a little past 16:9 reaches the window's edge
// (mesh.hlsl's StretchEdges).
inline void OverlayEdge(uint64_t shape, float out[2]) {
    out[0] = out[1] = 1.0f;
    if (!shape) return;
    const double ratio =
        double(ShapeWidth(shape)) / double(ShapeHeight(shape)) / (16.0 / 9.0);
    if (ratio > 1.0)
        out[0] = float(1.0 / ratio);
    else
        out[1] = float(ratio);
}

}  // namespace band3::aspect
