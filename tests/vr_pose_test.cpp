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

TEST_CASE("the eyes and the HUD panel split the back buffer's draws, each drawn once") {
    FrameCapture f;
    f.post_boundary = 10;
    RasterOptions o;
    auto part = [&](BackBufferPart p, size_t d) {
        o.part = p;
        return DrawnInPart(o, f, d);
    };
    // the whole frame draws all
    CHECK(part(BackBufferPart::kAll, 9));
    CHECK(part(BackBufferPart::kAll, 12));
    // from post_boundary on, the HUD's; before it, the eyes'
    for (size_t d : {0, 9, 10, 500}) {
        CAPTURE(d);
        CHECK(part(BackBufferPart::kEyes, d) != part(BackBufferPart::kHud, d));
        CHECK(part(BackBufferPart::kHud, d) == (d >= 10));
    }
    // hud_from moves the split: 0 puts all of it on the panel
    o.hud_from = 0;
    CHECK(part(BackBufferPart::kHud, 0));
    CHECK_FALSE(part(BackBufferPart::kEyes, 9));
    o.part = BackBufferPart::kHud;
    CHECK_FALSE(OverlayOnly(o, f));  // the whole frame, drawn as usual
    o.hud_from = FrameCapture::kNoPost;
    CHECK(OverlayOnly(o, f));
    // a frame without post-processing has no overlay to split off
    f.post_boundary = FrameCapture::kNoPost;
    CHECK(part(BackBufferPart::kEyes, 500));
    CHECK_FALSE(part(BackBufferPart::kHud, 500));
}

TEST_CASE("the HUD panel starts at post_boundary, or at 0 where the eyes can't see the frame") {
    Mat4 vp_a, vp_hud;
    FrameCapture f = Frame(vp_a, vp_hud);
    CHECK(vr::HudBoundary(f) == 4);
    // no DoPostProcess (some menus): all of it on the panel
    f.post_boundary = FrameCapture::kNoPost;
    CHECK(vr::HudBoundary(f) == 0);
    // no perspective world camera for RePose (a flat menu)
    f.post_boundary = 4;
    for (size_t d = 0; d < 4; d++) f.draws[d].view_proj = Mat4{};
    CHECK(vr::HudBoundary(f) == 0);
}

namespace {

// the quaternion's turn of v, as Rotate's
void Turned(const vr::XrPose& p, float x, float y, float z, float out[3]) {
    const float v[3] = {x, y, z};
    vr::Rotate(p.q, v, out);
}

}  // namespace

TEST_CASE("the HUD panel hangs ahead and below, tilted to face the eyes, its width by angle") {
    vr::HudLayout l;
    l.distance = 1.8f;
    l.below = 0.3f;
    l.width_deg = 55;
    vr::XrPose head;
    head.p[1] = 1.6f;  // eyes 1.6 m up, facing -z
    const vr::HudQuad h = vr::HudPlacement(l, head);
    CheckVec(h.pose.p, 0, 1.3f, -1.8f);
    // 55 degrees across at 1.8 m: 2 * 1.8 * tan(27.5), 16:9
    CHECK(h.size[0] == doctest::Approx(1.874f).epsilon(1e-3));
    CHECK(h.size[0] == doctest::Approx(2 * 1.8f * std::tan(27.5f * kPi / 180)));
    CHECK(h.size[1] == doctest::Approx(h.size[0] * 9 / 16));
    // its normal (+z) points back at the eyes, tilted up by atan(below /
    // distance); its right stays level
    float n[3], right[3];
    Turned(h.pose, 0, 0, 1, n);
    const float tilt = std::atan2(0.3f, 1.8f);
    CheckVec(n, 0, std::sin(tilt), std::cos(tilt));
    const float to_eye[3] = {head.p[0] - h.pose.p[0], head.p[1] - h.pose.p[1],
                             head.p[2] - h.pose.p[2]};
    CHECK(freecam::Dot3(n, to_eye) == doctest::Approx(freecam::Len3(to_eye)));
    Turned(h.pose, 1, 0, 0, right);
    CheckVec(right, 1, 0, 0);
    // a width in metres instead, and another aspect
    l.width_m = 2;
    l.aspect = 4.0f / 3;
    const vr::HudQuad m = vr::HudPlacement(l, head);
    CHECK(m.size[0] == doctest::Approx(2));
    CHECK(m.size[1] == doctest::Approx(1.5f));
}

TEST_CASE("the HUD panel goes where the head faced at the recenter, its yaw alone") {
    vr::HudLayout l;
    l.distance = 2;
    l.below = 0;
    // turned 90 degrees left (about +y) and pitched down 30: the panel
    // stands level ahead on the left, at the eyes' height
    vr::XrPose head;
    head.p[0] = 0.5f;
    head.p[1] = 1.7f;
    head.p[2] = -0.2f;
    const float yaw[4] = {0, std::sin(kPi / 4), 0, std::cos(kPi / 4)};
    const float pitch[4] = {std::sin(-kPi / 12), 0, 0, std::cos(-kPi / 12)};
    // q = yaw * pitch
    head.q[0] = yaw[3] * pitch[0];
    head.q[1] = yaw[1] * pitch[3];
    head.q[2] = -yaw[1] * pitch[0];
    head.q[3] = yaw[3] * pitch[3];
    const vr::HudQuad h = vr::HudPlacement(l, head);
    CheckVec(h.pose.p, 0.5f - 2, 1.7f, -0.2f);
    float n[3], up[3];
    Turned(h.pose, 0, 0, 1, n);
    CheckVec(n, 1, 0, 0);  // back toward the head, along +x
    Turned(h.pose, 0, 1, 0, up);
    CheckVec(up, 0, 1, 0);
}

TEST_CASE("the recenter's head is between the eyes, facing their way") {
    // both turned 40 degrees left about +y, 64 mm apart along that turn's
    // right; the right eye's quaternion the same turn negated
    const float a = 40 * kPi / 180;
    vr::XrPose left, right;
    left.q[1] = std::sin(a / 2);
    left.q[3] = std::cos(a / 2);
    for (int i = 0; i < 4; i++) right.q[i] = -left.q[i];
    const float r[3] = {std::cos(a), 0, -std::sin(a)};
    for (int i = 0; i < 3; i++) {
        left.p[i] = (i == 1 ? 1.6f : 0) - 0.032f * r[i];
        right.p[i] = (i == 1 ? 1.6f : 0) + 0.032f * r[i];
    }
    const vr::XrPose h = vr::HeadPose(left, right);
    CheckVec(h.p, 0, 1.6f, 0);
    float fwd[3], want[3];
    Turned(h, 0, 0, -1, fwd);
    Turned(left, 0, 0, -1, want);
    CheckVec(fwd, want[0], want[1], want[2]);
    // the panel then hangs where that head looks
    vr::HudLayout l;
    l.distance = 2;
    l.below = 0;
    const vr::HudQuad q = vr::HudPlacement(l, h);
    CheckVec(q.pose.p, 2 * want[0], 1.6f, 2 * want[2]);
}

TEST_CASE("a setting as a button fires on each change, not on the first value seen") {
    vr::ChangeTrigger t;
    CHECK_FALSE(t.Fired(true));  // saved as true: no recenter at launch
    CHECK_FALSE(t.Fired(true));
    CHECK(t.Fired(false));
    CHECK_FALSE(t.Fired(false));
    CHECK(t.Fired(true));
}

namespace {

// a game camera at `eye` looking at `at`, then rolled `roll` radians about its
// forward: right and up turned together
freecam::Pose Rolled(const float eye[3], const float at[3], float roll) {
    freecam::Pose p = freecam::LookAt(eye, at);
    const float c = std::cos(roll), s = std::sin(roll);
    for (int k = 0; k < 3; k++) {
        const float r = p.right[k], u = p.up[k];
        p.right[k] = c * r + s * u;
        p.up[k] = -s * r + c * u;
    }
    return p;
}

// the head 1.6 m up facing -z
vr::XrPose StandingHead() {
    vr::XrPose h;
    h.p[1] = 1.6f;
    return h;
}

// a camera 34.8 degrees across at 16:9
vr::GameFov Fov348() {
    vr::GameFov f;
    f.hfov = 34.8f * kPi / 180;
    f.vfov = 2 * std::atan(std::tan(f.hfov / 2) * 9 / 16);
    return f;
}

}  // namespace

TEST_CASE("the game's field of view from its projection") {
    const vr::GameFov f = vr::GameFovOf(GameProj());
    CHECK(f.hfov == doctest::Approx(kPi / 2));  // x scale 1: 90 degrees
    CHECK(f.vfov == doctest::Approx(2 * std::atan(9.0f / 16)));
}

TEST_CASE("the window is vr_window_width across at its distance, 16:9, level and straight ahead") {
    const vr::WindowFrame w = vr::WindowPlacement(StandingHead(), 1.8f, 100 * kPi / 180);
    // 2 * 1.8 * tan(50 degrees)
    CHECK(w.size[0] == doctest::Approx(4.290f).epsilon(1e-3));
    CHECK(w.size[1] == doctest::Approx(w.size[0] * 9 / 16));
    CheckVec(w.pose.p, 0, 1.6f, -1.8f);
    float n[3], up[3];
    Turned(w.pose, 0, 0, 1, n);
    CheckVec(n, 0, 0, 1);  // facing the head, no tilt
    Turned(w.pose, 0, 1, 0, up);
    CheckVec(up, 0, 1, 0);
    // a head pitched down and rolled at the recenter: still level, at its height
    vr::XrPose tipped = StandingHead();
    const float pitch[4] = {std::sin(-0.2f), 0, 0, std::cos(-0.2f)};
    const float roll[4] = {0, 0, std::sin(0.15f), std::cos(0.15f)};
    vr::QuatMul(pitch, roll, tipped.q);
    const vr::WindowFrame t = vr::WindowPlacement(tipped, 1.8f, 100 * kPi / 180);
    CheckVec(t.pose.p, 0, 1.6f, -1.8f);
    Turned(t.pose, 0, 1, 0, up);
    CheckVec(up, 0, 1, 0);
}

namespace {

// a captured projection `hfov` radians across at 16:9, depth as GameProj's
Mat4 ProjFor(float hfov) {
    Mat4 p = GameProj();
    p.m[0][0] = 1 / std::tan(hfov / 2);
    p.m[2][1] = p.m[0][0] * 16 / 9;
    return p;
}

// `x` (the venue) through vp, to normalized x, y and w
void VenueNdc(const Mat4& vp, const float x[3], float& nx, float& ny, float* w = nullptr) {
    float c[4];
    freecam::Transform(x, vp, c);
    nx = c[0] / c[3];
    ny = c[1] / c[3];
    if (w) *w = c[3];
}

// a LOCAL point in `eye`'s picture, normalized (-1..1 across its field of view)
bool LocalNdc(const vr::XrPose& eye, const vr::Fov& fov, const float p[3], float& nx, float& ny) {
    float x, y;
    if (!vr::EyePixel(eye, fov, 2, 2, p, x, y)) return false;
    nx = x - 1;
    ny = 1 - y;
    return true;
}

// an eye's view_proj, as RePose gives it
Mat4 EyeVp(const vr::Eye& e, const Mat4& proj) {
    return freecam::Mul(vr::EyeView(e), vr::EyeProjection(proj, e.fov));
}

// a venue point at game-picture position (nx, ny), `z` units ahead of `game`
void InPicture(const freecam::Pose& game, const vr::GameFov& f, float nx, float ny, float z,
               float out[3]) {
    const float tx = std::tan(f.hfov / 2), ty = std::tan(f.vfov / 2);
    for (int c = 0; c < 3; c++)
        out[c] = game.eye[c] + z * (nx * tx * game.right[c] + ny * ty * game.up[c] + game.fwd[c]);
}

}  // namespace

TEST_CASE("from the head at the recenter the window is the game's picture, at any field of view") {
    const float eye[3] = {0, -1000, 400}, at[3] = {50, -500, 200};  // pitched down
    const vr::Fov fov{-1.0f, 0.9f, 0.95f, -1.05f};  // asymmetric, as headsets are
    for (float hfov_deg : {34.8f, 79.0f, 110.0f}) {
        for (float roll : {0.0f, 0.3f}) {
            CAPTURE(hfov_deg);
            CAPTURE(roll);
            const freecam::Pose game = Rolled(eye, at, roll);
            const Mat4 proj = ProjFor(hfov_deg * kPi / 180);
            const vr::GameFov gf = vr::GameFovOf(proj);
            // the head turned 30 degrees left at the recenter; the eye there,
            // looking a little off the window's middle
            vr::XrPose head = StandingHead();
            head.q[1] = std::sin(kPi / 12);
            head.q[3] = std::cos(kPi / 12);
            const vr::WindowFrame w = vr::WindowPlacement(head, 1.8f, 100 * kPi / 180);
            vr::XrPose look = head;
            const float off[4] = {std::sin(0.05f), std::sin(0.08f), 0, 1};
            float q[4];
            vr::QuatMul(head.q, off, q);
            const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            for (int k = 0; k < 4; k++) look.q[k] = q[k] / len;
            const vr::Eye e = vr::WindowEye(look, fov, w, game, gf, vr::kUnitsPerMetre);
            CheckVec(e.pose.eye, eye[0], eye[1], eye[2], 1e-3f);
            const Mat4 vp = EyeVp(e, proj);
            // every point of the game's picture, near or far, is where the
            // window's point at that place of it is: the window is the
            // picture, clip -1..1 its edges
            for (float nx : {-1.0f, -0.4f, 0.0f, 0.7f, 1.0f}) {
                for (float ny : {-1.0f, 0.3f, 1.0f}) {
                    for (float z : {30.0f, 400.0f, 5000.0f}) {
                        float x[3];
                        InPicture(game, gf, nx, ny, z, x);
                        const float on_window[3] = {nx * w.size[0] / 2, ny * w.size[1] / 2, 0};
                        float local[3];
                        vr::WindowToLocal(w, on_window, local);
                        float ex, ey, wx, wy;
                        VenueNdc(vp, x, ex, ey);
                        REQUIRE(LocalNdc(look, fov, local, wx, wy));
                        CHECK(ex == doctest::Approx(wx).epsilon(1e-3));
                        CHECK(ey == doctest::Approx(wy).epsilon(1e-3));
                    }
                }
            }
            // facing the window square on, a point's depth is its distance
            // ahead of the camera, as the game's projection has it
            const vr::Eye square = vr::WindowEye(head, fov, w, game, gf, vr::kUnitsPerMetre);
            float x[3], nx, ny, dw;
            InPicture(game, gf, 0.3f, -0.2f, 750, x);
            VenueNdc(EyeVp(square, proj), x, nx, ny, &dw);
            CHECK(dw == doctest::Approx(750).epsilon(1e-3));
        }
    }
}

TEST_CASE("the window stays one size whatever the game's field of view; the picture's magnified") {
    Mat4 vp_a, vp_hud;
    FrameCapture f = Frame(vp_a, vp_hud);
    vr::FrameInputs in;
    in.head = StandingHead();
    for (int e = 0; e < 2; e++) {
        in.views[e] = in.head;
        in.fov[e] = {-0.9f, 0.9f, 0.9f, -0.9f};
    }
    in.hud_boundary = vr::HudBoundary(f);
    in.camera = freecam::Decompose(vp_a, in.game, in.proj);
    REQUIRE(in.camera);
    in.window_width_deg = 100;
    vr::Viewpoint vp;
    float size[2] = {0, 0};
    for (float hfov_deg : {35.0f, 63.0f, 79.0f}) {
        CAPTURE(hfov_deg);
        in.proj = ProjFor(hfov_deg * kPi / 180);
        const vr::FrameLayout l = vr::ResolveFrame(vp, in);
        REQUIRE(l.window);
        if (size[0] == 0) {
            size[0] = l.window_frame.size[0];
            size[1] = l.window_frame.size[1];
        }
        CHECK(l.window_frame.size[0] == doctest::Approx(size[0]));
        CHECK(l.window_frame.size[1] == doctest::Approx(size[1]));
        // k = tan(50) / tan(hfov / 2): 3.78 at 35 degrees, 1.45 at 79
        CHECK(l.magnify ==
              doctest::Approx(std::tan(50 * kPi / 180) / std::tan(hfov_deg * kPi / 360)));
        // the menu panel lies on it, the same rectangle
        CHECK(l.hud_in_window);
        CHECK(l.hud_quad.size[0] == doctest::Approx(size[0]));
        CHECK(l.hud_quad.size[1] == doctest::Approx(size[1]));
    }
    CHECK(size[0] == doctest::Approx(2 * 1.8f * std::tan(50 * kPi / 180)));
    // wider asked for, wider made
    in.window_width_deg = 120;
    CHECK(vr::ResolveFrame(vp, in).window_frame.size[0] ==
          doctest::Approx(2 * 1.8f * std::tan(60 * kPi / 180)));
}

TEST_CASE("leaning and stepping through the window: the squeezed venue seen as through a window") {
    const float cam[3] = {120, -900, 300}, at[3] = {-40, -500, 260};
    const freecam::Pose game = Rolled(cam, at, 0.2f);
    const Mat4 proj = ProjFor(40 * kPi / 180);
    const vr::GameFov gf = vr::GameFovOf(proj);
    const vr::XrPose head = StandingHead();
    const vr::WindowFrame w = vr::WindowPlacement(head, 1.8f, 100 * kPi / 180);
    const float u = vr::kUnitsPerMetre * 2;  // vr_world_scale 2
    const vr::WindowScale s = vr::WindowScaleOf(w, gf, u);
    const float k = vr::WindowMagnify(s);
    CHECK(k == doctest::Approx(std::tan(50 * kPi / 180) / std::tan(20 * kPi / 180)));
    CHECK(s.sx == doctest::Approx(u));  // life size across
    CHECK(s.sy == doctest::Approx(u));  // and up, the picture being 16:9 too
    CHECK(s.sz == doctest::Approx(k * u));
    const vr::Fov fov{-0.9f, 0.9f, 0.9f, -0.9f};
    // leaning 0.1 m right moves the eye u / 10 along the camera's right
    vr::XrPose lean = head;
    lean.p[0] += 0.1f;
    const vr::Eye l = vr::WindowEye(lean, fov, w, game, gf, u);
    CheckVec(l.pose.eye, cam[0] + 0.1f * u * game.right[0], cam[1] + 0.1f * u * game.right[1],
             cam[2] + 0.1f * u * game.right[2], 1e-3f);
    // stepping 0.5 m toward the window goes k times further forward
    vr::XrPose step = head;
    step.p[2] -= 0.5f;
    const vr::Eye st = vr::WindowEye(step, fov, w, game, gf, u);
    const float fwd = 0.5f * k * u;
    CheckVec(st.pose.eye, cam[0] + fwd * game.fwd[0], cam[1] + fwd * game.fwd[1],
             cam[2] + fwd * game.fwd[2], 1e-3f);
    // from either, every venue point is where its squeezed place in LOCAL
    // space is: exact for the squeezed venue (points in front of the eye:
    // stepped 0.5 m in, those under 0.5 k u = 129 units ahead of the camera
    // are behind it)
    for (const vr::XrPose* from : {&lean, &step}) {
        const vr::Eye e = vr::WindowEye(*from, fov, w, game, gf, u);
        const Mat4 vp = EyeVp(e, proj);
        for (float z : {300.0f, 900.0f, 4000.0f}) {
            float x[3], win[3], local[3], ex, ey, wx, wy;
            InPicture(game, gf, 0.5f, -0.3f, z, x);
            vr::VenueToWindow(w, game, s, x, win);
            vr::WindowToLocal(w, win, local);
            VenueNdc(vp, x, ex, ey);
            REQUIRE(LocalNdc(*from, fov, local, wx, wy));
            CHECK(ex == doctest::Approx(wx).epsilon(1e-3));
            CHECK(ey == doctest::Approx(wy).epsilon(1e-3));
        }
    }
    // the plane k u D ahead of the camera is the window's: its points stay
    // on the window's (same place in the picture) however the head moves,
    // and what's beyond moves with the head, as through a window
    float plane[3], beyond[3];
    InPicture(game, gf, 0, 0, k * u * 1.8f, plane);
    InPicture(game, gf, 0, 0, 6 * k * u * 1.8f, beyond);
    const Mat4 vp_lean = EyeVp(l, proj);
    float on_window[3];
    const float centre[3] = {0, 0, 0};
    vr::WindowToLocal(w, centre, on_window);
    float px, py, bx, by, wx, wy;
    VenueNdc(vp_lean, plane, px, py);
    REQUIRE(LocalNdc(lean, fov, on_window, wx, wy));
    CHECK(px == doctest::Approx(wx).epsilon(1e-3));
    VenueNdc(vp_lean, beyond, bx, by);
    CHECK(bx > px);  // leant right: the far point is right of the window's middle
}

TEST_CASE("the eyes are the user's IPD apart in the venue, whatever the magnification") {
    const float cam[3] = {0, -1000, 400}, at[3] = {50, -500, 200};
    const freecam::Pose game = freecam::LookAt(cam, at);
    const vr::XrPose head = StandingHead();
    const vr::WindowFrame w = vr::WindowPlacement(head, 1.8f, 100 * kPi / 180);
    const vr::Fov fov{-0.9f, 0.9f, 0.9f, -0.9f};
    for (float hfov_deg : {30.0f, 80.0f}) {
        const vr::GameFov gf = vr::GameFovOf(ProjFor(hfov_deg * kPi / 180));
        vr::XrPose l = head, r = head;
        l.p[0] -= 0.032f;
        r.p[0] += 0.032f;
        const vr::Eye el = vr::WindowEye(l, fov, w, game, gf, vr::kUnitsPerMetre);
        const vr::Eye er = vr::WindowEye(r, fov, w, game, gf, vr::kUnitsPerMetre);
        const float d[3] = {er.pose.eye[0] - el.pose.eye[0], er.pose.eye[1] - el.pose.eye[1],
                            er.pose.eye[2] - el.pose.eye[2]};
        CHECK(freecam::Len3(d) == doctest::Approx(0.064f * 39.37f));
        CHECK(freecam::Dot3(d, game.right) == doctest::Approx(0.064f * 39.37f));
    }
}

TEST_CASE("a pitched and rolled game camera: the user's up is the camera's, the window level") {
    const float eye[3] = {0, -1000, 400}, at[3] = {50, -500, 200};  // pitched down
    const freecam::Pose game = Rolled(eye, at, 0.3f);
    const vr::GameFov gf = Fov348();
    // the head turned 30 degrees left at the recenter: the window is too
    vr::XrPose head = StandingHead();
    head.q[1] = std::sin(kPi / 12);
    head.q[3] = std::cos(kPi / 12);
    const vr::WindowFrame w = vr::WindowPlacement(head, 1.8f, 100 * kPi / 180);
    float up[3];
    Turned(w.pose, 0, 1, 0, up);
    CheckVec(up, 0, 1, 0);
    const vr::Fov fov{-0.9f, 0.9f, 0.9f, -0.9f};
    // at the head the camera exactly, rolled and pitched as it is
    const freecam::Pose p = vr::WindowEye(head, fov, w, game, gf, vr::kUnitsPerMetre).pose;
    CheckVec(p.eye, eye[0], eye[1], eye[2], 1e-3f);
    CheckVec(p.up, game.up[0], game.up[1], game.up[2]);
    CheckVec(p.right, game.right[0], game.right[1], game.right[2]);
    // rising 0.1 m in LOCAL space (its y, the room's up) rises along the
    // camera's up, not the venue's
    vr::XrPose rise = head;
    rise.p[1] += 0.1f;
    const freecam::Pose r = vr::WindowEye(rise, fov, w, game, gf, vr::kUnitsPerMetre).pose;
    CheckVec(r.eye, eye[0] + 3.937f * game.up[0], eye[1] + 3.937f * game.up[1],
             eye[2] + 3.937f * game.up[2], 1e-3f);
    // looking 20 degrees left of the window turns from the camera's forward
    // about the camera's up
    vr::XrPose look = head;
    const float left[4] = {0, std::sin(kPi / 18), 0, std::cos(kPi / 18)};
    vr::QuatMul(left, head.q, look.q);
    const freecam::Pose k = vr::WindowEye(look, fov, w, game, gf, vr::kUnitsPerMetre).pose;
    CHECK(freecam::Dot3(k.fwd, game.fwd) == doctest::Approx(std::cos(kPi / 9)));
    CHECK(freecam::Dot3(k.fwd, game.right) == doctest::Approx(-std::sin(kPi / 9)));
    CHECK(freecam::Dot3(k.up, game.up) == doctest::Approx(1));
}

TEST_CASE("RePose takes the window's view where an eye has one") {
    Mat4 vp_a, vp_hud;
    const FrameCapture in = Frame(vp_a, vp_hud);
    freecam::Pose game{};
    Mat4 proj{};
    REQUIRE(freecam::Decompose(vp_a, game, proj));
    const vr::XrPose head = StandingHead();
    const vr::WindowFrame w = vr::WindowPlacement(head, 1.8f, 100 * kPi / 180);
    vr::XrPose left = head;
    left.p[0] -= 0.032f;
    const vr::Eye e = vr::WindowEye(left, {-0.9f, 0.9f, 0.9f, -0.9f}, w, game,
                                    vr::GameFovOf(proj), vr::kUnitsPerMetre);
    REQUIRE(e.has_view);
    FrameCapture out;
    REQUIRE(vr::RePose(in, e, out).ok);
    const Mat4 want = EyeVp(e, proj);
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            CHECK(std::fabs(out.draws[0].view_proj.m[r][c] - want.m[r][c]) <
                  1e-3f * (1 + std::fabs(want.m[r][c])));
    CheckVec(out.shades[size_t(out.draws[0].shade)].eye, e.pose.eye[0], e.pose.eye[1],
             e.pose.eye[2], 1e-3f);
}

namespace {

// whether a ray from `from` along `dir` (LOCAL) meets the quad, inside its
// edges by a hair (an edge two quads share counts for neither)
bool Hits(const vr::HudQuad& q, const float from[3], const float dir[3]) {
    float n[3], ax[3], ay[3];
    Turned(q.pose, 0, 0, 1, n);
    Turned(q.pose, 1, 0, 0, ax);
    Turned(q.pose, 0, 1, 0, ay);
    const float to[3] = {q.pose.p[0] - from[0], q.pose.p[1] - from[1], q.pose.p[2] - from[2]};
    const float dn = freecam::Dot3(dir, n);
    if (std::fabs(dn) < 1e-9f) return false;
    const float t = freecam::Dot3(to, n) / dn;
    if (t <= 0) return false;
    float off[3];
    for (int k = 0; k < 3; k++) off[k] = from[k] + t * dir[k] - q.pose.p[k];
    return std::fabs(freecam::Dot3(off, ax)) < q.size[0] / 2 - 1e-4f &&
           std::fabs(freecam::Dot3(off, ay)) < q.size[1] / 2 - 1e-4f;
}

}  // namespace

TEST_CASE("the dark around the window: every way but through it covered once, facing the head") {
    vr::XrPose head = StandingHead();
    head.q[1] = std::sin(0.3f);  // turned
    head.q[3] = std::cos(0.3f);
    const vr::WindowFrame w = vr::WindowPlacement(head, 1.8f, 34.8f * kPi / 180);
    vr::HudQuad q[vr::kFrameQuads];
    REQUIRE(vr::FrameQuads(w, q) == vr::kFrameQuads);
    // each faces the head: its normal toward the box's inside
    for (const vr::HudQuad& f : q) {
        float n[3];
        Turned(f.pose, 0, 0, 1, n);
        const float to_head[3] = {head.p[0] - f.pose.p[0], head.p[1] - f.pose.p[1],
                                  head.p[2] - f.pose.p[2]};
        CHECK(freecam::Dot3(n, to_head) > 0);
    }
    // the face's four strips tile it with the window left out: their area is
    // the face's less the window's
    const float s = vr::kFrameReach * 1.8f;
    float area = 0;
    for (int i = 0; i < 4; i++) area += q[i].size[0] * q[i].size[1];
    CHECK(area == doctest::Approx(4 * s * s - w.size[0] * w.size[1]));
    // rays from the head, and from it leant 0.3 m, over a sphere of
    // directions: one quad each, none where the ray goes through the window
    float inv[4];
    vr::Conjugate(w.pose.q, inv);
    int through = 0, covered = 0, grazing = 0;
    for (float lean : {0.0f, 0.3f, -0.3f}) {
        vr::XrPose at = head;
        float side[3];
        Turned(w.pose, lean, lean / 2, 0, side);
        for (int k = 0; k < 3; k++) at.p[k] += side[k];
        for (int a = 0; a < 48; a++) {
            for (int b = 1; b < 24; b++) {
                const float az = (float(a) + 0.37f) * 2 * kPi / 48;
                const float el = float(b) * kPi / 24 - kPi / 2;
                const float dir[3] = {std::cos(el) * std::sin(az), std::sin(el),
                                      std::cos(el) * std::cos(az)};
                int hits = 0;
                for (const vr::HudQuad& f : q) hits += Hits(f, at.p, dir);
                // through the window's rectangle?
                const float rel[3] = {at.p[0] - w.pose.p[0], at.p[1] - w.pose.p[1],
                                      at.p[2] - w.pose.p[2]};
                float o[3], d[3];
                vr::Rotate(inv, rel, o);
                vr::Rotate(inv, dir, d);
                bool in_window = false;
                if (d[2] < 0) {
                    const float t = -o[2] / d[2];
                    in_window = std::fabs(o[0] + t * d[0]) < w.size[0] / 2 &&
                                std::fabs(o[1] + t * d[1]) < w.size[1] / 2;
                }
                // a ray along an edge two quads share: neither, by Hits' hair
                if (hits == 0 && !in_window) {
                    grazing++;
                    continue;
                }
                CAPTURE(lean);
                CAPTURE(az);
                CAPTURE(el);
                CHECK(hits == (in_window ? 0 : 1));
                (in_window ? through : covered)++;
            }
        }
    }
    CHECK(through > 0);
    CHECK(covered > 3000);
    CHECK(grazing < 10);
}

TEST_CASE("the frame's choice: window, anchor or black, and the panel's place") {
    Mat4 vp_a, vp_hud;
    const FrameCapture f = Frame(vp_a, vp_hud);
    vr::FrameInputs in;
    in.views[0].p[0] = -0.032f;
    in.views[1].p[0] = 0.032f;
    for (int e = 0; e < 2; e++) {
        in.views[e].p[1] = 1.6f;
        in.fov[e] = {-0.9f, 0.8f, 0.85f, -0.95f};
    }
    in.head = StandingHead();
    in.hud_boundary = vr::HudBoundary(f);
    REQUIRE(in.hud_boundary == 4);
    in.camera = freecam::Decompose(vp_a, in.game, in.proj);
    REQUIRE(in.camera);
    in.dim = 1;
    vr::Viewpoint vp;  // game_camera
    // a menu with a world: the window, the panel in it, the dark around it
    vr::FrameLayout l = vr::ResolveFrame(vp, in);
    CHECK(l.window);
    CHECK_FALSE(l.eyes_black);
    CHECK(l.hud_in_window);
    CheckVec(l.hud_quad.pose.p, l.window_frame.pose.p[0], l.window_frame.pose.p[1],
             l.window_frame.pose.p[2]);
    CHECK(l.hud_quad.size[0] == doctest::Approx(l.window_frame.size[0]));
    CHECK(l.hud_quad.size[1] == doctest::Approx(l.window_frame.size[1]));
    CHECK(l.frame_quads == vr::kFrameQuads);
    // the eyes 32 mm either side of the camera, with the headset's fields of view
    CheckVec(l.eyes[0].pose.eye, -0.032f * 39.37f, -1000, 200, 1e-3f);
    CHECK(l.eyes[1].fov.left == doctest::Approx(-0.9f));
    // a song: still the window, the panel on its own place unless asked
    in.in_song = true;
    l = vr::ResolveFrame(vp, in);
    CHECK(l.window);
    CHECK_FALSE(l.hud_in_window);
    const vr::HudQuad own = vr::HudPlacement(in.hud_layout, in.head);
    CheckVec(l.hud_quad.pose.p, own.pose.p[0], own.pose.p[1], own.pose.p[2]);
    in.hud_in_window = true;
    CHECK(vr::ResolveFrame(vp, in).hud_in_window);
    // vr_window_dim 0: no dark
    in.dim = 0;
    CHECK(vr::ResolveFrame(vp, in).frame_quads == 0);
    // another viewpoint: songs from the anchor, menus through the window
    vr::Viewpoint fixed;
    fixed.mode = vr::ViewMode::kFrontRow;
    fixed.window = false;
    REQUIRE(vr::ParseAnchor("0,-1250,520,0", fixed.anchor));
    l = vr::ResolveFrame(fixed, in);
    CHECK_FALSE(l.window);
    CHECK_FALSE(l.eyes_black);
    CHECK(l.frame_quads == 0);
    CheckVec(l.eyes[0].pose.eye, -0.032f * 39.37f, -1250, 520 + 1.6f * 39.37f, 1e-3f);
    in.in_song = false;
    CHECK(vr::ResolveFrame(fixed, in).window);
    // HudBoundary 0 (no world the eyes can see): black eyes, the panel alone
    // on its own place
    in.hud_boundary = 0;
    l = vr::ResolveFrame(vp, in);
    CHECK(l.eyes_black);
    CHECK_FALSE(l.window);
    CHECK_FALSE(l.hud_in_window);
    CHECK(l.hud);
    CHECK(l.frame_quads == 0);
    CheckVec(l.hud_quad.pose.p, own.pose.p[0], own.pose.p[1], own.pose.p[2]);
    in.hud = false;
    CHECK_FALSE(vr::ResolveFrame(vp, in).hud);
}

namespace {

// a track camera's projection: `hfov` degrees across, 21.05 up and down (RB3's
// track cameras'), depth from near to far as theirs (z = a w + b, 0..1),
// shifted `shift` across (clip x += shift w)
Mat4 TrackProj(float hfov, float n, float f, float shift = 0) {
    Mat4 p{};
    p.m[0][0] = 1 / std::tan(hfov * kPi / 360);
    p.m[1][0] = shift;
    p.m[2][1] = 1 / std::tan(21.05f * kPi / 360);
    p.m[1][2] = f / (f - n);
    p.m[3][2] = -n * f / (f - n);
    p.m[1][3] = 1;
    return p;
}

// a camera at `eye` turned `yaw` degrees (counterclockwise from above) and
// pitched `pitch` down, as RB3's track cameras are
Mat4 TrackVp(const float eye[3], float yaw, float pitch, const Mat4& proj) {
    const float y = yaw * kPi / 180, p = pitch * kPi / 180;
    const float at[3] = {eye[0] - std::sin(y) * std::cos(p) * 100,
                         eye[1] + std::cos(y) * std::cos(p) * 100, eye[2] - std::sin(p) * 100};
    return freecam::Mul(freecam::View(freecam::LookAt(eye, at)), proj);
}

// a flat rectangle in its draw's world, x0..x1 by y0..y1 at height 0
std::shared_ptr<Geometry> Rect(float x0, float x1, float y0, float y1) {
    auto g = std::make_shared<Geometry>();
    const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
    for (int i = 0; i < 4; i++) {
        Vertex v{};
        v.pos[0] = xs[i];
        v.pos[1] = ys[i];
        g->verts.push_back(v);
    }
    g->indices = {0, 1, 2, 0, 2, 3};
    return g;
}

struct SongCam {
    uint32_t cam;
    Mat4 vp;
    float viewport[4];
    float zrange[2];
    int draws;
    std::shared_ptr<Geometry> geom;
};

// A song's frame as RB3 draws it (crowd-10s, screen-mp2-25s, screen-mp4-10s
// in out/): the world camera's draws, post_boundary, then each camera of
// `after`'s draws in turn: the tracks', then HudFadePause's
FrameCapture SongFrame(const std::vector<SongCam>& after) {
    FrameCapture f;
    const float eye[3] = {0, -1000, 200}, at[3] = {0, 0, 200};
    const Mat4 world = freecam::Mul(freecam::View(freecam::LookAt(eye, at)), GameProj());
    auto add = [&](uint32_t cam, const Mat4& vp, std::shared_ptr<Geometry> g) {
        DrawItem d{};
        d.view_proj = vp;
        d.cam = cam;
        d.geom = std::move(g);
        for (int i = 0; i < 4; i++) d.world.m[i][i] = 1;
        f.draws.push_back(d);
    };
    CameraView w{};
    w.cam = 1;
    w.viewport[2] = 1280;
    w.viewport[3] = 720;
    w.target_w = 1280;
    w.target_h = 720;
    w.zrange[0] = 0.1f;
    f.cameras.push_back(w);
    for (int i = 0; i < 5; i++) add(1, world, Rect(-50, 50, 0, 10));
    f.post_boundary = uint32_t(f.draws.size());
    f.proc_cmds = 7;
    // the post-processing's own quads, into textures
    DrawItem rect{};
    rect.rect_shader = 3;
    rect.target = 0x77;
    f.draws.push_back(rect);
    for (const SongCam& c : after) {
        CameraView v{};
        v.cam = c.cam;
        for (int k = 0; k < 4; k++) v.viewport[k] = c.viewport[k];
        v.target_w = 1280;
        v.target_h = 720;
        v.zrange[0] = c.zrange[0];
        v.zrange[1] = c.zrange[1];
        f.cameras.push_back(v);
        for (int i = 0; i < c.draws; i++) add(c.cam, c.vp, c.geom);
    }
    return f;
}

// the cameras after the tracks: the flat HUD's, the fade's, the pause menu's
void HudFadePause(std::vector<SongCam>& cams, bool pause) {
    const float hud_eye[3] = {0, 0, 0};
    cams.push_back({0x2520BFD8, TrackVp(hud_eye, 0, 0, TrackProj(60, 1, 1000)), {0, 0, 1280, 720},
                    {0, 0.1f}, 42, Rect(-1, 1, 100, 100.1f)});
    // the fade: one quad 768 ahead, far wider than the picture
    const float fade_eye[3] = {0, -768, 0};
    auto fade = std::make_shared<Geometry>();
    for (int i = 0; i < 4; i++) {
        Vertex v{};
        v.pos[0] = (i == 1 || i == 2) ? 600.0f : -600.0f;
        v.pos[2] = i >= 2 ? 600.0f : -600.0f;
        fade->verts.push_back(v);
    }
    fade->indices = {0, 1, 2, 0, 2, 3};
    cams.push_back({0x24F44910, TrackVp(fade_eye, 0, 0, TrackProj(60, 1, 1000)), {0, 0, 1280, 720},
                    {0, 1}, 1, fade});
    if (pause)
        cams.push_back({0x2531A3D0, TrackVp(fade_eye, 0, 7, TrackProj(29, 4.6f, 4561)),
                        {0, 0, 998, 720}, {0, 1}, 13, Rect(-100, 100, 0, 50)});
}

}  // namespace

TEST_CASE("the track cameras: one per player's track, never the HUD's, the fade's or a menu's") {
    const std::shared_ptr<Geometry> track = Rect(-9.8f, 9.8f, -50, 270);
    // one player (crowd-10s): 36.6 degrees across, 7 down, 30..224
    const float e1[3] = {0, -91.82f, 22.6f};
    std::vector<SongCam> one = {
        {0x25492B68, TrackVp(e1, 0, 7, TrackProj(36.56f, 30, 224)), {0, 0, 1280, 720}, {0, 1}, 48,
         track}};
    HudFadePause(one, false);
    FrameCapture f = SongFrame(one);
    vr::TrackCams t = vr::TrackCamerasOf(f);
    REQUIRE(t.count == 1);
    CHECK(t.cams[0].cam == 0x25492B68);
    CHECK(t.cams[0].draws == 48);
    CHECK(t.cams[0].first_draw == f.post_boundary + 1);
    CheckVec(t.cams[0].pose.eye, 0, -91.82f, 22.6f, 1e-3f);
    CHECK(std::asin(t.cams[0].pose.fwd[2]) * 180 / kPi == doctest::Approx(-7).epsilon(1e-3));
    CHECK(t.cams[0].viewport[2] == 1280);
    CHECK(t.Find(0x25492B68) == &t.cams[0]);
    CHECK(t.Find(0x2520BFD8) == nullptr);
    // the pause menu's camera after the HUD's: not a track's
    std::vector<SongCam> paused = {one[0]};
    HudFadePause(paused, true);
    t = vr::TrackCamerasOf(SongFrame(paused));
    REQUIRE(t.count == 1);
    CHECK(t.cams[0].cam == 0x25492B68);
    // a track camera's single draw that doesn't cover the picture is a track's
    std::vector<SongCam> lone = one;
    lone[0].draws = 1;
    CHECK(vr::TrackCamerasOf(SongFrame(lone)).count == 1);
    // two players (screen-mp2-25s): one place, in two viewports, each
    // projection shifted out by its viewport's offset (0.2826 of its half)
    const float e2[3] = {0, -104.82f, 26.39f};
    std::vector<SongCam> two = {
        {0x25492BF8, TrackVp(e2, 0, 7, TrackProj(28.82f, 40.6f, 258, -0.2826f)),
         {0, 0, 998, 720}, {0, 1}, 52, track},
        {0x2555D288, TrackVp(e2, 0, 7, TrackProj(28.82f, 40.6f, 258, 0.2826f)),
         {281, 0, 998, 720}, {0, 1}, 39, track}};
    HudFadePause(two, true);
    t = vr::TrackCamerasOf(SongFrame(two));
    REQUIRE(t.count == 2);
    CHECK(t.cams[0].cam == 0x25492BF8);
    CHECK(t.cams[1].cam == 0x2555D288);
    CHECK(t.cams[1].viewport[0] == 281);
    CHECK(t.cams[1].viewport[2] == 998);
    // the shift isn't taken for a camera moved aside (freecam::Decompose)
    CheckVec(t.cams[0].pose.eye, 0, -104.82f, 26.39f, 1e-3f);
    CheckVec(t.cams[1].pose.right, 1, 0, 0);
    CHECK(t.cams[0].proj.m[1][0] == doctest::Approx(-0.2826f).epsilon(1e-3));
    // four parts (screen-mp4-10s): three tracks turned 10 degrees apart; the
    // vocals' strip is the flat HUD's
    const float a4[3] = {2.63f, -122.14f, 29.54f}, b4[3] = {-2.71f, -122.35f, 28.71f},
                c4[3] = {0, -123.01f, 29.69f};
    const Mat4 p4 = TrackProj(36.56f, 50, 305);
    std::vector<SongCam> four = {
        {0x25499AB8, TrackVp(a4, -10, 6, p4), {0, 0, 1280, 720}, {0, 1}, 47, track},
        {0x256F8848, TrackVp(b4, 10, 6, p4), {0, 0, 1280, 720}, {0, 1}, 40, track},
        {0x25564148, TrackVp(c4, 0, 6, p4), {0, 0, 1280, 720}, {0, 1}, 41, track}};
    HudFadePause(four, false);
    t = vr::TrackCamerasOf(SongFrame(four));
    REQUIRE(t.count == 3);
    CHECK(t.cams[0].cam == 0x25499AB8);
    CHECK(t.cams[2].cam == 0x25564148);
    // without DoPostProcess, or without the flat HUD's camera: none
    f.post_boundary = FrameCapture::kNoPost;
    CHECK(vr::TrackCamerasOf(f).count == 0);
    std::vector<SongCam> bare = {one[0]};
    CHECK(vr::TrackCamerasOf(SongFrame(bare)).count == 0);
}

TEST_CASE("in songs the eyes draw the track cameras' draws past the HUD's boundary, the panel not") {
    const float e1[3] = {0, -91.82f, 22.6f};
    std::vector<SongCam> one = {{0x25492B68, TrackVp(e1, 0, 7, TrackProj(36.56f, 30, 224)),
                                 {0, 0, 1280, 720}, {0, 1}, 3, Rect(-9.8f, 9.8f, -50, 270)}};
    HudFadePause(one, false);
    const FrameCapture f = SongFrame(one);
    RasterOptions o;
    o.hud_from = vr::HudBoundary(f);
    REQUIRE(o.hud_from == f.post_boundary);
    o.eye_cams[0] = vr::TrackCamerasOf(f).cams[0].cam;
    auto part = [&](BackBufferPart p, size_t d) {
        o.part = p;
        return DrawnInPart(o, f, d);
    };
    for (size_t d = 0; d < f.draws.size(); d++) {
        CAPTURE(d);
        const bool track = f.draws[d].cam == 0x25492B68;
        CHECK(part(BackBufferPart::kEyes, d) != part(BackBufferPart::kHud, d));
        CHECK(part(BackBufferPart::kEyes, d) == (d < f.post_boundary || track));
    }
    // a DrawRect quad is never a camera's
    FrameCapture r = f;
    r.draws[f.post_boundary].cam = 0x25492B68;
    r.draws[f.post_boundary].target = 0;
    o.part = BackBufferPart::kHud;
    CHECK(DrawnInPart(o, r, f.post_boundary));
    // a panel from draw 0 (no world the eyes can see) is the whole frame
    o.hud_from = 0;
    CHECK(part(BackBufferPart::kHud, f.post_boundary + 1));
    CHECK_FALSE(part(BackBufferPart::kEyes, f.post_boundary + 1));
}

TEST_CASE("an off-centre projection splits into the camera where it is, the shift in proj") {
    const float eye[3] = {0, -104.8f, 26.4f};
    const Mat4 vp = TrackVp(eye, 0, 7, TrackProj(28.82f, 40.6f, 258, -0.2826f));
    freecam::Pose pose{};
    Mat4 proj{};
    REQUIRE(freecam::Decompose(vp, pose, proj));
    CheckVec(pose.eye, 0, -104.8f, 26.4f, 1e-3f);
    CheckVec(pose.right, 1, 0, 0);
    CHECK(freecam::Dot3(pose.right, pose.fwd) == doctest::Approx(0).epsilon(1e-5));
    CHECK(proj.m[1][0] == doctest::Approx(-0.2826f).epsilon(1e-3));
    CHECK(proj.m[0][0] == doctest::Approx(1 / std::tan(14.41f * kPi / 180)).epsilon(1e-3));
}

namespace {

// one player's track camera, as crowd-10s has it
vr::TrackCams OneTrack() {
    const float e1[3] = {0, -91.82f, 22.6f};
    std::vector<SongCam> one = {{0x25492B68, TrackVp(e1, 0, 7, TrackProj(36.56f, 30, 224)),
                                 {0, 0, 1280, 720}, {0, 1}, 3, Rect(-9.8f, 9.8f, -50, 270)}};
    HudFadePause(one, false);
    return vr::TrackCamerasOf(SongFrame(one));
}

// the track's point x through an eye's TrackView, against its LOCAL place
// through the eye itself: the same picture (normalized x, y)
void CheckSeen(const vr::TrackPlace& p, const vr::TrackView& v, const vr::XrPose& eye,
               const vr::Fov& fov, const float x[3]) {
    float c[4];
    freecam::Transform(x, v.view_proj, c);
    REQUIRE(c[3] > 0);
    float local[3], px = 0, py = 0;
    vr::TrackToLocal(p, 30, x, local);
    REQUIRE(vr::EyePixel(eye, fov, 2, 2, local, px, py));
    CHECK(c[0] / c[3] == doctest::Approx(px - 1).epsilon(1e-3));
    CHECK(c[1] / c[3] == doctest::Approx(1 - py).epsilon(1e-3));
}

}  // namespace

TEST_CASE("the track in front of the head: its strikeline where asked, level, seen from each eye") {
    const vr::TrackCams t = OneTrack();
    REQUIRE(t.count == 1);
    vr::XrPose head = StandingHead();
    head.q[1] = std::sin(0.2f);  // turned 0.4 radians left
    head.q[3] = std::cos(0.2f);
    vr::TrackLayout l;  // 1.2 ahead, 0.3 down, 30 units a metre
    vr::TrackPlace p[vr::kMaxTrackCams];
    REQUIRE(vr::PlaceTracks(t, head, l, p) == 1);
    // the strikeline (the origin) 1.2 m ahead along the head's yaw, 0.3 below
    const float origin[3] = {0, 0, 0}, ahead[3] = {0, 100, 0}, side[3] = {9.8f, 0, 0};
    float s[3], f[3], r[3];
    vr::TrackToLocal(p[0], l.scale, origin, s);
    CheckVec(s, -std::sin(0.4f) * 1.2f, 1.3f, -std::cos(0.4f) * 1.2f, 1e-3f);
    // level, running away from the head, 0.65 m across
    vr::TrackToLocal(p[0], l.scale, ahead, f);
    CHECK(f[1] == doctest::Approx(1.3f).epsilon(1e-3));
    CHECK(std::atan2(-(f[0] - s[0]), -(f[2] - s[2])) == doctest::Approx(0.4f).epsilon(1e-3));
    vr::TrackToLocal(p[0], l.scale, side, r);
    const float half[3] = {r[0] - s[0], r[1] - s[1], r[2] - s[2]};
    CHECK(freecam::Len3(half) == doctest::Approx(9.8f / 30).epsilon(1e-3));
    // tilted: the far end raised
    l.pitch = 20;
    REQUIRE(vr::PlaceTracks(t, head, l, p) == 1);
    vr::TrackToLocal(p[0], l.scale, ahead, f);
    CHECK(f[1] - 1.3f == doctest::Approx(100.0f / 30 * std::sin(20 * kPi / 180)).epsilon(1e-3));
    l.pitch = 0;
    REQUIRE(vr::PlaceTracks(t, head, l, p) == 1);
    // each eye sees the track's points where they are in LOCAL space, from
    // wherever it is and whichever way it looks
    const vr::Fov fov{-0.9f, 0.8f, 0.85f, -0.95f};
    for (float lean : {0.0f, 0.2f}) {
        for (int e = 0; e < 2; e++) {
            vr::XrPose eye = head;
            eye.p[0] += (e ? 0.032f : -0.032f) + lean;
            eye.p[2] -= lean;
            const float look[4] = {std::sin(-0.15f), 0, 0, std::cos(-0.15f)};  // down
            vr::QuatMul(head.q, look, eye.q);
            const vr::TrackView v = vr::TrackEye(eye, fov, p[0], l.scale);
            CHECK(v.cam == 0x25492B68);
            CAPTURE(lean);
            CAPTURE(e);
            CheckSeen(p[0], v, eye, fov, origin);
            CheckSeen(p[0], v, eye, fov, ahead);
            CheckSeen(p[0], v, eye, fov, side);
        }
    }
    // off: none
    l.on = false;
    CHECK(vr::PlaceTracks(t, head, l, p) == 0);
}

TEST_CASE("two players' tracks side by side and parallel, four parts' turned apart") {
    const std::shared_ptr<Geometry> track = Rect(-9.8f, 9.8f, -50, 270);
    const float e2[3] = {0, -104.82f, 26.39f};
    std::vector<SongCam> two = {
        {11, TrackVp(e2, 0, 7, TrackProj(28.82f, 40.6f, 258, -0.2826f)), {0, 0, 998, 720}, {0, 1},
         5, track},
        {12, TrackVp(e2, 0, 7, TrackProj(28.82f, 40.6f, 258, 0.2826f)), {281, 0, 998, 720},
         {0, 1}, 5, track}};
    HudFadePause(two, false);
    const vr::TrackCams t2 = vr::TrackCamerasOf(SongFrame(two));
    REQUIRE(t2.count == 2);
    const vr::XrPose head = StandingHead();
    vr::TrackLayout l;
    vr::TrackPlace p[vr::kMaxTrackCams];
    REQUIRE(vr::PlaceTracks(t2, head, l, p) == 2);
    const float origin[3] = {0, 0, 0}, ahead[3] = {0, 100, 0};
    float s[2][3], f[2][3];
    for (int i = 0; i < 2; i++) {
        vr::TrackToLocal(p[i], l.scale, origin, s[i]);
        vr::TrackToLocal(p[i], l.scale, ahead, f[i]);
    }
    // where the picture has them: the shift (viewport and projection, 0.565
    // of a viewport's half) through tan(14.41 degrees), at the strikeline's
    // 107.3 units
    const float across = 0.5652f * std::tan(14.41f * kPi / 180) * 107.26f / 30;
    CHECK(s[0][0] == doctest::Approx(-across).epsilon(0.01));
    CHECK(s[1][0] == doctest::Approx(across).epsilon(0.01));
    CHECK(s[0][2] == doctest::Approx(-1.2f).epsilon(1e-3));
    CHECK(s[1][2] == doctest::Approx(-1.2f).epsilon(1e-3));
    // parallel
    CHECK(f[0][0] - s[0][0] == doctest::Approx(0).epsilon(1e-3));
    CHECK(f[1][0] - s[1][0] == doctest::Approx(0).epsilon(1e-3));
    // four parts: the side cameras' tracks off to the sides, turned outward
    // as their cameras are turned in
    const float a4[3] = {2.63f, -122.14f, 29.54f}, b4[3] = {-2.71f, -122.35f, 28.71f},
                c4[3] = {0, -123.01f, 29.69f};
    const Mat4 p4 = TrackProj(36.56f, 50, 305);
    std::vector<SongCam> four = {
        {21, TrackVp(a4, -10, 6, p4), {0, 0, 1280, 720}, {0, 1}, 5, track},
        {22, TrackVp(b4, 10, 6, p4), {0, 0, 1280, 720}, {0, 1}, 5, track},
        {23, TrackVp(c4, 0, 6, p4), {0, 0, 1280, 720}, {0, 1}, 5, track}};
    HudFadePause(four, false);
    const vr::TrackCams t4 = vr::TrackCamerasOf(SongFrame(four));
    REQUIRE(vr::PlaceTracks(t4, head, l, p) == 3);
    float s4[3][3], f4[3][3];
    for (int i = 0; i < 3; i++) {
        vr::TrackToLocal(p[i], l.scale, origin, s4[i]);
        vr::TrackToLocal(p[i], l.scale, ahead, f4[i]);
    }
    // the middle one (the reference, the least turned) straight ahead
    CheckVec(s4[2], 0, 1.3f, -1.2f, 1e-3f);
    CHECK(s4[0][0] < -0.5f);  // the camera turned right sees its track left
    CHECK(s4[1][0] > 0.5f);
    CHECK(s4[0][0] == doctest::Approx(-s4[1][0]).epsilon(0.05));
    CHECK(f4[0][0] - s4[0][0] < 0);  // running out to the left
    CHECK(f4[1][0] - s4[1][0] > 0);
}

TEST_CASE("RePose: the track's draws seen from the eye, folded ones too, cut below the picture") {
    const float e1[3] = {0, -91.82f, 22.6f};
    const Mat4 vp = TrackVp(e1, 0, 7, TrackProj(36.56f, 30, 224));
    std::vector<SongCam> one = {
        {0x25492B68, vp, {0, 0, 998, 720}, {0, 1}, 2, Rect(-9.8f, 9.8f, -50, 270)}};
    HudFadePause(one, false);
    FrameCapture f = SongFrame(one);
    const uint32_t first = f.post_boundary + 1;
    // a gem: its place folded into its matrix (a translation 40 along)
    Mat4 move{};
    for (int i = 0; i < 4; i++) move.m[i][i] = 1;
    move.m[3][1] = 40;
    f.draws[first + 1].view_proj = freecam::Mul(move, vp);
    f.draws[first + 1].geom = Rect(-1, 1, -1, 1);
    const vr::TrackCams t = vr::TrackCamerasOf(f);
    REQUIRE(t.count == 1);
    vr::TrackPlace p[vr::kMaxTrackCams];
    REQUIRE(vr::PlaceTracks(t, StandingHead(), vr::TrackLayout{}, p) == 1);
    vr::Eye eye;
    eye.fov = {-0.9f, 0.9f, 0.9f, -0.9f};
    eye.track_count = 1;
    eye.tracks[0] = vr::TrackEye(StandingHead(), eye.fov, p[0], 30);
    FrameCapture out;
    const vr::RePoseStats st = vr::RePose(f, eye, out);
    CHECK(st.track_draws == 2);
    CHECK(st.track_unmoved == 0);
    CHECK(std::memcmp(&out.draws[first].view_proj, &eye.tracks[0].view_proj, sizeof(Mat4)) == 0);
    // the gem keeps its 40 along: its origin is the track's (0, 40, 0)
    const float gem[3] = {0, 0, 0}, at40[3] = {0, 40, 0};
    float a[4], b[4];
    freecam::Transform(gem, out.draws[first + 1].view_proj, a);
    freecam::Transform(at40, eye.tracks[0].view_proj, b);
    for (int k = 0; k < 4; k++) CHECK(a[k] == doctest::Approx(b[k]).epsilon(1e-3));
    // its camera the whole picture
    const CameraView* cv = CameraOf(out, 0x25492B68);
    REQUIRE(cv);
    CHECK(cv->viewport[0] == 0);
    CHECK(cv->viewport[2] == 1280);
    // the surface cut where the game's picture ends below (clip y = -w):
    // none of it below, some of it new
    const Geometry& g = *out.draws[first].geom;
    CHECK(g.verts.size() > 4);
    float lowest = 1e9f;
    for (size_t i = 0; i < g.indices.size(); i++) {
        float c[4];
        freecam::Transform(g.verts[g.indices[i]].pos, vp, c);
        lowest = std::fmin(lowest, c[1] / c[3]);
    }
    CHECK(lowest == doctest::Approx(-1).epsilon(1e-3));
    // the source untouched
    CHECK(f.draws[first].geom->verts.size() == 4);
    CHECK(CameraOf(f, 0x25492B68)->viewport[2] == 998);
}

TEST_CASE("a mesh cut at a plane keeps what's above it, its corners between the old") {
    const auto g = Rect(0, 10, 0, 10);  // two triangles
    // keep y >= 4: y - 4 >= 0
    const float plane[4] = {0, 1, 0, -4};
    const auto cut = vr::CutAtPlane(g, plane);
    REQUIRE(cut != g);
    float area = 0, lo = 1e9f;
    for (size_t i = 0; i + 2 < cut->indices.size(); i += 3) {
        const float* a = cut->verts[cut->indices[i]].pos;
        const float* b = cut->verts[cut->indices[i + 1]].pos;
        const float* c = cut->verts[cut->indices[i + 2]].pos;
        area += std::fabs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])) / 2;
        lo = std::fmin(lo, std::fmin(a[1], std::fmin(b[1], c[1])));
    }
    CHECK(area == doctest::Approx(60));
    CHECK(lo == doctest::Approx(4));
    // the same cut again is the one made (kept per thread)
    CHECK(vr::CutAtPlane(g, plane) == cut);
    // nothing below: the mesh itself
    const float under[4] = {0, 1, 0, 1};
    CHECK(vr::CutAtPlane(g, under) == g);
}

TEST_CASE("the track's cameras held still until one moves past the threshold") {
    vr::TrackHold hold;
    vr::TrackCams t = OneTrack();
    int moved = 0;
    vr::TrackCams h = hold.Hold(t, moved);
    CHECK(moved == 0);
    CheckVec(h.cams[0].pose.eye, 0, -91.82f, 22.6f, 1e-3f);
    // a shake: held
    vr::TrackCams shaken = t;
    shaken.cams[0].pose.eye[2] += 0.5f;
    h = hold.Hold(shaken, moved);
    CHECK(moved == 0);
    CHECK(h.cams[0].pose.eye[2] == doctest::Approx(22.6f));
    // its matrix is still the frame's own, to match its draws
    CHECK(std::memcmp(&h.cams[0].view_proj, &shaken.cams[0].view_proj, sizeof(Mat4)) == 0);
    // moved further: taken again
    shaken.cams[0].pose.eye[2] += 2;
    h = hold.Hold(shaken, moved);
    CHECK(moved == 1);
    CHECK(h.cams[0].pose.eye[2] == doctest::Approx(25.1f));
    // none (the song's over), then a song again: taken afresh
    h = hold.Hold(vr::TrackCams{}, moved);
    CHECK(h.count == 0);
    h = hold.Hold(t, moved);
    CHECK(moved == 0);
    CHECK(h.cams[0].pose.eye[2] == doctest::Approx(22.6f));
}

TEST_CASE("the frame's tracks: in songs with a world, each eye's view of them") {
    const float e1[3] = {0, -91.82f, 22.6f};
    std::vector<SongCam> one = {{0x25492B68, TrackVp(e1, 0, 7, TrackProj(36.56f, 30, 224)),
                                 {0, 0, 1280, 720}, {0, 1}, 3, Rect(-9.8f, 9.8f, -50, 270)}};
    HudFadePause(one, false);
    const FrameCapture f = SongFrame(one);
    vr::FrameInputs in;
    in.views[0].p[0] = -0.032f;
    in.views[1].p[0] = 0.032f;
    for (int e = 0; e < 2; e++) {
        in.views[e].p[1] = 1.6f;
        in.fov[e] = {-0.9f, 0.8f, 0.85f, -0.95f};
    }
    in.head = StandingHead();
    in.hud_boundary = vr::HudBoundary(f);
    REQUIRE(in.hud_boundary == f.post_boundary);
    const WorldCamera wc = WorldCameraOf(f);
    in.camera = freecam::Decompose(wc.view_proj, in.game, in.proj);
    REQUIRE(in.camera);
    in.tracks = vr::TrackCamerasOf(f);
    in.in_song = true;
    vr::FrameLayout l = vr::ResolveFrame(vr::Viewpoint{}, in);
    CHECK(l.window);
    REQUIRE(l.track_count == 1);
    for (int e = 0; e < 2; e++) {
        REQUIRE(l.eyes[e].track_count == 1);
        CHECK(l.eyes[e].tracks[0].cam == 0x25492B68);
    }
    // the eyes 64 mm apart in the track's units: 1.92
    const float* a = l.eyes[0].tracks[0].pose.eye;
    const float* b = l.eyes[1].tracks[0].pose.eye;
    const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    CHECK(freecam::Len3(d) == doctest::Approx(0.064f * 30).epsilon(1e-3));
    uint32_t cams[RasterOptions::kEyeCams];
    vr::EyeCamsOf(l, cams, RasterOptions::kEyeCams);
    CHECK(cams[0] == 0x25492B68);
    CHECK(cams[1] == 0);
    // from the anchor too (a viewpoint not made yet)
    vr::Viewpoint fixed;
    fixed.window = false;
    CHECK(vr::ResolveFrame(fixed, in).track_count == 1);
    // menus, and frames the eyes can't see: none
    in.in_song = false;
    CHECK(vr::ResolveFrame(vr::Viewpoint{}, in).track_count == 0);
    in.in_song = true;
    in.hud_boundary = 0;
    l = vr::ResolveFrame(vr::Viewpoint{}, in);
    CHECK(l.track_count == 0);
    CHECK(l.eyes[0].track_count == 0);
}

TEST_CASE("a cut's fade: black at the cut, back over the fade's length") {
    CHECK(vr::CutFade(0, 120) == doctest::Approx(1));
    CHECK(vr::CutFade(30'000'000, 120) == doctest::Approx(0.75f));
    CHECK(vr::CutFade(60'000'000, 120) == doctest::Approx(0.5f));
    CHECK(vr::CutFade(120'000'000, 120) == 0);
    CHECK(vr::CutFade(500'000'000, 120) == 0);
    CHECK(vr::CutFade(0, 0) == 0);  // vr_cut_fade_ms 0: no fade
    CHECK(vr::CutFade(-5, 120) == doctest::Approx(1));
}

TEST_CASE("a viewpoint's name") {
    vr::ViewMode m;
    REQUIRE(vr::ParseViewMode("game_camera", m));
    CHECK(m == vr::ViewMode::kGameCamera);
    REQUIRE(vr::ParseViewMode("on_stage", m));
    CHECK(m == vr::ViewMode::kOnStage);
    CHECK_FALSE(vr::ParseViewMode("Game_Camera", m));
}

TEST_CASE("the window's corners in an eye's picture") {
    // an eye at the head, 90 degrees each way, 1000 pixels square: the
    // window 34.8 degrees across in the middle, tan(17.4) of half across
    const vr::XrPose head = StandingHead();
    const vr::WindowFrame w = vr::WindowPlacement(head, 1.8f, 34.8f * kPi / 180);
    vr::HudQuad q;
    q.pose = w.pose;
    q.size[0] = w.size[0];
    q.size[1] = w.size[1];
    float c[4][3];
    vr::QuadCorners(q, c);
    const vr::Fov fov{-kPi / 4, kPi / 4, kPi / 4, -kPi / 4};
    float x, y;
    REQUIRE(vr::EyePixel(head, fov, 1000, 1000, c[0], x, y));  // top left
    const float half = 500 * std::tan(17.4f * kPi / 180);
    CHECK(x == doctest::Approx(500 - half).epsilon(1e-3));
    CHECK(y == doctest::Approx(500 - half * 9 / 16).epsilon(1e-3));
    REQUIRE(vr::EyePixel(head, fov, 1000, 1000, c[2], x, y));  // bottom right
    CHECK(x == doctest::Approx(500 + half).epsilon(1e-3));
    CHECK(y == doctest::Approx(500 + half * 9 / 16).epsilon(1e-3));
    // behind the eye: none
    const float behind[3] = {0, 1.6f, 1};
    CHECK_FALSE(vr::EyePixel(head, fov, 1000, 1000, behind, x, y));
}
