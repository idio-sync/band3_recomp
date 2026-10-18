#pragma once

// A camera's pose and projection apart: the replay's free camera
// (tools/native_view_replay) and VR's eyes (vr_pose.h). A captured camera's
// view_proj (row vectors, v' = v * VP) is its view (the inverse of its world
// transform) times its projection. Milo's camera looks down its local +y with
// +z up, so the projection sends camera-space x to clip x, z to clip y and y
// (the distance in front) to w: VP's column 0 is the right axis times the x
// scale, column 1 the up axis times the y scale, column 3 the forward axis.
// Splitting it gives the pose and the projection apart, so the same projection
// can be put anywhere in the world.

#include <cmath>

#include "src/Render/scene_capture.h"

namespace band3::render::freecam {

struct Pose {
    float right[3], fwd[3], up[3], eye[3];
};

inline float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline float Len3(const float a[3]) { return std::sqrt(Dot3(a, a)); }

inline Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

// the view matrix of a camera at pose p: world -> camera space (x right, y
// forward, z up)
inline Mat4 View(const Pose& p) {
    Mat4 v{};
    for (int r = 0; r < 3; r++) {
        v.m[r][0] = p.right[r];
        v.m[r][1] = p.fwd[r];
        v.m[r][2] = p.up[r];
    }
    v.m[3][0] = -Dot3(p.eye, p.right);
    v.m[3][1] = -Dot3(p.eye, p.fwd);
    v.m[3][2] = -Dot3(p.eye, p.up);
    v.m[3][3] = 1;
    return v;
}

// its inverse, camera space -> world
inline Mat4 World(const Pose& p) {
    Mat4 w{};
    for (int c = 0; c < 3; c++) {
        w.m[0][c] = p.right[c];
        w.m[1][c] = p.fwd[c];
        w.m[2][c] = p.up[c];
        w.m[3][c] = p.eye[c];
    }
    w.m[3][3] = 1;
    return w;
}

// Splits vp into the camera's pose and its projection (proj = View(pose)^-1 * vp).
// False if it isn't a perspective camera's (forward not a unit vector). An
// off-centre projection (a lens shift: two players' track cameras, each
// shifted toward its half of the picture) puts some of the forward axis in
// columns 0 and 1 (clip x = sx x + ox y); that part is taken out to leave
// the right and up axes, and stays in proj (m[1][0], m[1][1]).
inline bool Decompose(const Mat4& vp, Pose& pose, Mat4& proj) {
    float fl = 0;
    for (int r = 0; r < 3; r++) fl += vp.m[r][3] * vp.m[r][3];
    fl = std::sqrt(fl);
    if (std::fabs(fl - 1) > 0.01f) return false;
    for (int r = 0; r < 3; r++) pose.fwd[r] = vp.m[r][3] / fl;
    // the shifts: columns 0 and 1 along the forward axis
    float ox = 0, oy = 0;
    for (int r = 0; r < 3; r++) {
        ox += vp.m[r][0] * pose.fwd[r];
        oy += vp.m[r][1] * pose.fwd[r];
    }
    float sx = 0, sy = 0;
    for (int r = 0; r < 3; r++) {
        pose.right[r] = vp.m[r][0] - ox * pose.fwd[r];
        pose.up[r] = vp.m[r][1] - oy * pose.fwd[r];
        sx += pose.right[r] * pose.right[r];
        sy += pose.up[r] * pose.up[r];
    }
    sx = std::sqrt(sx), sy = std::sqrt(sy);
    if (!(sx > 0) || !(sy > 0)) return false;
    for (int r = 0; r < 3; r++) {
        pose.right[r] /= sx;
        pose.up[r] /= sy;
    }
    // the translation row: vp.m[3][j] = -(eye . axis_j) times the scale,
    // less the shift times eye . forward
    const float ey = -vp.m[3][3] / fl;
    const float ex = -(vp.m[3][0] + ox * ey) / sx, ez = -(vp.m[3][1] + oy * ey) / sy;
    for (int c = 0; c < 3; c++) pose.eye[c] = ex * pose.right[c] + ey * pose.fwd[c] + ez * pose.up[c];
    proj = Mul(World(pose), vp);
    return true;
}

// a pose at eye looking at target, with world +z up
inline Pose LookAt(const float eye[3], const float target[3]) {
    Pose p{};
    float f[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const float lf = Len3(f);
    for (float& x : f) x /= lf;
    const float z[3] = {0, 0, 1};
    // right = fwd x up
    float r[3] = {f[1] * z[2] - f[2] * z[1], f[2] * z[0] - f[0] * z[2], f[0] * z[1] - f[1] * z[0]};
    const float lr = Len3(r);
    for (float& x : r) x /= lr;
    // up = right x fwd
    const float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    for (int c = 0; c < 3; c++) {
        p.right[c] = r[c];
        p.fwd[c] = f[c];
        p.up[c] = u[c];
        p.eye[c] = eye[c];
    }
    return p;
}

// a projection with its x and y scales divided by `wider` (2: twice the
// tangent of the half angle)
inline Mat4 Widen(Mat4 proj, float wider) {
    for (int r = 0; r < 4; r++) {
        proj.m[r][0] /= wider;
        proj.m[r][1] /= wider;
    }
    return proj;
}

// which clip-space planes a point is outside of: bit 0 x<-w, 1 x>w, 2 y<-w,
// 3 y>w, 4 z<0 (in front of the near plane), 5 z>w
inline unsigned Outcode(const float c[4]) {
    unsigned o = 0;
    if (c[0] < -c[3]) o |= 1;
    if (c[0] > c[3]) o |= 2;
    if (c[1] < -c[3]) o |= 4;
    if (c[1] > c[3]) o |= 8;
    if (c[2] < 0) o |= 16;
    if (c[2] > c[3]) o |= 32;
    return o;
}

inline void Transform(const float p[3], const Mat4& m, float out[4]) {
    for (int c = 0; c < 4; c++)
        out[c] = p[0] * m.m[0][c] + p[1] * m.m[1][c] + p[2] * m.m[2][c] + m.m[3][c];
}

// a draw's vertex in the world: skinned with its bones' weights, else by its
// world transform
inline void WorldPos(const DrawItem& d, const Vertex& v, float out[3]) {
    float t[4];
    if (d.bones.empty()) {
        Transform(v.pos, d.world, t);
        for (int c = 0; c < 3; c++) out[c] = t[c];
        return;
    }
    float total = 0;
    out[0] = out[1] = out[2] = 0;
    for (int k = 0; k < 4; k++) {
        const float w = v.weight[k];
        if (w <= 0) continue;
        const Mat4& b = d.bones[v.bone[k] < d.bones.size() ? v.bone[k] : 0];
        Transform(v.pos, b, t);
        for (int c = 0; c < 3; c++) out[c] += t[c] * w;
        total += w;
    }
    if (total <= 0) {
        Transform(v.pos, d.bones[0], t);
        for (int c = 0; c < 3; c++) out[c] = t[c];
    }
}

}  // namespace band3::render::freecam
