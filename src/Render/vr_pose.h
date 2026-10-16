#pragma once

// VR's eyes for the native renderer (the `vr` setting, xr_present.h): where
// each eye is in the venue and what it sees, and a captured frame re-posed
// for one (RePose). No OpenXR here: XR's poses and fields of view come in as
// plain numbers, so this is the same on every platform and unit-tested.
//
// Units and axes: Milo's are inches (kUnitsPerMetre) with x right, y forward,
// z up; OpenXR's metres with x right, y up, -z forward, so XR (x, y, z) is
// Milo (x, -z, y). The head is in XR's LOCAL space, its origin where the
// head was as the session began, placed in the venue by an Anchor: a point
// and a turn about z.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "src/Render/free_cam.h"
#include "src/Render/scene_capture.h"

namespace band3::render::vr {

inline constexpr float kUnitsPerMetre = 39.37f;

// XrFovf: angles in radians, left and down negative
struct Fov {
    float left = 0, right = 0, up = 0, down = 0;
};

// `proj` (a captured perspective projection, freecam::Decompose's) for an eye
// whose frustum is `fov`, asymmetric as a headset's are: clip x from the
// camera-space x over the distance in front (y) spanning tan(left)..tan(right),
// clip y from z likewise; the depth and w columns (2, 3) as captured, so depth
// is as the game's camera has it.
inline Mat4 EyeProjection(Mat4 proj, const Fov& fov) {
    const float l = std::tan(fov.left), r = std::tan(fov.right);
    const float u = std::tan(fov.up), d = std::tan(fov.down);
    for (int row = 0; row < 4; row++) proj.m[row][0] = proj.m[row][1] = 0;
    proj.m[0][0] = 2 / (r - l);
    proj.m[1][0] = -(r + l) / (r - l);
    proj.m[2][1] = 2 / (u - d);
    proj.m[1][1] = -(u + d) / (u - d);
    return proj;
}

// XrPosef: orientation x, y, z, w; position in metres
struct XrPose {
    float q[4] = {0, 0, 0, 1};
    float p[3] = {0, 0, 0};
};

// where LOCAL space's origin is in the venue, and its turn about z in
// radians, counterclockwise seen from above (0 looks down +y)
struct Anchor {
    float pos[3] = {0, 0, 0};
    float yaw = 0;
};

// v turned by unit quaternion q (x, y, z, w)
inline void Rotate(const float q[4], const float v[3], float out[3]) {
    // t = 2 q.xyz x v; v' = v + w t + q.xyz x t
    const float t[3] = {2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]),
                        2 * (q[0] * v[1] - q[1] * v[0])};
    out[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    out[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    out[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

// an XR direction or point (metres times `scale`) in Milo's axes
inline void XrToMilo(const float v[3], float scale, float out[3]) {
    out[0] = v[0] * scale;
    out[1] = -v[2] * scale;
    out[2] = v[1] * scale;
}

// turned by `a`'s yaw, and moved to its point if a position
inline void Place(const Anchor& a, const float v[3], bool point, float out[3]) {
    const float c = std::cos(a.yaw), s = std::sin(a.yaw);
    out[0] = v[0] * c - v[1] * s + (point ? a.pos[0] : 0);
    out[1] = v[0] * s + v[1] * c + (point ? a.pos[1] : 0);
    out[2] = v[2] + (point ? a.pos[2] : 0);
}

// An eye's pose in the venue from its XR pose in LOCAL space: its right,
// forward and up axes and where it is, `world_scale` times life size (2: the
// venue looks half as big)
inline freecam::Pose EyePose(const XrPose& x, const Anchor& a, float world_scale = 1) {
    freecam::Pose p{};
    const float axes[3][3] = {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};  // right, forward, up
    float* out[3] = {p.right, p.fwd, p.up};
    for (int i = 0; i < 3; i++) {
        float xr[3], milo[3];
        Rotate(x.q, axes[i], xr);
        XrToMilo(xr, 1, milo);
        Place(a, milo, false, out[i]);
    }
    float milo[3];
    XrToMilo(x.p, kUnitsPerMetre * world_scale, milo);
    Place(a, milo, true, p.eye);
    return p;
}

// "x,y,z,yaw" (yaw in degrees); false if it isn't four numbers
inline bool ParseAnchor(const std::string& text, Anchor& a) {
    float v[4];
    const char* at = text.c_str();
    for (int i = 0; i < 4; i++) {
        char* end = nullptr;
        v[i] = std::strtof(at, &end);
        if (end == at) return false;
        while (*end == ' ') end++;
        if (i < 3 && *end++ != ',') return false;
        at = end;
    }
    if (*at) return false;
    for (int i = 0; i < 3; i++) a.pos[i] = v[i];
    a.yaw = v[3] * 3.14159265f / 180;
    return true;
}

// the game camera's point, facing its forward flattened onto the floor
inline Anchor AnchorFromCamera(const freecam::Pose& cam) {
    Anchor a;
    for (int i = 0; i < 3; i++) a.pos[i] = cam.eye[i];
    a.yaw = std::atan2(-cam.fwd[0], cam.fwd[1]);
    return a;
}

// An eye for RePose: where it is and what it sees
struct Eye {
    freecam::Pose pose;
    Fov fov;
};

struct RePoseStats {
    bool ok = false;  // a perspective world camera was found
    uint32_t cam = 0;
    uint32_t draws_moved = 0, shades_cloned = 0;
    freecam::Pose game{};  // the game camera's pose
};

// `in` seen from `eye`: a copy of it whose world camera (WorldCameraOf, the
// back buffer's camera with the most draws before post_boundary) is the eye.
// Every draw drawn with that camera's view_proj, in any pass, gets the eye's
// (its view times EyeProjection of the camera's projection), and the shades
// they use are cloned (the source's are shared, never changed) with the
// eye's position and, for billboards, its axes (VS c16..c18). Other cameras'
// draws (the track, the HUD, shadow maps' lights) stay as they are. `in` is
// left unchanged. Without a perspective world camera, `out` is `in` as is.
RePoseStats RePose(const FrameCapture& in, const Eye& eye, FrameCapture& out);

}  // namespace band3::render::vr
