#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "src/Render/frame_compose.h"
#include "src/Render/scene_capture.h"

// Detects RB3's camera cuts for native_view.cpp's slow-frame log and stats: a
// new shot can bring back resources the GPU evicted (gpu_view.h's residency).
//
// A frame's world camera is the one with the most back-buffer mesh draws
// before post_boundary. Cameras are compared by picture movement, which
// doesn't depend on world units or origin as raw matrix numbers do: a 3x3
// grid at the first's median view depth reprojected by the second
// (CameraJump), and the angle between view directions (CameraTurn). A cut
// moves the picture a good part of a screen or turns tens of degrees; within
// a shot a few frames apart, a few hundredths and a few degrees (a 180
// degree/s pan: 6 degrees and 0.06 screens over two 60 Hz frames).
//
// A drop in PostParams::vel_frame (reset by CamShot::StartAnim) is a cut too,
// seen a frame or two late; CameraCuts uses it to confirm jumps.

namespace band3::render {

// a post frame's is that of the world frame it's composed with; depth is its
// draws' median clip w, the world's own scale
struct WorldCamera {
    bool valid = false;
    uint64_t world_frame = 0;  // WorldFrameOf
    Mat4 view_proj{};
    float depth = 0;
};

inline WorldCamera WorldCameraOf(const FrameCapture& fc) {
    WorldCamera c;
    // few cameras a frame, so a short list
    std::vector<std::pair<uint32_t, uint32_t>> cams;  // cam, draws
    auto world_draw = [&](size_t d) {
        const DrawItem& it = fc.draws[d];
        return d < fc.post_boundary && it.target == 0 && it.rect_shader < 0;
    };
    for (size_t d = 0; d < fc.draws.size(); d++) {
        if (!world_draw(d)) continue;
        const uint32_t cam = fc.draws[d].cam;
        auto f = std::find_if(cams.begin(), cams.end(), [&](const auto& e) { return e.first == cam; });
        if (f == cams.end())
            cams.emplace_back(cam, 1);
        else
            f->second++;
    }
    if (cams.empty()) return c;
    const uint32_t cam =
        std::max_element(cams.begin(), cams.end(),
                         [](const auto& a, const auto& b) { return a.second < b.second; })
            ->first;
    // a draw's position: its world origin, or a skinned one's first bone's
    // (its vertices are in world space)
    std::vector<float> depths;
    for (size_t d = 0; d < fc.draws.size(); d++) {
        const DrawItem& it = fc.draws[d];
        if (!world_draw(d) || it.cam != cam) continue;
        if (!c.valid) {
            c.valid = true;
            c.view_proj = it.view_proj;
        }
        const Mat4& at = it.bones.empty() ? it.world : it.bones[0];
        const float* p = at.m[3];
        const Mat4& vp = it.view_proj;
        const float w = p[0] * vp.m[0][3] + p[1] * vp.m[1][3] + p[2] * vp.m[2][3] + vp.m[3][3];
        if (w > 0 && std::isfinite(w)) depths.push_back(w);
    }
    if (depths.empty()) {
        c.valid = false;
        return c;
    }
    auto mid = depths.begin() + depths.size() / 2;
    std::nth_element(depths.begin(), mid, depths.end());
    c.depth = *mid;
    c.world_frame = WorldFrameOf(fc);
    return c;
}

// Mean grid point movement in screens (each capped at 1, as are points behind
// the camera), or -1 if it can't be told
inline float CameraJump(const WorldCamera& from, const WorldCamera& to) {
    if (!from.valid || !to.valid || !(from.depth > 0)) return -1;
    // p at screen x, y and depth D in `from`: clip x, y, w (columns 0, 1, 3)
    // are x D, y D, D, three equations in p
    const auto& a = from.view_proj.m;
    const int cols[3] = {0, 1, 3};
    double m[3][3];  // m[i][k]: coordinate i's part of column cols[k]
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) m[i][k] = a[i][cols[k]];
    // p m = r, so p = r m^-1, by its adjugate
    const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    if (!std::isfinite(det) || std::abs(det) < 1e-12) return -1;
    double inv[3][3];
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) {
            const int r0 = (k + 1) % 3, r1 = (k + 2) % 3, c0 = (i + 1) % 3, c1 = (i + 2) % 3;
            inv[i][k] = (m[r0][c0] * m[r1][c1] - m[r0][c1] * m[r1][c0]) / det;
        }
    }
    const auto& b = to.view_proj.m;
    const double depth = from.depth;
    constexpr float kGrid[3] = {-0.6f, 0.0f, 0.6f};
    double moved = 0;
    for (float y : kGrid) {
        for (float x : kGrid) {
            const double r[3] = {x * depth - a[3][0], y * depth - a[3][1], depth - a[3][3]};
            double p[3];
            for (int k = 0; k < 3; k++) p[k] = r[0] * inv[0][k] + r[1] * inv[1][k] + r[2] * inv[2][k];
            double clip[4];
            for (int c = 0; c < 4; c++) clip[c] = p[0] * b[0][c] + p[1] * b[1][c] + p[2] * b[2][c] + b[3][c];
            double d = 1;
            if (clip[3] > 1e-6 * depth) {
                // NDC is 2 across a screen
                const double dx = (clip[0] / clip[3] - x) / 2, dy = (clip[1] / clip[3] - y) / 2;
                d = std::min(1.0, std::sqrt(dx * dx + dy * dy));
            }
            if (!std::isfinite(d)) d = 1;
            moved += d;
        }
    }
    return float(moved / 9);
}

// Degrees between view directions (clip w's column), or -1 if it can't be
// told. Catches orbits around the grid's centre, which CameraJump barely sees
// (30 degrees apart moves it a twentieth of a screen).
inline float CameraTurn(const WorldCamera& from, const WorldCamera& to) {
    if (!from.valid || !to.valid) return -1;
    double a[3], b[3], la = 0, lb = 0, dot = 0;
    for (int i = 0; i < 3; i++) {
        a[i] = from.view_proj.m[i][3];
        b[i] = to.view_proj.m[i][3];
        la += a[i] * a[i];
        lb += b[i] * b[i];
        dot += a[i] * b[i];
    }
    if (!(la > 0) || !(lb > 0) || !std::isfinite(dot)) return -1;
    const double c = std::clamp(dot / std::sqrt(la * lb), -1.0, 1.0);
    return float(std::acos(c) * 180 / 3.14159265358979323846);
}

// cut thresholds: screens (CameraJump), degrees (CameraTurn)
inline constexpr float kCameraCutJump = 0.15f;
inline constexpr float kCameraCutTurn = 15.0f;
// worlds further apart aren't compared: the camera may move that much in a shot
inline constexpr uint64_t kCameraCutMaxGap = 8;

// Fed the frames drawn in order (the worker skips some while busy).
class CameraCuts {
 public:
    struct Step {
        // world_frame 0: no world camera. jump and turn are from the last
        // world `gap` game frames before, -1 if not compared.
        uint64_t world_frame = 0;
        bool new_world = false;
        float jump = -1, turn = -1;
        uint64_t gap = 0;
        bool cut = false;
        // vel_reset: vel_frame dropped; confirmed: within kCameraCutMaxGap of
        // an unconfirmed cut
        bool vel_valid = false;
        uint32_t vel_frame = 0;
        bool vel_reset = false;
        bool confirmed = false;
        // counted from the cut's world frame (a post frame composed with it is
        // 1), -1 none seen yet
        int64_t since_cut = -1;
    };

    Step Next(const FrameCapture& fc) {
        const bool vel = (fc.proc_cmds & kProcPost) && fc.post.valid && fc.post.vel_read;
        return Next(WorldCameraOf(fc), fc.game_frame, vel, fc.post.vel_frame);
    }

    Step Next(const WorldCamera& cam, uint64_t game_frame, bool vel_valid, uint32_t vel_frame) {
        Step s;
        if (cam.valid) s.world_frame = cam.world_frame;
        if (cam.valid && (!last_.valid || cam.world_frame != last_.world_frame)) {
            s.new_world = true;
            // a world older than the last: captures started over; compare none
            if (last_.valid && cam.world_frame > last_.world_frame) {
                s.gap = cam.world_frame - last_.world_frame;
                if (s.gap <= kCameraCutMaxGap) {
                    s.jump = CameraJump(last_, cam);
                    s.turn = CameraTurn(last_, cam);
                    s.cut = s.jump > kCameraCutJump || s.turn > kCameraCutTurn;
                }
            }
            if (s.cut) {
                last_cut_ = cam.world_frame;
                unconfirmed_ = true;
            }
            last_ = cam;
        }
        if (vel_valid) {
            s.vel_valid = true;
            s.vel_frame = vel_frame;
            s.vel_reset = has_vel_ && vel_frame < last_vel_;
            if (s.vel_reset && unconfirmed_ && last_cut_ && game_frame >= last_cut_ &&
                game_frame - last_cut_ <= kCameraCutMaxGap) {
                s.confirmed = true;
                unconfirmed_ = false;
            }
            has_vel_ = true;
            last_vel_ = vel_frame;
        }
        if (last_cut_ && game_frame >= last_cut_) s.since_cut = int64_t(game_frame - last_cut_);
        return s;
    }

 private:
    WorldCamera last_;
    uint64_t last_cut_ = 0;    // the cut's world frame, 0 none
    bool unconfirmed_ = false; // its vel_frame reset not seen yet
    bool has_vel_ = false;
    uint32_t last_vel_ = 0;
};

}  // namespace band3::render
