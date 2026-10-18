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

// One track camera's draws seen from an eye (TrackEye): those drawn by camera
// `cam` with `old_vp` get `view_proj` and the whole picture rather than the
// camera's viewport, their shades `pose`'s point and axes
inline constexpr int kMaxTrackCams = 4;

struct TrackView {
    uint32_t cam = 0;
    Mat4 old_vp{};
    Mat4 view_proj{};
    freecam::Pose pose{};
};

// the dark around the game camera's window as quads (FrameQuads)
inline constexpr int kFrameQuads = 9;

// An eye for RePose: where it is and what it sees. Its view is View(pose),
// or `view` with has_view (WindowEye's, which isn't a rigid camera's); the
// shades still get `pose`'s point and axes. In songs each track camera's
// draws are seen from it too (`tracks`). The eye draws its own dark (its
// draws through kEyeDarkCam, before the tracks): black over the picture at
// `fade` (a cut's, CutFade), and the dark around the window, `dark_count` of
// FrameQuads' quads at `dark_alpha` (vr_window_dim), seen from `xr`, its
// pose in LOCAL space.
struct Eye {
    freecam::Pose pose;
    Fov fov;
    bool has_view = false;
    Mat4 view{};
    int track_count = 0;
    TrackView tracks[kMaxTrackCams];
    XrPose xr;
    float fade = 0;
    int dark_count = 0;
    float dark_alpha = 0;
    // the quads' corners in LOCAL space (QuadCorners' order)
    float dark[kFrameQuads][4][3] = {};
    // From a fixed viewpoint (vr_crowd_clear): the crowds' draws
    // (FrameCapture::mesh_owners) standing within this many venue units of
    // the eye across the floor are left out, so it isn't inside a card or a
    // crowd member; 0 none
    float crowd_clear = 0;
};

// the eyes' own dark draws' camera (Eye::fade, Eye::dark): no camera of
// the game's, so drawn over the whole picture, depth as 1/w
inline constexpr uint32_t kEyeDarkCam = 0xFFFFFFF0u;

inline Mat4 EyeView(const Eye& e) { return e.has_view ? e.view : freecam::View(e.pose); }

struct RePoseStats {
    bool ok = false;  // a perspective world camera was found
    uint32_t cam = 0;
    uint32_t draws_moved = 0, shades_cloned = 0;
    freecam::Pose game{};  // the game camera's pose
    // the eye's track cameras' draws moved, and those of theirs drawn with
    // another matrix (left as they were)
    uint32_t track_draws = 0, track_unmoved = 0;
    // crowd draws left out near the eye (Eye::crowd_clear)
    uint32_t crowd_cleared = 0;
};

// `in` seen from `eye`: a copy of it whose world camera (WorldCameraOf, the
// back buffer's camera with the most draws before post_boundary) is the eye.
// Every draw drawn with that camera's view_proj, in any pass, gets the eye's
// (EyeView times EyeProjection of the camera's projection), and the shades
// they use are cloned (the source's are shared, never changed) with the
// eye's position and, for billboards, its axes (VS c16..c18). Likewise each
// of the eye's track cameras' draws (Eye::tracks) gets its TrackView (a draw
// with its own transform folded into its matrix, as the gems and beat lines
// are, keeps it), the camera the whole picture, and is cut where the game's
// picture ends below (the bottom of the camera's viewport, through its
// captured matrix; CutAtPlane), so the track before the strikeline ends
// where the game shows it end instead of reaching under the head. The eye's
// own dark (Eye::fade, Eye::dark) goes in after the resolve, before the
// tracks: black draws through kEyeDarkCam, alpha blended, no depth. Other
// cameras' draws (the HUD, shadow maps' lights) stay as they are. With
// Eye::crowd_clear, the crowds' draws near the eye are squashed to a point
// (drawn, but nothing shows: the draws and passes stay as they were). `in`
// is left unchanged. Without a perspective world camera, the world's draws
// are left as they are.
RePoseStats RePose(const FrameCapture& in, const Eye& eye, FrameCapture& out);

// `g` with its triangles cut to plane . (x, y, z, 1) >= 0 (in its own
// space), new corners between the old; `g` itself if nothing is below it.
// The last few cuts are kept per thread, so both eyes and frames after cut
// the same mesh once.
std::shared_ptr<const Geometry> CutAtPlane(const std::shared_ptr<const Geometry>& g,
                                           const float plane[4]);

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

// The song's track cameras in `fc`: the 3D highway (the track's surface,
// lanes, gems, smashers, OD meter, multiplier), which the eyes draw from
// their own place in songs (RasterOptions::eye_cams, TrackEye) rather than
// leaving it flat on the HUD panel. RB3 draws each track after
// post-processing with a camera of its own, z range 0..1, in front of which
// a 0..0.1 camera draws the flat HUD (score, stars, band meter, the vocals'
// strip, lyrics), then a 0..1 camera one full-picture fade quad, then the
// pause menu's 0..1 camera if it's up. So a track camera is one with
// back-buffer mesh draws from post_boundary on, the first before the flat
// HUD's camera's (z range ending before 1; none without one), listed in
// fc.cameras with z range 0..1, a perspective camera (freecam::Decompose),
// and either more than one such draw or its one draw's picture not covering
// its viewport (the fade's does). One per player's track: 2 players' in two
// viewports (998x720 at 0 and 281), their projections shifted out by as much
// again (freecam::Decompose); 4 parts' three are full-picture and turned 10
// degrees apart. In
// order of their first draws; none without DoPostProcess. Menus' overlay
// cameras can look the same: callers ask only in songs.
struct TrackCamera {
    uint32_t cam = 0;
    Mat4 view_proj{};  // as captured: its draws are matched by it
    freecam::Pose pose{};
    Mat4 proj{};
    float viewport[4] = {};  // x, y, w, h in the back buffer's pixels
    uint32_t target_w = 0, target_h = 0;
    uint32_t first_draw = 0, draws = 0;
};

struct TrackCams {
    int count = 0;
    TrackCamera cams[kMaxTrackCams];
    const TrackCamera* Find(uint32_t cam) const {
        for (int i = 0; i < count; i++)
            if (cams[i].cam == cam) return &cams[i];
        return nullptr;
    }
};

TrackCams TrackCamerasOf(const FrameCapture& fc);

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

// A head's yaw, its turn about XR's +y (counterclockwise from above, 0
// facing -z): its back (+z, opposite its forward) turned by its quaternion,
// flattened onto the floor; 0 looking straight up or down
inline float HeadYaw(const XrPose& head) {
    const float back[3] = {0, 0, 1};
    float f[3];
    Rotate(head.q, back, f);
    if (!(std::fabs(f[0]) + std::fabs(f[2]) > 1e-6f)) return 0;
    return std::atan2(f[0], f[2]);
}

// `head` is the recenter's head pose in LOCAL space (its position the eyes'
// midpoint); only its yaw (turn about XR's +y) is kept, so the panel stands
// level whatever the head's pitch and roll were
inline HudQuad HudPlacement(const HudLayout& l, const XrPose& head) {
    HudQuad h;
    const float yaw = HeadYaw(head);
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

// The 3D highway (vr_track_3d): in songs each track (TrackCamerasOf) is an
// object in LOCAL space in front of the head, seen by each eye from where it
// is, with the stereo depth and parallax of a real one. RB3 lays a track out
// with its strikeline at its origin (the smashers sit there; the
// camera is 93.9 units from it, 22.6 above, looking 7 degrees down,
// 36.6 degrees across, near 30, far 224 for one player). The track is put so
// that `scale` of its units are a metre (30: its 19.6 units across are 0.65
// m) and the strikeline is `forward` metres ahead of the head at the
// recenter, along the floor, and `down` metres below its eyes. It lies as
// the game lays it out, level, its far end raised `pitch` degrees from that
// toward the eyes.
struct TrackLayout {
    bool on = true;        // vr_track_3d
    float forward = 1.2f;  // vr_track_forward, metres
    float down = 0.3f;     // vr_track_down, metres
    float pitch = 0;       // vr_track_pitch, degrees
    float scale = 30;      // vr_track_scale, track units a metre
};

// Where a track camera's frame is in LOCAL space (PlaceTracks): its point
// and turn (a quad's axes: x right, y up, -z the camera's forward). Each
// eye is placed against it as the camera is against the track (TrackEye).
struct TrackPlace {
    TrackCamera cam;
    XrPose frame;
};

// a turn of `yaw` about XR's +y (counterclockwise from above), then `pitch`
// about the turned x (positive looks up)
inline void YawPitch(float yaw, float pitch, float q[4]) {
    const float qy[4] = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    const float qx[4] = {std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)};
    QuatMul(qy, qx, q);
}

// a camera's pitch in radians, its forward above the floor (negative down)
inline float CameraPitch(const freecam::Pose& p) {
    return std::asin(std::fmax(-1.0f, std::fmin(1.0f, p.fwd[2])));
}

// The tracks' frames for `head`, the recenter's head (LOCAL; its yaw alone
// is kept). The frames share one point, so the tracks keep the game's
// layout, each where its camera shows it: the reference camera (the one
// looking most nearly along the track, +y; the first of equals) has its
// strikeline straight ahead at `forward` and `down`, and four parts'
// cameras, turned 10 degrees apart, show their tracks turned apart too.
// Each frame keeps its camera's own pitch plus `pitch` (so the track lies
// level). A camera whose forward isn't the picture's middle (two players':
// 998x720 viewports at either side, each projection shifted further out by
// as much) has its frame moved aside by as much as that shift is at its
// strikeline, so two players' tracks lie side by side, parallel, as the
// picture has them. Returns how many, none with `on` off.
inline int PlaceTracks(const TrackCams& t, const XrPose& head, const TrackLayout& l,
                       TrackPlace out[kMaxTrackCams]) {
    if (!l.on || t.count <= 0 || !(l.scale > 0)) return 0;
    const float yaw = HeadYaw(head);
    const float raise = l.pitch * 3.14159265f / 180;
    int ref = 0;
    float least = 1e9f;
    for (int i = 0; i < t.count; i++) {
        const float* f = t.cams[i].pose.fwd;
        const float turned = std::fabs(std::atan2(-f[0], f[1]));
        if (turned < least - 1e-4f) {
            least = turned;
            ref = i;
        }
    }
    const freecam::Pose& r = t.cams[ref].pose;
    // the strikeline (the origin) from the reference camera, in its axes
    const float to[3] = {-r.eye[0], -r.eye[1], -r.eye[2]};
    const float ahead = freecam::Dot3(to, r.fwd), above = freecam::Dot3(to, r.up);
    float q[4];
    YawPitch(yaw, CameraPitch(r) + raise, q);
    // the frames' point: the strikeline's place less its offset from the
    // camera, turned as the frame is (camera forward is the frame's -z)
    const float s = std::sin(yaw), c = std::cos(yaw);
    const float at[3] = {head.p[0] - s * l.forward, head.p[1] - l.down,
                         head.p[2] - c * l.forward};
    const float offset[3] = {0, above / l.scale, -ahead / l.scale};
    float turned[3];
    Rotate(q, offset, turned);
    int n = 0;
    for (int i = 0; i < t.count && i < kMaxTrackCams; i++) {
        const TrackCamera& cam = t.cams[i];
        TrackPlace& p = out[n++];
        p.cam = cam;
        YawPitch(yaw, CameraPitch(cam.pose) + raise, p.frame.q);
        float aside[3] = {0, 0, 0};
        if (cam.target_w && cam.target_h && cam.viewport[2] > 0 && cam.viewport[3] > 0 &&
            cam.proj.m[1][3] != 0) {
            const GameFov f = GameFovOf(cam.proj);
            const float half_w = cam.viewport[2] / 2, half_h = cam.viewport[3] / 2;
            // where the camera's forward is in the picture, from its middle,
            // in the viewport's halves: the viewport's middle, and the
            // projection's shift (freecam::Decompose) within it (y down)
            const float ox = cam.proj.m[1][0] / cam.proj.m[1][3];
            const float oy = cam.proj.m[1][1] / cam.proj.m[1][3];
            const float dx = (cam.viewport[0] + half_w - float(cam.target_w) / 2) / half_w + ox;
            const float dy = (cam.viewport[1] + half_h - float(cam.target_h) / 2) / half_h - oy;
            // the picture's shift is the track's, at its strikeline's distance
            const float to_i[3] = {-cam.pose.eye[0], -cam.pose.eye[1], -cam.pose.eye[2]};
            const float d = freecam::Dot3(to_i, cam.pose.fwd) / l.scale;
            aside[0] = d * dx * std::tan(f.hfov / 2);
            aside[1] = -d * dy * std::tan(f.vfov / 2);
        }
        float moved[3];
        Rotate(p.frame.q, aside, moved);
        for (int k = 0; k < 3; k++) p.frame.p[k] = at[k] - turned[k] + moved[k];
    }
    return n;
}

// An eye's view of a track camera's draws (RePose): the eye placed against
// the track as it is against the camera's frame (TrackPlace), `scale` units
// a metre, its axes the camera's turned as the eye is from the frame; seen
// with the eye's field of view (EyeProjection, the depth as captured: the
// renderers have no far plane, and the track's far end is its own fade).
inline TrackView TrackEye(const XrPose& eye, const Fov& fov, const TrackPlace& p, float scale) {
    TrackView v;
    v.cam = p.cam.cam;
    v.old_vp = p.cam.view_proj;
    const freecam::Pose& t = p.cam.pose;
    float inv[4];
    Conjugate(p.frame.q, inv);
    const float rel[3] = {eye.p[0] - p.frame.p[0], eye.p[1] - p.frame.p[1],
                          eye.p[2] - p.frame.p[2]};
    float r[3];
    Rotate(inv, rel, r);
    // the frame's x, y, z (XR) as the camera's right, up and back
    auto track = [&](const float a[3], float out[3]) {
        for (int k = 0; k < 3; k++) out[k] = a[0] * t.right[k] + a[1] * t.up[k] - a[2] * t.fwd[k];
    };
    float moved[3];
    track(r, moved);
    for (int k = 0; k < 3; k++) v.pose.eye[k] = t.eye[k] + scale * moved[k];
    float turn[4];
    QuatMul(inv, eye.q, turn);
    const float axes[3][3] = {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};  // right, forward, up
    float* out[3] = {v.pose.right, v.pose.fwd, v.pose.up};
    for (int i = 0; i < 3; i++) {
        float a[3];
        Rotate(turn, axes[i], a);
        track(a, out[i]);
    }
    v.view_proj = freecam::Mul(freecam::View(v.pose), EyeProjection(p.cam.proj, fov));
    return v;
}

// where a point of the track's (in its camera's world) is in LOCAL space,
// TrackEye's placing undone
inline void TrackToLocal(const TrackPlace& p, float scale, const float x[3], float out[3]) {
    const freecam::Pose& t = p.cam.pose;
    const float d[3] = {(x[0] - t.eye[0]) / scale, (x[1] - t.eye[1]) / scale,
                        (x[2] - t.eye[2]) / scale};
    const float in_frame[3] = {freecam::Dot3(d, t.right), freecam::Dot3(d, t.up),
                               -freecam::Dot3(d, t.fwd)};
    float turned[3];
    Rotate(p.frame.q, in_frame, turned);
    for (int k = 0; k < 3; k++) out[k] = p.frame.p[k] + turned[k];
}

// The track cameras held still through a song (native_view.cpp's VrFrame),
// so the track stays put in front of the head whatever the game's camera
// does: each slot's pose as first seen, taken again when its camera moves
// more than kTrackMoved units or turns kTrackTurned degrees (a new layout),
// or the number of tracks changes. Draws are still matched by each frame's
// own matrices and drawn with its projections.
inline constexpr float kTrackMoved = 1.0f;
inline constexpr float kTrackTurned = 1.0f;

class TrackHold {
 public:
    // `now` with the held poses; `moved` counts the slots taken again
    TrackCams Hold(const TrackCams& now, int& moved) {
        moved = 0;
        if (now.count != held_.count) {
            held_ = now;
            return now;
        }
        TrackCams out = now;
        for (int i = 0; i < now.count; i++) {
            const freecam::Pose &a = held_.cams[i].pose, &b = now.cams[i].pose;
            const float d[3] = {a.eye[0] - b.eye[0], a.eye[1] - b.eye[1], a.eye[2] - b.eye[2]};
            const float cos_turn = std::fmin(freecam::Dot3(a.fwd, b.fwd), freecam::Dot3(a.up, b.up));
            if (freecam::Len3(d) > kTrackMoved ||
                cos_turn < std::cos(kTrackTurned * 3.14159265f / 180)) {
                held_.cams[i] = now.cams[i];
                moved++;
            } else {
                out.cams[i].pose = held_.cams[i].pose;
            }
        }
        return out;
    }

 private:
    TrackCams held_;
};

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
//
// A close-up's k is large (8.5 at 16 degrees) and its depth nearly flat, so
// in songs the squeeze is capped (`max_squeeze`, vr_window_max_squeeze; 0 or
// less: none): sz is at most max_squeeze u, sy kept as the uncapped sz gives
// it. The frustum's plane at the window is then max_squeeze u D ahead and
// half max_squeeze D tx across: from the recenter's head the game's picture is
// a centred rectangle `picture` = max_squeeze / k of the window each way,
// level, the window around it dark (PictureFrame), and the depth flattened
// max_squeeze times at most.
struct WindowScale {
    float sx = 1, sy = 1, sz = 1;  // venue units a metre: across, up, forward
    float picture = 1;             // the picture's share of the window, each way
};

inline WindowScale WindowScaleOf(const WindowFrame& w, const GameFov& fov, float u,
                                 float max_squeeze = 0) {
    WindowScale s{u, u, u};
    const float tx = std::tan(fov.hfov / 2), ty = std::tan(fov.vfov / 2);
    const float a = w.size[0] / 2, b = w.size[1] / 2;
    if (tx > 0 && w.distance > 0 && a > 0) s.sz = u * a / (w.distance * tx);
    if (ty > 0 && b > 0) s.sy = s.sz * w.distance * ty / b;
    if (max_squeeze > 0 && s.sz > max_squeeze * u) {
        s.picture = max_squeeze * u / s.sz;
        s.sz = max_squeeze * u;
    }
    return s;
}

// k, the venue's magnification against a true window (1 without a field of view)
inline float WindowMagnify(const WindowScale& s) { return s.sx > 0 ? s.sz / s.sx : 1; }

// the game's picture in the window: the window itself, or with a capped
// squeeze the centred part of it the picture fills (WindowScale::picture)
inline WindowFrame PictureFrame(const WindowFrame& w, float picture) {
    WindowFrame p = w;
    if (picture > 0 && picture < 1) {
        p.size[0] *= picture;
        p.size[1] *= picture;
    }
    return p;
}

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
                     const freecam::Pose& game, const GameFov& game_fov, float units_per_metre,
                     float max_squeeze = 0) {
    const WindowScale s = WindowScaleOf(w, game_fov, units_per_metre, max_squeeze);
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
// the box. The eyes draw them themselves (Eye::dark) unless vr_window_quads
// asks for quad layers, so the tracks in front of the head can reach past the
// window's edge.
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
// camera's window (game_camera), or one of the fixed places in the venue
// (front_row, on_stage, venue: vr_venue.h's PresetAnchor, found per venue;
// vr_anchor's place until the song's venue is known), where the game's cuts
// don't move the head. Menus are seen through the window whatever this is.
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
    // venue units around the eyes clear of the crowd from the anchor
    // (Eye::crowd_clear)
    float crowd_clear = 0;
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
    float max_squeeze = 2;         // vr_window_max_squeeze, in songs (WindowScaleOf)
    float dim = 1;                 // vr_window_dim
    float world_scale = 1;
    // the frame's track cameras (TrackCamerasOf, held by TrackHold), placed
    // in songs by `track` (vr_track_*)
    TrackCams tracks;
    TrackLayout track;
    // vr_window_quads: the dark around the window as the compositor's quad
    // layers over the eyes (and the cut's fade by the compositor), not drawn
    // by the eyes
    bool window_quads = false;
};

// what the eyes show and where the quads go, for VrFrame and Present
struct FrameLayout {
    // the eyes through the game camera's window; else from the anchor
    // (`anchor`), or black: the frame has no world the eyes can see
    // (HudBoundary 0)
    bool window = false;
    bool eyes_black = false;
    Anchor anchor;
    bool anchor_follows = false;  // the game camera's (Viewpoint::follow): it cuts with it
    float fade = 0;  // the eyes darkened by this (CutFade), set by the caller
    uint32_t hud_boundary = 0;  // the frame's HudBoundary, the eyes' hud_from
    Eye eyes[2];
    GameFov game_fov;
    WindowFrame window_frame;
    float magnify = 1;  // the window's k (WindowMagnify), capped in songs
    // the game's picture's share of the window each way (WindowScale::
    // picture): 1, or less where a song's squeeze is capped, the dark then
    // around the picture (PictureFrame) rather than the window
    float picture = 1;
    // the HUD panel: shown, where, and whether that's the window itself
    bool hud = false;
    bool hud_in_window = false;
    HudQuad hud_quad;
    // the dark around the window, black at `dim`'s alpha: drawn by the eyes
    // (Eye::dark), or with frame_layers as quad layers (vr_window_quads)
    int frame_quads = 0;
    HudQuad frame[kFrameQuads];
    float dim = 0;
    bool frame_layers = false;
    // the eyes draw the cut's fade themselves (Eye::fade, set by the caller),
    // under the tracks; else the compositor fades the eyes' whole picture
    bool fade_drawn = false;
    // in songs with a world, the tracks in front of the head (PlaceTracks),
    // which each eye sees (Eye::tracks) and the panel leaves out (EyeCamsOf),
    // at track_scale units a metre (TrackToLocal)
    int track_count = 0;
    TrackPlace tracks[kMaxTrackCams];
    float track_scale = 30;
};

// RasterOptions::eye_cams for `l`'s eyes and panel (up to `max`): its track
// cameras and the eyes' dark (kEyeDarkCam); the rest 0
inline void EyeCamsOf(const FrameLayout& l, uint32_t* out, int max) {
    for (int i = 0; i < max; i++)
        out[i] = i < l.track_count ? l.tracks[i].cam.cam : i == l.track_count ? kEyeDarkCam : 0;
}

inline void QuadCorners(const HudQuad& q, float out[4][3]);

// ResolveFrame's choice of view, before the tracks
inline FrameLayout ResolveView(const Viewpoint& vp, const FrameInputs& in) {
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
        // menus' pictures fill the window whatever their field of view, so
        // the panel on it stays aligned with them
        const float cap = in.in_song ? in.max_squeeze : 0;
        const WindowScale s = WindowScaleOf(l.window_frame, l.game_fov, u, cap);
        l.magnify = WindowMagnify(s);
        l.picture = s.picture;
        const WindowFrame picture = PictureFrame(l.window_frame, s.picture);
        for (int e = 0; e < 2; e++)
            l.eyes[e] =
                WindowEye(in.views[e], in.fov[e], l.window_frame, in.game, l.game_fov, u, cap);
        if (!in.in_song || in.hud_in_window) {
            l.hud_in_window = true;
            l.hud_quad.pose = picture.pose;
            l.hud_quad.size[0] = picture.size[0];
            l.hud_quad.size[1] = picture.size[1];
        }
        l.dim = in.dim < 0 ? 0 : in.dim > 1 ? 1 : in.dim;
        // the dark reaches the picture's edge, so no clear colour shows
        // around a capped one
        if (l.dim > 0) l.frame_quads = FrameQuads(picture, l.frame);
        return l;
    }
    Anchor a = vp.anchor;
    if (vp.follow && in.camera) a = AnchorFromCamera(in.game);
    l.anchor = a;
    l.anchor_follows = vp.follow && in.camera;
    for (int e = 0; e < 2; e++) {
        l.eyes[e].pose = EyePose(in.views[e], a, in.world_scale);
        l.eyes[e].fov = in.fov[e];
        l.eyes[e].crowd_clear = l.eyes_black ? 0 : vp.crowd_clear;
    }
    return l;
}

// The frame's choice: through the window where the capture has a world
// camera and it's a menu or `vp.window` (the panel in the window, as the
// game's picture has it, in menus and with hud_in_window; on its own place in
// songs, whose shots are wider than a comfortable panel); from the anchor in
// songs otherwise; black eyes and the panel on its own place without a world.
// In songs with a world, whichever way the eyes see it, the tracks in front
// of the head (PlaceTracks) and each eye's view of them (TrackEye). Unless
// in.window_quads, the eyes draw the dark around the window themselves
// (Eye::dark), and the cut's fade.
inline FrameLayout ResolveFrame(const Viewpoint& vp, const FrameInputs& in) {
    FrameLayout l = ResolveView(vp, in);
    l.track_scale = in.track.scale;
    if (!l.eyes_black && in.in_song)
        l.track_count = PlaceTracks(in.tracks, in.head, in.track, l.tracks);
    l.frame_layers = in.window_quads && l.frame_quads > 0;
    l.fade_drawn = !in.window_quads && !l.eyes_black;
    for (int e = 0; e < 2; e++) {
        Eye& eye = l.eyes[e];
        eye.xr = in.views[e];
        eye.track_count = l.track_count;
        for (int t = 0; t < l.track_count; t++)
            eye.tracks[t] = TrackEye(in.views[e], in.fov[e], l.tracks[t], in.track.scale);
        if (in.window_quads || l.eyes_black) continue;
        eye.dark_count = l.frame_quads;
        eye.dark_alpha = l.dim;
        for (int q = 0; q < l.frame_quads; q++) QuadCorners(l.frame[q], eye.dark[q]);
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
