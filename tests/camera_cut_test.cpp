// Checks src/Render/camera_cut.h: the world camera a capture's draws have,
// how far it moved between two worlds, and the cuts CameraCuts finds over a
// run of frames, with vel_frame's resets confirming them.

#include <doctest/doctest.h>
#include <cmath>
#include <cstdint>
#include <memory>
#include "src/Render/camera_cut.h"

using namespace band3::render;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct V3 {
    double x, y, z;
};
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Unit(V3 a) {
    const double l = std::sqrt(Dot(a, a));
    return {a.x / l, a.y / l, a.z / l};
}

// a camera at `eye` looking at `at` (z up), `fov_y` degrees, 16:9, as RB3's
// view-projection: row vectors, D3D's perspective (w the view depth)
Mat4 ViewProj(V3 eye, V3 at, double fov_y = 50) {
    const V3 f = Unit(Sub(at, eye));
    const V3 r = Unit(Cross(V3{0, 0, 1}, f));
    const V3 u = Cross(f, r);
    const double view[4][4] = {{r.x, u.x, f.x, 0},
                               {r.y, u.y, f.y, 0},
                               {r.z, u.z, f.z, 0},
                               {-Dot(r, eye), -Dot(u, eye), -Dot(f, eye), 1}};
    const double sy = 1 / std::tan(fov_y * kPi / 360), sx = sy * 9 / 16;
    const double n = 10, fz = 10000;
    const double proj[4][4] = {
        {sx, 0, 0, 0}, {0, sy, 0, 0}, {0, 0, fz / (fz - n), 1}, {0, 0, -n * fz / (fz - n), 0}};
    Mat4 m{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += view[i][k] * proj[k][j];
            m.m[i][j] = float(s);
        }
    return m;
}

// a mesh draw by camera `cam` with `vp`, placed at `pos`
DrawItem Draw(uint32_t cam, const Mat4& vp, V3 pos) {
    DrawItem d{};
    d.cam = cam;
    d.view_proj = vp;
    for (int i = 0; i < 4; i++) d.world.m[i][i] = 1;
    d.world.m[3][0] = float(pos.x);
    d.world.m[3][1] = float(pos.y);
    d.world.m[3][2] = float(pos.z);
    return d;
}

// a stage's worth of draws around the origin, seen by `vp`
FrameCapture Stage(const Mat4& vp, uint64_t game_frame) {
    FrameCapture fc;
    fc.game_frame = game_frame;
    for (int i = -3; i <= 3; i++) fc.draws.push_back(Draw(7, vp, {i * 60.0, 0, 0}));
    return fc;
}

WorldCamera Camera(const Mat4& vp, uint64_t world_frame) {
    WorldCamera c = WorldCameraOf(Stage(vp, world_frame));
    return c;
}

// RB3's camera 800 units in front of the stage, a little above it
const V3 kEye{0, -800, 150};
const V3 kAt{0, 0, 80};

}  // namespace

TEST_CASE("the world camera is the one with the most of the back buffer's world draws") {
    const Mat4 world = ViewProj(kEye, kAt);
    const Mat4 other = ViewProj({500, -500, 300}, kAt);
    FrameCapture fc = Stage(world, 100);
    // a camera with fewer, a texture pass's, a DrawRect quad, and the
    // overlay's after post-processing started: none of them the world's
    fc.draws.push_back(Draw(9, other, {0, 0, 0}));
    for (int i = 0; i < 10; i++) {
        DrawItem pass = Draw(5, other, {0, 0, 0});
        pass.target = 0x1234;
        fc.draws.push_back(pass);
        DrawItem rect = Draw(5, other, {0, 0, 0});
        rect.rect_shader = 6;
        fc.draws.push_back(rect);
    }
    fc.post_boundary = uint32_t(fc.draws.size());
    for (int i = 0; i < 20; i++) fc.draws.push_back(Draw(11, other, {0, 0, 0}));
    const WorldCamera c = WorldCameraOf(fc);
    REQUIRE(c.valid);
    CHECK(c.world_frame == 100);
    CHECK(c.view_proj.m[3][0] == world.m[3][0]);
    CHECK(c.view_proj.m[3][3] == world.m[3][3]);
    // the stage's draws are all 810 units deep in its view
    CHECK(c.depth == doctest::Approx(810).epsilon(0.001));

    // a post frame composed with a world frame's world: that frame's
    fc.composed = 1;
    fc.world_frame = 99;
    CHECK(WorldCameraOf(fc).world_frame == 99);
    // a frame with no world draws has no world camera
    FrameCapture overlay;
    overlay.post_boundary = 0;
    overlay.draws.push_back(Draw(11, other, {0, 0, 0}));
    CHECK_FALSE(WorldCameraOf(overlay).valid);
}

TEST_CASE("the camera moving within a shot is no cut; another shot is") {
    const WorldCamera a = Camera(ViewProj(kEye, kAt), 10);
    CHECK(CameraJump(a, a) == doctest::Approx(0).epsilon(1e-4));
    CHECK(CameraTurn(a, a) == doctest::Approx(0).epsilon(1e-2));

    // within a shot, two game frames on: a fast pan (6 degrees, 180 a
    // second), a dolly and a zoom of a few percent, a crane move
    const V3 panned_at{800 * std::sin(6 * kPi / 180), -800 + 800 * std::cos(6 * kPi / 180), 80};
    for (const Mat4& vp : {ViewProj(kEye, panned_at), ViewProj({0, -780, 150}, kAt),
                           ViewProj(kEye, kAt, 48.5), ViewProj({15, -800, 165}, {10, 0, 85})}) {
        const WorldCamera b = Camera(vp, 12);
        CHECK(CameraJump(a, b) < kCameraCutJump);
        CHECK(CameraTurn(a, b) < kCameraCutTurn);
    }

    // cuts: to a close-up of the same middle, to another part of the stage,
    // to the stage from 30 degrees round (the grid hardly moves; the view
    // turns), and to the stage from behind (the grid mirrored: its middle
    // column stays)
    const double s30 = std::sin(30 * kPi / 180), c30 = std::cos(30 * kPi / 180);
    const WorldCamera close_up = Camera(ViewProj({0, -250, 120}, kAt), 12);
    CHECK(CameraJump(a, close_up) > kCameraCutJump);
    const WorldCamera elsewhere = Camera(ViewProj({-300, -400, 120}, {-200, 0, 80}), 12);
    CHECK(CameraJump(a, elsewhere) > kCameraCutJump);
    const WorldCamera round = Camera(ViewProj({800 * s30, -800 * c30, 150}, kAt), 12);
    CHECK(CameraTurn(a, round) > kCameraCutTurn);
    const WorldCamera back = Camera(ViewProj({0, 800, 150}, kAt), 12);
    CHECK(CameraJump(a, back) > kCameraCutJump);
    CHECK(CameraTurn(a, back) > 160);
    // and a point behind the camera counts a whole screen
    const WorldCamera away{true, 12, ViewProj({0, 900, 150}, {0, 1800, 80}), 800};
    CHECK(CameraJump(a, away) == doctest::Approx(1));

    // nothing to tell from
    CHECK(CameraJump(WorldCamera{}, a) == -1);
    CHECK(CameraTurn(a, WorldCamera{}) == -1);
}

TEST_CASE("CameraCuts finds a cut once, counts the frames since it, and checks it by vel_frame") {
    CameraCuts cuts;
    const Mat4 wide = ViewProj(kEye, kAt), close_up = ViewProj({0, -250, 120}, kAt);
    // the first world: nothing to compare with
    CameraCuts::Step s = cuts.Next(Camera(wide, 100), 100, false, 0);
    CHECK(s.new_world);
    CHECK(s.jump == -1);
    CHECK_FALSE(s.cut);
    CHECK(s.since_cut == -1);
    // the post frame composed with it: the same world, nothing new
    s = cuts.Next(Camera(wide, 100), 101, true, 40);
    CHECK_FALSE(s.new_world);
    CHECK(s.world_frame == 100);
    CHECK_FALSE(s.vel_reset);
    // the next world, the same shot
    s = cuts.Next(Camera(wide, 102), 102, false, 0);
    CHECK(s.new_world);
    CHECK(s.gap == 2);
    CHECK(s.jump >= 0);
    CHECK_FALSE(s.cut);
    s = cuts.Next(Camera(wide, 102), 103, true, 41);
    CHECK_FALSE(s.vel_reset);
    // a cut: at its world frame, then one and two game frames after
    s = cuts.Next(Camera(close_up, 104), 104, false, 0);
    CHECK(s.cut);
    CHECK(s.since_cut == 0);
    // its post frame: vel_frame starts over, confirming it
    s = cuts.Next(Camera(close_up, 104), 105, true, 0);
    CHECK_FALSE(s.cut);
    CHECK(s.since_cut == 1);
    CHECK(s.vel_reset);
    CHECK(s.confirmed);
    s = cuts.Next(Camera(close_up, 106), 106, false, 0);
    CHECK_FALSE(s.cut);
    CHECK(s.since_cut == 2);
    s = cuts.Next(Camera(close_up, 106), 107, true, 1);
    CHECK_FALSE(s.vel_reset);
    CHECK(s.since_cut == 3);

    // a reset long after the cut, with no jump seen: counted, confirms nothing
    s = cuts.Next(Camera(close_up, 140), 141, true, 0);
    CHECK(s.vel_reset);
    CHECK_FALSE(s.confirmed);
    // worlds too far apart (the worker skipped them) aren't compared
    CHECK(s.gap == 34);
    CHECK(s.jump == -1);
    CHECK_FALSE(s.cut);
    // nor a world from before the last (captures started over)
    s = cuts.Next(Camera(wide, 50), 50, false, 0);
    CHECK(s.new_world);
    CHECK(s.jump == -1);
    CHECK_FALSE(s.cut);
}
