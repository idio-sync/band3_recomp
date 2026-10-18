#pragma once

// VR's eyes for the native renderer (the `vr` setting, xr_present.h): where
// each eye is in the venue and what it sees, and a captured frame re-posed
// for one (RePose); and the HUD panel, the frame's other draws (the track,
// the HUD, menus) as a quad in front of the head: which draws are its
// (HudBoundary) and where it hangs (HudPlacement); and the game camera's
// window (vr_viewpoint game_camera, and every menu): the head stands where the
// game's camera is and sees the venue through a window in LOCAL space of a
// fixed size (vr_window_width) that the camera's view fills (WindowFrame,
// WindowEye), dark around it (FrameQuads); and each frame's choice of all
// that (ResolveFrame). No OpenXR
// here: XR's poses and fields of view come in as plain numbers, so this is the
// same on every platform and unit-tested.
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

#include "src/Render/camera_cut.h"
#include "src/Render/frame_compose.h"
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

// An eye for RePose: where it is and what it sees. Its view is View(pose),
// or `view` with has_view (WindowEye's, which isn't a rigid camera's); the
// shades still get `pose`'s point and axes.
struct Eye {
    freecam::Pose pose;
    Fov fov;
    bool has_view = false;
    Mat4 view{};
};

inline Mat4 EyeView(const Eye& e) { return e.has_view ? e.view : freecam::View(e.pose); }

struct RePoseStats {
    bool ok = false;  // a perspective world camera was found
    uint32_t cam = 0;
    uint32_t draws_moved = 0, shades_cloned = 0;
    freecam::Pose game{};  // the game camera's pose
};

// `in` seen from `eye`: a copy of it whose world camera (WorldCameraOf, the
// back buffer's camera with the most draws before post_boundary) is the eye.
// Every draw drawn with that camera's view_proj, in any pass, gets the eye's
// (EyeView times EyeProjection of the camera's projection), and the shades
// they use are cloned (the source's are shared, never changed) with the
// eye's position and, for billboards, its axes (VS c16..c18). Other cameras'
// draws (the track, the HUD, shadow maps' lights) stay as they are. `in` is
// left unchanged. Without a perspective world camera, `out` is `in` as is.
RePoseStats RePose(const FrameCapture& in, const Eye& eye, FrameCapture& out);

// Where the HUD panel's draws start in `fc` (RasterOptions::hud_from,
// soft_raster.h's OnHud): post_boundary, or 0, the whole picture on the
// panel, where the eyes can't see the frame from where they are: no
// DoPostProcess (some menu frames, frame_compose.h's ProcKnown) or no
// perspective world camera for RePose. The eyes then show the clear colour,
// not the flat picture stretched across them.
inline uint32_t HudBoundary(const FrameCapture& fc) {
    if (!ProcKnown(fc)) return 0;
    const WorldCamera cam = WorldCameraOf(fc);
    freecam::Pose pose{};
    Mat4 proj{};
    if (!cam.valid || !freecam::Decompose(cam.view_proj, pose, proj)) return 0;
    return fc.post_boundary;
}

// The HUD panel's quad layer (XrCompositionLayerQuad) in LOCAL space: in
// front of the head as it faced at the recenter, `distance` metres ahead
// along the floor and `below` metres under the eyes, tilted back to face
// them. Its pose's -z (the quad's back) points away from the eyes: the quad
// faces +z, toward them, at a tilt of atan(below / distance) about its x.
struct HudLayout {
    float distance = 1.8f;  // metres ahead
    float below = 0.25f;    // metres below the eyes
    float width_deg = 55;   // across, seen from the eyes at `distance`; used without width_m
    float width_m = 0;      // across in metres; 0: from width_deg
    float aspect = 16.0f / 9.0f;  // the picture's, width over height
};

struct HudQuad {
    XrPose pose;
    float size[2] = {0, 0};  // XrExtent2Df: width, height in metres
};

// `head` is the recenter's head pose in LOCAL space (its position the eyes'
// midpoint); only its yaw (turn about XR's +y) is kept, so the panel stands
// level whatever the head's pitch and roll were
inline HudQuad HudPlacement(const HudLayout& l, const XrPose& head) {
    HudQuad h;
    // the head's back (+z, opposite its forward) turned by its quaternion,
    // flattened onto the floor: the yaw about +y, from +z
    const float back[3] = {0, 0, 1};
    float f[3];
    Rotate(head.q, back, f);
    float yaw = std::atan2(f[0], f[2]);
    if (!(std::fabs(f[0]) + std::fabs(f[2]) > 1e-6f)) yaw = 0;  // looking straight up or down
    // the panel's centre: ahead of the head on the floor's plane, below it
    const float s = std::sin(yaw), c = std::cos(yaw);
    h.pose.p[0] = head.p[0] - s * l.distance;
    h.pose.p[1] = head.p[1] - l.below;
    h.pose.p[2] = head.p[2] - c * l.distance;
    // yaw about +y, then the tilt back about the panel's own x (its top edge
    // away from the eyes, its normal up toward them: -tilt): q = yaw * tilt
    const float tilt = std::atan2(l.below, l.distance);
    const float qy[4] = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    const float qx[4] = {-std::sin(tilt / 2), 0, 0, std::cos(tilt / 2)};
    h.pose.q[0] = qy[3] * qx[0] + qy[0] * qx[3] + qy[1] * qx[2] - qy[2] * qx[1];
    h.pose.q[1] = qy[3] * qx[1] - qy[0] * qx[2] + qy[1] * qx[3] + qy[2] * qx[0];
    h.pose.q[2] = qy[3] * qx[2] + qy[0] * qx[1] - qy[1] * qx[0] + qy[2] * qx[3];
    h.pose.q[3] = qy[3] * qx[3] - qy[0] * qx[0] - qy[1] * qx[1] - qy[2] * qx[2];
    const float width =
        l.width_m > 0 ? l.width_m : 2 * l.distance * std::tan(l.width_deg * 3.14159265f / 360);
    h.size[0] = width;
    h.size[1] = l.aspect > 0 ? width / l.aspect : width;
    return h;
}

// The head between two eyes' poses, for HudPlacement's recenter: the
// midpoint, facing the way they do (their quaternions' normalized sum, the
// right's sign matched to the left's: q and -q are one turn)
inline XrPose HeadPose(const XrPose& left, const XrPose& right) {
    XrPose h;
    const float dot = left.q[0] * right.q[0] + left.q[1] * right.q[1] +
                      left.q[2] * right.q[2] + left.q[3] * right.q[3];
    const float sign = dot < 0 ? -1.0f : 1.0f;
    float len = 0;
    for (int i = 0; i < 4; i++) {
        h.q[i] = left.q[i] + sign * right.q[i];
        len += h.q[i] * h.q[i];
    }
    len = std::sqrt(len);
    if (len > 1e-6f) {
        for (float& c : h.q) c /= len;
    } else {
        for (int i = 0; i < 4; i++) h.q[i] = left.q[i];
    }
    for (int i = 0; i < 3; i++) h.p[i] = (left.p[i] + right.p[i]) / 2;
    return h;
}

// q's inverse (a unit quaternion's conjugate)
inline void Conjugate(const float q[4], float out[4]) {
    out[0] = -q[0];
    out[1] = -q[1];
    out[2] = -q[2];
    out[3] = q[3];
}

// a * b: b's turn, then a's
inline void QuatMul(const float a[4], const float b[4], float out[4]) {
    const float r[4] = {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    for (int i = 0; i < 4; i++) out[i] = r[i];
}

// A captured camera's field of view in radians, across and up and down: its
// projection's x scale (camera-space x to clip x, m[0][0]) and up scale
// (camera-space z to clip y, m[2][1]: Milo's up is z), as replay.cpp's free
// cam reports it. RB3's cameras are symmetric; an off-centre one's offset is
// left out.
struct GameFov {
    float hfov = 0, vfov = 0;
};

inline GameFov GameFovOf(const Mat4& proj) {
    GameFov f;
    if (proj.m[0][0] != 0) f.hfov = 2 * std::atan(1 / std::fabs(proj.m[0][0]));
    if (proj.m[2][1] != 0) f.vfov = 2 * std::atan(1 / std::fabs(proj.m[2][1]));
    return f;
}

// The game camera's window: a rectangle in LOCAL space, `distance` metres
// straight ahead of the head at the recenter, at its eyes' height, turned by
// its yaw alone (level and upright whatever the head's pitch and roll),
// `width` radians across seen from there (vr_window_width) and the game's
// picture's aspect high (kWindowAspect: RB3 draws 16:9 whatever the camera).
// Its size doesn't follow the game camera's field of view, so it stays put at
// cuts; the camera's view is fitted to it instead (WindowEye). Its pose as a
// quad's: +z toward the head.
inline constexpr float kWindowAspect = 16.0f / 9.0f;

struct WindowFrame {
    XrPose pose;
    float distance = 0;
    float size[2] = {0, 0};  // width, height in metres
};

inline WindowFrame WindowPlacement(const XrPose& head, float distance, float width,
                                   float aspect = kWindowAspect) {
    WindowFrame w;
    w.distance = distance;
    w.size[0] = 2 * distance * std::tan(width / 2);
    w.size[1] = aspect > 0 ? w.size[0] / aspect : w.size[0];
    HudLayout l;
    l.distance = distance;
    l.below = 0;
    l.width_m = w.size[0] > 0 ? w.size[0] : 1;
    w.pose = HudPlacement(l, head).pose;
    return w;
}

// What the window shows. From the head at the recenter the window is the game's
// picture, whatever the camera's field of view: the venue is magnified
// k = tan(width / 2) / tan(hfov / 2) times against a true window that size
// (a 35-degree menu camera in a 100-degree window: 3.8; a 79-degree song
// shot: 1.45). It is a true window onto the venue squeezed k times along the
// camera's forward. In the window's frame (metres; x right, y up, z toward
// the head, which is at (0, 0, D) at the recenter), the venue's point X, at
// d = X - C from the game camera C with right R, up U and forward F, is at
//
//     ( d.R / sx,  d.U / sy,  D - d.F / sz )      (VenueToWindow)
//
// with sx = u, u the venue's units a metre (39.37 times vr_world_scale), so
// across the venue is life size and the eyes are the user's IPD apart in it;
// sz = k u along the camera's forward; and sy = sz D tan(vfov / 2) / (the
// window's half height), u as well when the camera's picture has the window's
// aspect. The window's plane (z = 0) is the plane k u D ahead of the camera
// and its edges are the frustum's there, so from (0, 0, D) the window maps
// exactly onto the game's picture (clip -1..1 both ways), and from anywhere
// else each eye sees exactly that squeezed venue through a true window: the
// head moving a metre moves the eye u across and k u along the forward, with
// the parallax a window gives. The squeeze is the approximation: a point Z
// units ahead of the camera looks Z / (k u) metres away, k times nearer than
// at life size, and is life size across, so the stereo depth of the scene is
// flattened k times (as a long lens flattens it; a camera wider than the
// window, k < 1, deepens it instead).
struct WindowScale {
    float sx = 1, sy = 1, sz = 1;  // venue units a metre: across, up, forward
};

inline WindowScale WindowScaleOf(const WindowFrame& w, const GameFov& fov, float u) {
    WindowScale s{u, u, u};
    const float tx = std::tan(fov.hfov / 2), ty = std::tan(fov.vfov / 2);
    const float a = w.size[0] / 2, b = w.size[1] / 2;
    if (tx > 0 && w.distance > 0 && a > 0) s.sz = u * a / (w.distance * tx);
    if (ty > 0 && b > 0) s.sy = s.sz * w.distance * ty / b;
    return s;
}

// k, the venue's magnification against a true window (1 without a field of view)
inline float WindowMagnify(const WindowScale& s) { return s.sx > 0 ? s.sz / s.sx : 1; }

inline void VenueToWindow(const WindowFrame& w, const freecam::Pose& game, const WindowScale& s,
                          const float x[3], float out[3]) {
    const float d[3] = {x[0] - game.eye[0], x[1] - game.eye[1], x[2] - game.eye[2]};
    out[0] = freecam::Dot3(d, game.right) / s.sx;
    out[1] = freecam::Dot3(d, game.up) / s.sy;
    out[2] = w.distance - freecam::Dot3(d, game.fwd) / s.sz;
}

// a point in the window's frame in LOCAL space
inline void WindowToLocal(const WindowFrame& w, const float p[3], float out[3]) {
    float t[3];
    Rotate(w.pose.q, p, t);
    for (int c = 0; c < 3; c++) out[c] = w.pose.p[c] + t[c];
}

// An eye through the window: its view is the venue put in LOCAL space by
// VenueToWindow and seen from where the eye is (XR's pose, LOCAL), scaled by
// sz so that from the head at the recenter a point's depth is its distance
// ahead of the game camera, as the captured projection's near and far planes
// have it; its field of view stays the headset's (RePose gives it its own
// projection). Its pose, for the shades, is the venue point VenueToWindow
// puts at the eye (game.eye + sx r.x R + sy r.y U - sz (r.z - D) F, r the
// eye in the window's frame) with the camera's axes turned as the eye is from
// facing the window (rigid: billboards face it, unsqueezed).
inline Eye WindowEye(const XrPose& eye, const Fov& fov, const WindowFrame& w,
                     const freecam::Pose& game, const GameFov& game_fov, float units_per_metre) {
    const WindowScale s = WindowScaleOf(w, game_fov, units_per_metre);
    Eye e;
    e.fov = fov;
    float inv[4];
    Conjugate(w.pose.q, inv);
    // the eye's place in the window's frame
    const float rel[3] = {eye.p[0] - w.pose.p[0], eye.p[1] - w.pose.p[1],
                          eye.p[2] - w.pose.p[2]};
    float r[3];
    Rotate(inv, rel, r);
    for (int c = 0; c < 3; c++)
        e.pose.eye[c] = game.eye[c] + s.sx * r[0] * game.right[c] + s.sy * r[1] * game.up[c] -
                        s.sz * (r[2] - w.distance) * game.fwd[c];
    // the window's axes x, y, z as the venue's: right, up, -forward
    auto venue = [&](const float v[3], float out[3]) {
        for (int c = 0; c < 3; c++)
            out[c] = v[0] * game.right[c] + v[1] * game.up[c] - v[2] * game.fwd[c];
    };
    // the eye's axes (XR's +x right, -z forward, +y up) in the window's frame
    float turn[4];
    QuatMul(inv, eye.q, turn);
    const float axes[3][3] = {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};
    float* out[3] = {e.pose.right, e.pose.fwd, e.pose.up};
    for (int i = 0; i < 3; i++) {
        float a[3];
        Rotate(turn, axes[i], a);
        venue(a, out[i]);
    }
    // The view, row vectors: the venue's axes and origin each to the eye's
    // camera space (Milo's: x right, y forward, z up), times sz. The axes
    // through the linear part alone, so no large numbers are subtracted.
    float eye_inv[4];
    Conjugate(eye.q, eye_inv);
    auto camera = [&](const float local[3], float out3[3]) {
        float v[3];
        Rotate(eye_inv, local, v);
        out3[0] = v[0] * s.sz;
        out3[1] = -v[2] * s.sz;
        out3[2] = v[1] * s.sz;
    };
    for (int i = 0; i < 3; i++) {
        const float dir[3] = {game.right[i] / s.sx, game.up[i] / s.sy, -game.fwd[i] / s.sz};
        float local[3];
        Rotate(w.pose.q, dir, local);
        camera(local, e.view.m[i]);
    }
    const float origin[3] = {0, 0, 0};
    float win[3], local[3];
    VenueToWindow(w, game, s, origin, win);
    WindowToLocal(w, win, local);
    for (int c = 0; c < 3; c++) local[c] -= eye.p[c];
    camera(local, e.view.m[3]);
    e.view.m[3][3] = 1;
    e.has_view = true;
    return e;
}

// The dark around the window (vr_window_dim): a box of quads about the head
// whose front face is the window's plane with the window cut out of it, four
// strips that tile the face around the window, and its four sides and back,
// so that from anywhere in it every way but through the window is covered,
// each way by one quad alone (quad layers have no depth: they cover in the
// order they're given, so none may cover the window). The face reaches
// kFrameReach times the window's distance each way (further for a window
// wider than that), the box as far behind the head. Each quad's +z faces into
// the box.
inline constexpr int kFrameQuads = 9;
inline constexpr float kFrameReach = 3;

// a pose in the window's frame (x right, y up, z toward the head) in LOCAL space
inline XrPose InWindow(const WindowFrame& w, const float p[3], const float q[4]) {
    XrPose out;
    float turned[3];
    Rotate(w.pose.q, p, turned);
    for (int c = 0; c < 3; c++) out.p[c] = w.pose.p[c] + turned[c];
    QuatMul(w.pose.q, q, out.q);
    return out;
}

inline int FrameQuads(const WindowFrame& w, HudQuad out[kFrameQuads]) {
    const float s = std::fmax(kFrameReach * w.distance, 1.5f * std::fmax(w.size[0], w.size[1]) / 2);
    // the window's half size, kept inside the face
    const float hx = std::fmin(w.size[0] / 2, s * 0.99f), hy = std::fmin(w.size[1] / 2, s * 0.99f);
    const float back = w.distance + s;  // the back face's z
    const float face[4] = {0, 0, 0, 1};
    const float h = 0.70710678f;
    // facing +x, -x, -y, +y, -z: turns about y and x
    const float to_right[4] = {0, h, 0, h}, to_left[4] = {0, -h, 0, h};
    const float down[4] = {h, 0, 0, h}, up[4] = {-h, 0, 0, h};
    const float behind[4] = {0, 1, 0, 0};
    struct Q {
        float p[3];
        const float* q;
        float w, h;
    } quads[kFrameQuads] = {
        // the face: top and bottom strips the face's width, left and right
        // the window's height between them
        {{0, (hy + s) / 2, 0}, face, 2 * s, s - hy},
        {{0, -(hy + s) / 2, 0}, face, 2 * s, s - hy},
        {{-(hx + s) / 2, 0, 0}, face, s - hx, 2 * hy},
        {{(hx + s) / 2, 0, 0}, face, s - hx, 2 * hy},
        // the sides, from the face to the back: the left wall faces +x (its
        // width along -z), the right -x, the ceiling -y (its height along
        // +z), the floor +y
        {{-s, 0, back / 2}, to_right, back, 2 * s},
        {{s, 0, back / 2}, to_left, back, 2 * s},
        {{0, s, back / 2}, down, 2 * s, back},
        {{0, -s, back / 2}, up, 2 * s, back},
        {{0, 0, back}, behind, 2 * s, 2 * s},
    };
    for (int i = 0; i < kFrameQuads; i++) {
        out[i].pose = InWindow(w, quads[i].p, quads[i].q);
        out[i].size[0] = quads[i].w;
        out[i].size[1] = quads[i].h;
    }
    return kFrameQuads;
}

// How dark the eyes are after a cut (vr_cut_fade_ms): black at the cut, back
// to the picture over `fade_ms`, as the time left over the whole (1 black,
// 0 none)
inline float CutFade(int64_t since_ns, double fade_ms) {
    if (!(fade_ms > 0)) return 0;
    if (since_ns < 0) since_ns = 0;
    const double left = 1 - double(since_ns) / (fade_ms * 1e6);
    return left <= 0 ? 0.0f : left >= 1 ? 1.0f : float(left);
}

// Where the eyes are in the venue (the `vr_viewpoint` setting): the game
// camera's window (game_camera), or one of the places in the venue
// (front_row, on_stage, venue: not made yet, so LOCAL space where vr_anchor
// says, as before the window). Menus are seen through the window whatever
// this is.
enum class ViewMode { kGameCamera, kFrontRow, kOnStage, kVenue };

inline bool ParseViewMode(const std::string& text, ViewMode& mode) {
    if (text == "game_camera") mode = ViewMode::kGameCamera;
    else if (text == "front_row") mode = ViewMode::kFrontRow;
    else if (text == "on_stage") mode = ViewMode::kOnStage;
    else if (text == "venue") mode = ViewMode::kVenue;
    else return false;
    return true;
}

struct Viewpoint {
    ViewMode mode = ViewMode::kGameCamera;
    bool window = true;  // songs too through the window; else from `anchor`
    // LOCAL space's place in the venue, or the game camera's point facing
    // where it looks (AnchorFromCamera) with `follow`
    Anchor anchor;
    bool follow = false;
};

// One frame's inputs: the views and the recenter's head (LOCAL), the
// captured frame's HudBoundary and world camera, and the settings
struct FrameInputs {
    XrPose views[2];
    Fov fov[2];
    XrPose head;
    uint32_t hud_boundary = 0;  // HudBoundary's
    bool camera = false;        // a perspective world camera, split into these
    freecam::Pose game{};
    Mat4 proj{};
    // a song is being played: RB3's Game exists and its screen is up
    // (native_view.h's InSong and SongScreen), not its loading or results
    bool in_song = false;
    bool hud = true;       // vr_hud
    bool hud_in_window = false;
    HudLayout hud_layout;  // the panel's own place (vr_hud_*); its distance the window's
    float window_width_deg = 100;  // vr_window_width
    float dim = 1;                 // vr_window_dim
    float world_scale = 1;
};

// what the eyes show and where the quads go, for VrFrame and Present
struct FrameLayout {
    // the eyes through the game camera's window; else from the anchor, or
    // black: the frame has no world the eyes can see (HudBoundary 0)
    bool window = false;
    bool eyes_black = false;
    float fade = 0;  // the eyes darkened by this (CutFade), set by the caller
    uint32_t hud_boundary = 0;  // the frame's HudBoundary, the eyes' hud_from
    Eye eyes[2];
    GameFov game_fov;
    WindowFrame window_frame;
    float magnify = 1;  // the window's k (WindowMagnify)
    // the HUD panel: shown, where, and whether that's the window itself
    bool hud = false;
    bool hud_in_window = false;
    HudQuad hud_quad;
    // the dark around the window, black at `dim`'s alpha
    int frame_quads = 0;
    HudQuad frame[kFrameQuads];
    float dim = 0;
};

// The frame's choice: through the window where the capture has a world
// camera and it's a menu or `vp.window` (the panel in the window, as the
// game's picture has it, in menus and with hud_in_window; on its own place in
// songs, whose shots are wider than a comfortable panel); from the anchor in
// songs otherwise; black eyes and the panel on its own place without a world.
inline FrameLayout ResolveFrame(const Viewpoint& vp, const FrameInputs& in) {
    FrameLayout l;
    l.hud_boundary = in.hud_boundary;
    const bool world = in.hud_boundary > 0 && in.camera;
    l.eyes_black = !world;
    l.window = world && (vp.window || !in.in_song);
    l.hud = in.hud;
    l.hud_quad = HudPlacement(in.hud_layout, in.head);
    if (l.window) {
        l.game_fov = GameFovOf(in.proj);
        l.window_frame = WindowPlacement(in.head, in.hud_layout.distance,
                                         in.window_width_deg * 3.14159265f / 180);
        const float u = kUnitsPerMetre * in.world_scale;
        l.magnify = WindowMagnify(WindowScaleOf(l.window_frame, l.game_fov, u));
        for (int e = 0; e < 2; e++)
            l.eyes[e] = WindowEye(in.views[e], in.fov[e], l.window_frame, in.game, l.game_fov, u);
        if (!in.in_song || in.hud_in_window) {
            l.hud_in_window = true;
            l.hud_quad.pose = l.window_frame.pose;
            l.hud_quad.size[0] = l.window_frame.size[0];
            l.hud_quad.size[1] = l.window_frame.size[1];
        }
        l.dim = in.dim < 0 ? 0 : in.dim > 1 ? 1 : in.dim;
        if (l.dim > 0) l.frame_quads = FrameQuads(l.window_frame, l.frame);
        return l;
    }
    Anchor a = vp.anchor;
    if (vp.follow && in.camera) a = AnchorFromCamera(in.game);
    for (int e = 0; e < 2; e++) {
        l.eyes[e].pose = EyePose(in.views[e], a, in.world_scale);
        l.eyes[e].fov = in.fov[e];
    }
    return l;
}

// A point in LOCAL space in an eye's picture, in pixels from its top left
// (`width` by `height`, its field of view `fov`); false behind the eye
inline bool EyePixel(const XrPose& eye, const Fov& fov, uint32_t width, uint32_t height,
                     const float p[3], float& x, float& y) {
    float inv[4];
    Conjugate(eye.q, inv);
    const float rel[3] = {p[0] - eye.p[0], p[1] - eye.p[1], p[2] - eye.p[2]};
    float v[3];
    Rotate(inv, rel, v);
    if (!(v[2] < 0)) return false;
    const float tx = v[0] / -v[2], ty = v[1] / -v[2];
    const float l = std::tan(fov.left), r = std::tan(fov.right);
    const float u = std::tan(fov.up), d = std::tan(fov.down);
    x = (tx - l) / (r - l) * float(width);
    y = (u - ty) / (u - d) * float(height);
    return true;
}

// a quad's corners in LOCAL space: top left, top right, bottom right, bottom left
inline void QuadCorners(const HudQuad& q, float out[4][3]) {
    const float sx[4] = {-1, 1, 1, -1}, sy[4] = {1, 1, -1, -1};
    for (int i = 0; i < 4; i++) {
        const float c[3] = {sx[i] * q.size[0] / 2, sy[i] * q.size[1] / 2, 0};
        float t[3];
        Rotate(q.pose.q, c, t);
        for (int k = 0; k < 3; k++) out[i][k] = q.pose.p[k] + t[k];
    }
}

// A setting used as a button (vr_hud_recenter): each change of its value
// from the one last seen fires once; the first value seen only sets the
// start, so the value it was saved with doesn't fire at launch
struct ChangeTrigger {
    bool seen = false;
    bool last = false;
    bool Fired(bool value) {
        const bool fired = seen && value != last;
        seen = true;
        last = value;
        return fired;
    }
};

}  // namespace band3::render::vr
