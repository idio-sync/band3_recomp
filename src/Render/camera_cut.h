#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include "src/Render/frame_compose.h"
#include "src/Render/scene_capture.h"

// Experimental: tells where RB3's world camera cut to another shot, from the
// captures the native renderer draws, for its slow-frame log and its numbers
// (native_view.cpp): a new shot can bring back characters, render targets and
// textures the GPU let go of while they weren't seen (gpu_view.h's residency),
// which that frame then makes again.
//
// The world camera of a frame is the one with the most of the back buffer's
// world draws (before post_boundary, mesh draws only: the overlay, the
// DrawRect quads and the texture passes have cameras of their own), its
// view-projection a draw's. Two worlds' cameras are compared by where the
// first's picture lands in the second's: a 3x3 grid of points across the
// first's screen, at its draws' median view depth, projected by the second's
// (CameraJump), and by the angle between their view directions (CameraTurn).
// That's the picture's own movement, the same whatever the world's units or
// where its origin is, which comparing the matrices' numbers isn't (their
// translation, the world origin's place in view, outweighs the rest, and two
// shots framing the stage's centre from two sides keep it). A cut moves the
// picture a good part of a screen or turns the view tens of degrees; the
// camera within a shot, between two world frames a few game frames apart,
// moves it a few hundredths of one and turns a few degrees (a 180
// degree-a-second pan turns 6 degrees in two frames at 60 Hz, and moves a 50
// degree view's picture 0.06 of a screen).
//
// RB3's motion blur counts the frames since its shot started
// (PostParams::vel_frame, CamShot::StartAnim resetting it), on the frames that
// post-process with it read: one smaller than the last such frame's is a cut
// too, seen a frame or two late, which CameraCuts uses to check the jumps.

namespace band3::render {

// a frame's world camera, valid if it has one (a menu's or a world frame's;
// a post frame's is the world frame's it's composed with): its view-projection
// and its draws' median view depth (clip w), the world's own scale
struct WorldCamera {
    bool valid = false;
    uint64_t world_frame = 0;  // WorldFrameOf
    Mat4 view_proj{};
    float depth = 0;
};

inline WorldCamera WorldCameraOf(const FrameCapture& fc) {
    WorldCamera c;
    // draws by camera: most cameras draw a few runs a frame, so a short list
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
    // where its draws are: an unskinned draw's world origin, a skinned one's
    // first bone's (vertices in world space have the world's origin)
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

// How far `from`'s picture moved in `to`'s: the mean distance its grid's
// points moved on the screen, in screens (1 the whole width or height; a point
// that went behind the camera or off by more counts 1), or -1 if it can't be
// told (no depth, or a view-projection that sees no 3D)
inline float CameraJump(const WorldCamera& from, const WorldCamera& to) {
    if (!from.valid || !to.valid || !(from.depth > 0)) return -1;
    // a point p at screen x, y and view depth D in `from`: clip x, y and w
    // (columns 0, 1, 3) are x D, y D and D, three equations in p's three
    // coordinates
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

// The angle in degrees between `from`'s view direction and `to`'s, or -1 if
// it can't be told: clip w is the view depth, so its column is the direction
// the camera looks in. CameraJump's grid, on a plane facing the camera, moves
// little when the camera circles its middle (two shots framing the stage's
// centre from 30 degrees apart move it a twentieth of a screen); this sees it.
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

// a jump bigger than this is a cut (CameraJump: screens), and a turn
// (CameraTurn: degrees): within a shot the camera moves and turns far less
// between two worlds kCameraCutMaxGap game frames apart at most
inline constexpr float kCameraCutJump = 0.15f;
inline constexpr float kCameraCutTurn = 15.0f;
// two worlds further apart than this many game frames aren't compared: the
// camera may have moved that much within a shot
inline constexpr uint64_t kCameraCutMaxGap = 8;

// The camera's cuts over the frames drawn, in order (the worker's; it skips
// captures while busy, so not every frame).
class CameraCuts {
 public:
    // what a frame drawn showed of the camera
    struct Step {
        // the game frame of its world (WorldCamera::world_frame), 0 if it has
        // no world camera; whether that's a world not seen before (not a post
        // frame's whose world frame was drawn, nor a frame drawn again), and
        // how far its camera jumped and turned from the last world's `gap`
        // game frames before (-1 not compared: the first, or too far apart,
        // or it can't be told), and whether that was a cut
        uint64_t world_frame = 0;
        bool new_world = false;
        float jump = -1, turn = -1;
        uint64_t gap = 0;
        bool cut = false;
        // vel_frame, if this frame read it (vel_valid); a reset (smaller than
        // the last read), and whether that was within kCameraCutMaxGap game
        // frames after a cut that wasn't confirmed yet (the jump's cut
        // confirmed)
        bool vel_valid = false;
        uint32_t vel_frame = 0;
        bool vel_reset = false;
        bool confirmed = false;
        // game frames since the last cut (0 at one; its world's game frame,
        // so a post frame composed with a cut's world is 1), -1 none seen yet
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
