// Checks src/Render/vr_pose.h, VR's eyes for the native renderer: a headset's
// field of view as a projection, XR's axes and units as Milo's, an anchor's
// place and turn, the eyes' offset, and RePose: a captured frame seen from an
// eye, its world camera's draws and shades moved and nothing else touched.

#include <doctest/doctest.h>
#include <cmath>
#include <cstring>
#include <memory>
#include "src/Render/free_cam.h"
#include "src/Render/soft_raster.h"
#include "src/Render/vr_pose.h"

using namespace band3::render;

namespace {

constexpr float kPi = 3.14159265f;

// a captured camera's kind of projection: 90 degrees across, z = 1.001 w - 10
Mat4 GameProj() {
    Mat4 p{};
    p.m[0][0] = 1;          // x
    p.m[2][1] = 16.0f / 9;  // z (up) to clip y
    p.m[1][2] = 1.001f;     // depth from the distance
    p.m[3][2] = -10.01f;
    p.m[1][3] = 1;  // w: the distance in front
    return p;
}

// camera space (x right, y forward, z up) through proj, to normalized x, y
void Ndc(const Mat4& proj, float x, float y, float z, float& nx, float& ny) {
    const float p[3] = {x, y, z};
    float c[4];
    freecam::Transform(p, proj, c);
    nx = c[0] / c[3];
    ny = c[1] / c[3];
}

void CheckVec(const float a[3], float x, float y, float z, float eps = 1e-4f) {
    CHECK(a[0] == doctest::Approx(x).epsilon(eps));
    CHECK(a[1] == doctest::Approx(y).epsilon(eps));
    CHECK(a[2] == doctest::Approx(z).epsilon(eps));
}

}  // namespace

TEST_CASE("an eye's field of view: its edges at the picture's, depth as captured") {
    const vr::Fov fov{-0.9f, 0.7f, 0.8f, -1.0f};  // asymmetric, as headsets are
    const Mat4 proj = vr::EyeProjection(GameProj(), fov);
    float nx, ny;
    const float d = 250;  // any distance in front
    Ndc(proj, std::tan(fov.left) * d, d, std::tan(fov.up) * d, nx, ny);
    CHECK(nx == doctest::Approx(-1));
    CHECK(ny == doctest::Approx(1));
    Ndc(proj, std::tan(fov.right) * d, d, std::tan(fov.down) * d, nx, ny);
    CHECK(nx == doctest::Approx(1));
    CHECK(ny == doctest::Approx(-1));
    // columns 2 and 3 kept
    for (int r = 0; r < 4; r++) {
        CHECK(proj.m[r][2] == GameProj().m[r][2]);
        CHECK(proj.m[r][3] == GameProj().m[r][3]);
    }
}

TEST_CASE("a symmetric eye's view_proj splits back into its pose and projection") {
    const float eye[3] = {0, -1250, 520}, at[3] = {0, -540, 290};
    const freecam::Pose pose = freecam::LookAt(eye, at);
    const vr::Fov fov{-0.7854f, 0.7854f, 0.7854f, -0.7854f};
    const Mat4 proj = vr::EyeProjection(GameProj(), fov);
    const Mat4 vp = freecam::Mul(freecam::View(pose), proj);
    freecam::Pose back{};
    Mat4 back_proj{};
    REQUIRE(freecam::Decompose(vp, back, back_proj));
    CheckVec(back.eye, eye[0], eye[1], eye[2], 1e-3f);
    CheckVec(back.fwd, pose.fwd[0], pose.fwd[1], pose.fwd[2]);
    CheckVec(back.right, pose.right[0], pose.right[1], pose.right[2]);
    CheckVec(back.up, pose.up[0], pose.up[1], pose.up[2]);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            // float maths at a venue's distances: a thousandth absolute
            CHECK(std::fabs(back_proj.m[r][c] - proj.m[r][c]) < 1e-3f);
}

TEST_CASE("XR's axes are Milo's: right +x, forward +y, up +z, metres to inches") {
    vr::XrPose x;  // facing XR's -z, at 1 m right, 1.6 m up, 2 m back
    x.p[0] = 1;
    x.p[1] = 1.6f;
    x.p[2] = 2;
    const freecam::Pose p = vr::EyePose(x, vr::Anchor{});
    CheckVec(p.right, 1, 0, 0);
    CheckVec(p.fwd, 0, 1, 0);
    CheckVec(p.up, 0, 0, 1);
    CheckVec(p.eye, 39.37f, -2 * 39.37f, 1.6f * 39.37f);
}

TEST_CASE("a head turned left in XR looks left in the venue") {
    vr::XrPose x;  // 90 degrees about XR's up (+y), counterclockwise from above
    x.q[1] = std::sin(kPi / 4);
    x.q[3] = std::cos(kPi / 4);
    const freecam::Pose p = vr::EyePose(x, vr::Anchor{});
    CheckVec(p.fwd, -1, 0, 0);
    CheckVec(p.right, 0, 1, 0);
    CheckVec(p.up, 0, 0, 1);
}

TEST_CASE("the anchor places and turns the head") {
    vr::Anchor a;
    REQUIRE(vr::ParseAnchor("0, -1250, 520, 90", a));
    CheckVec(a.pos, 0, -1250, 520);
    CHECK(a.yaw == doctest::Approx(kPi / 2));
    vr::XrPose x;
    x.p[2] = -1;  // a metre forward
    const freecam::Pose p = vr::EyePose(x, a);
    CheckVec(p.fwd, -1, 0, 0);
    CheckVec(p.eye, -39.37f, -1250, 520);
    // a world scale moves the head further
    const freecam::Pose big = vr::EyePose(x, a, 10);
    CheckVec(big.eye, -393.7f, -1250, 520);
    CHECK_FALSE(vr::ParseAnchor("", a));
    CHECK_FALSE(vr::ParseAnchor("1,2,3", a));
    CHECK_FALSE(vr::ParseAnchor("1,2,3,4,5", a));
}

TEST_CASE("the eyes sit an IPD apart along the head's right") {
    vr::Anchor a;
    REQUIRE(vr::ParseAnchor("100,200,300,30", a));
    vr::XrPose l, r;
    l.p[0] = -0.0315f;
    r.p[0] = 0.0315f;
    const freecam::Pose pl = vr::EyePose(l, a), pr = vr::EyePose(r, a);
    const float d[3] = {pr.eye[0] - pl.eye[0], pr.eye[1] - pl.eye[1], pr.eye[2] - pl.eye[2]};
    CHECK(freecam::Len3(d) == doctest::Approx(0.063f * 39.37f));
    // along right, which the anchor turned 30 degrees
    CheckVec(pl.right, std::cos(kPi / 6), std::sin(kPi / 6), 0);
    CHECK(freecam::Dot3(d, pl.right) == doctest::Approx(0.063f * 39.37f));
}

TEST_CASE("an anchor from the game's camera: its point, facing its forward on the floor") {
    const float eye[3] = {10, -900, 400}, at[3] = {300, -600, 100};
    const vr::Anchor a = vr::AnchorFromCamera(freecam::LookAt(eye, at));
    CheckVec(a.pos, 10, -900, 400);
    const freecam::Pose p = vr::EyePose(vr::XrPose{}, a);
    const float flat = std::sqrt(290.0f * 290.0f + 300.0f * 300.0f);
    CheckVec(p.fwd, 290 / flat, 300 / flat, 0);
}

namespace {

// a frame: world camera A's three draws (one a billboard), a texture pass's
// draw through A, a second camera's HUD draw after post_boundary
FrameCapture Frame(Mat4& vp_a, Mat4& vp_hud) {
    const float eye[3] = {0, -1000, 200}, at[3] = {0, 0, 200};
    vp_a = freecam::Mul(freecam::View(freecam::LookAt(eye, at)), GameProj());
    vp_hud = GameProj();
    vp_hud.m[3][0] = 5;  // something else
    FrameCapture f;
    ShadeState plain{}, card{}, hud{};
    card.options = 1ull << shader_opt::kBillboard;
    for (ShadeState* s : {&plain, &card, &hud}) {
        s->eye[1] = -1000;
        s->eye[2] = 200;
    }
    f.shades = {plain, card, hud};
    auto add = [&](const Mat4& vp, uint32_t cam, int32_t shade, uint32_t target) {
        DrawItem d{};
        d.view_proj = vp;
        d.cam = cam;
        d.shade = shade;
        d.target = target;
        for (int i = 0; i < 4; i++) d.world.m[i][i] = 1;
        d.world.m[3][1] = 100;  // in front of camera A
        f.draws.push_back(d);
    };
    add(vp_a, 1, 0, 0);
    add(vp_a, 1, 1, 0);
    add(vp_a, 1, 0, 0);
    add(vp_a, 1, 0, 0x1234);  // a texture pass's, seen through the world camera
    f.post_boundary = 4;
    add(vp_hud, 2, 2, 0);
    return f;
}

}  // namespace

TEST_CASE("RePose: the world camera's draws see from the eye, nothing else moves") {
    Mat4 vp_a, vp_hud;
    const FrameCapture in = Frame(vp_a, vp_hud);
    // copies (a FrameCapture's copy shares its shades)
    const std::vector<DrawItem> draws_before = in.draws;
    std::vector<ShadeInputs> shades_before;
    for (const ShadeState& s : in.shades) shades_before.push_back(s);
    vr::Eye eye;
    const float at_eye[3] = {50, -1250, 520}, look[3] = {0, -540, 290};
    eye.pose = freecam::LookAt(at_eye, look);
    eye.fov = {-0.8f, 0.8f, 0.8f, -0.8f};
    FrameCapture out;
    const vr::RePoseStats st = vr::RePose(in, eye, out);
    REQUIRE(st.ok);
    CHECK(st.cam == 1);
    CHECK(st.draws_moved == 4);
    CHECK(st.shades_cloned == 2);
    CheckVec(st.game.eye, 0, -1000, 200, 1e-3f);
    const Mat4 want = freecam::Mul(freecam::View(eye.pose), vr::EyeProjection(GameProj(), eye.fov));
    for (size_t d = 0; d < 4; d++) {
        CAPTURE(d);
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++)
                CHECK(std::fabs(out.draws[d].view_proj.m[r][c] - want.m[r][c]) <
                      1e-3f * (1 + std::fabs(want.m[r][c])));
    }
    // the HUD as it was
    CHECK(std::memcmp(&out.draws[4].view_proj, &vp_hud, sizeof(Mat4)) == 0);
    CHECK(out.draws[4].shade == 2);
    // cloned shades: the eye's position, and the card's axes
    REQUIRE(out.shades.size() == 5);
    CHECK(out.draws[0].shade == out.draws[2].shade);
    CHECK(out.draws[0].shade >= 3);
    const ShadeState& plain = out.shades[size_t(out.draws[0].shade)];
    const ShadeState& card = out.shades[size_t(out.draws[1].shade)];
    CheckVec(plain.eye, 50, -1250, 520);
    CheckVec(card.eye, 50, -1250, 520);
    for (int k = 0; k < 3; k++) {
        CHECK(card.Vs(16 + k)[0] == doctest::Approx(eye.pose.right[k]));
        CHECK(card.Vs(16 + k)[1] == doctest::Approx(eye.pose.up[k]));
        CHECK(card.Vs(16 + k)[2] == doctest::Approx(eye.pose.fwd[k]));
    }
    // the source untouched, its shades shared and unchanged
    REQUIRE(in.shades.size() == 3);
    for (size_t s = 0; s < 3; s++) {
        CHECK(std::memcmp(static_cast<const ShadeInputs*>(&in.shades[s]), &shades_before[s],
                          sizeof(ShadeInputs)) == 0);
        CHECK(in.shades[s].eye[1] == -1000);
    }
    REQUIRE(in.draws.size() == draws_before.size());
    for (size_t d = 0; d < in.draws.size(); d++) {
        CHECK(std::memcmp(&in.draws[d].view_proj, &draws_before[d].view_proj, sizeof(Mat4)) == 0);
        CHECK(in.draws[d].shade == draws_before[d].shade);
    }
}

TEST_CASE("RePose without a perspective world camera leaves the frame as it is") {
    FrameCapture in;
    DrawItem d{};
    d.cam = 1;
    in.draws.push_back(d);  // all-zero view_proj: no camera to split
    in.post_boundary = 1;
    FrameCapture out;
    vr::Eye eye;
    CHECK_FALSE(vr::RePose(in, eye, out).ok);
    REQUIRE(out.draws.size() == 1);
    CHECK(std::memcmp(&out.draws[0].view_proj, &in.draws[0].view_proj, sizeof(Mat4)) == 0);
}

TEST_CASE("skip_overlay leaves out the back buffer's draws from post_boundary on") {
    FrameCapture f;
    f.post_boundary = 10;
    RasterOptions o;
    CHECK_FALSE(SkipsOverlay(o, f, 12));
    o.skip_overlay = true;
    CHECK_FALSE(SkipsOverlay(o, f, 9));
    CHECK(SkipsOverlay(o, f, 10));
    CHECK(SkipsOverlay(o, f, 500));
    // a frame without post-processing has no overlay to leave out
    f.post_boundary = FrameCapture::kNoPost;
    CHECK_FALSE(SkipsOverlay(o, f, 500));
}
