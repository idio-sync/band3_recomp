// Checks src/Render/vr_hud.h, VR's song HUD in pieces (vr_hud_split): each
// HUD draw's rectangle of the picture and the piece it goes to (from where
// RB3 draws them in real captures), a song's frame cut into pieces with the
// tracks' draws, a menu or a fade keeping it whole, the pieces held through a
// song, and where PlaceHudPieces hangs them for one player, two and four
// parts, through the window and from a fixed viewpoint.

#include <doctest/doctest.h>
#include <cmath>
#include <memory>
#include <vector>
#include "src/Render/free_cam.h"
#include "src/Render/soft_raster.h"
#include "src/Render/vr_hud.h"
#include "src/Render/vr_pose.h"

using namespace band3::render;

namespace {

constexpr float kPi = 3.14159265f;

// a flat card's vertices (x, y) straight to clip x and y, depth 0.5: as the
// flat HUD's camera's cards land
Mat4 FlatVp() {
    Mat4 m{};
    m.m[0][0] = 1;
    m.m[1][1] = 1;
    m.m[3][2] = 0.5f;
    m.m[3][3] = 1;
    return m;
}

// a card over u0..u1 by v0..v1 of the picture (v down), in clip space
std::shared_ptr<Geometry> Card(const vr::PictureRect& r) {
    auto g = std::make_shared<Geometry>();
    const float xs[4] = {r.u0, r.u1, r.u1, r.u0}, ys[4] = {r.v0, r.v0, r.v1, r.v1};
    for (int i = 0; i < 4; i++) {
        Vertex v{};
        v.pos[0] = 2 * xs[i] - 1;
        v.pos[1] = 1 - 2 * ys[i];
        g->verts.push_back(v);
    }
    g->indices = {0, 1, 2, 0, 2, 3};
    return g;
}

DrawItem CardDraw(uint32_t cam, const vr::PictureRect& r, float alpha = 1, int blend = 3) {
    DrawItem d{};
    d.cam = cam;
    d.geom = Card(r);
    d.view_proj = FlatVp();
    for (int i = 0; i < 4; i++) d.world.m[i][i] = 1;
    d.blend = blend;
    for (float& c : d.color) c = 1;
    d.color[3] = alpha;
    return d;
}

CameraView Camera(uint32_t cam, float z_end, float x = 0, float w = 1280) {
    CameraView v{};
    v.cam = cam;
    v.viewport[0] = x;
    v.viewport[2] = w;
    v.viewport[3] = 720;
    v.target_w = 1280;
    v.target_h = 720;
    v.zrange[1] = z_end;
    return v;
}

constexpr uint32_t kTrackCam = 0x11, kHudCam = 0x22, kFadeCam = 0x33, kPauseCam = 0x44;

// A song's frame as RB3 draws it after post-processing: the track camera's
// draws (one card over the picture's lower middle), the flat HUD camera's
// cards (z range 0..0.1), the fade camera's quad over the whole picture at
// `fade` alpha, and with `pause` the pause menu camera's panel
FrameCapture HudFrame(const std::vector<vr::PictureRect>& cards, float fade = 0,
                      bool pause = false) {
    FrameCapture f;
    f.cameras = {Camera(kTrackCam, 1), Camera(kHudCam, 0.1f), Camera(kFadeCam, 1),
                 Camera(kPauseCam, 1)};
    DrawItem world = CardDraw(1, {0, 0, 1, 1});
    f.draws.push_back(world);
    f.post_boundary = 1;
    for (int i = 0; i < 3; i++) f.draws.push_back(CardDraw(kTrackCam, {0.3f, 0.5f, 0.7f, 1}));
    for (const vr::PictureRect& r : cards) f.draws.push_back(CardDraw(kHudCam, r));
    f.draws.push_back(CardDraw(kFadeCam, {-0.04f, -0.36f, 1.04f, 1.36f}, fade));
    if (pause) f.draws.push_back(CardDraw(kPauseCam, {0.36f, 0.53f, 0.64f, 0.96f}));
    return f;
}

// the panel's options in a song: from post_boundary, the track camera the
// eyes'
RasterOptions SongPanel(const FrameCapture& f) {
    RasterOptions o;
    o.part = BackBufferPart::kHud;
    o.hud_from = f.post_boundary;
    o.eye_cams[0] = kTrackCam;
    return o;
}

// where RB3 draws them (trk_a, screen-mp2-25s, screen-mp4-10s/25s, the
// pauses: native_view_replay --overlay-only --track-eyes --hud-pieces)
const vr::PictureRect kMeter{0.040f, 0.311f, 0.065f, 0.693f};
const vr::PictureRect kMeterIcon{0.054f, 0.361f, 0.089f, 0.418f};
const vr::PictureRect kScoreBox{0.788f, 0.197f, 0.971f, 0.273f};
const vr::PictureRect kProgressIcon{0.767f, 0.326f, 0.792f, 0.364f};
const vr::PictureRect kTime{0.888f, 0.359f, 0.968f, 0.395f};
const vr::PictureRect kTitle{0.638f, 0.212f, 0.773f, 0.251f};
const vr::PictureRect kArtist{0.725f, 0.238f, 0.773f, 0.273f};
const vr::PictureRect kLyrics{0.484f, 0.106f, 0.517f, 0.138f};
const vr::PictureRect kVocals{0.017f, 0.075f, 0.983f, 0.211f};
const vr::PictureRect kPills{0.221f, 0.267f, 0.289f, 0.294f};
const vr::PictureRect kUnison{0.398f, 0.271f, 0.602f, 0.366f};
const vr::PictureRect kUnisonIcon{0.470f, 0.350f, 0.530f, 0.450f};
const vr::PictureRect kPauseMenu{0.086f, 0.625f, 0.322f, 0.971f};

void CheckRect(const vr::PictureRect& a, const vr::PictureRect& b, float eps = 1e-4f) {
    CHECK(a.u0 == doctest::Approx(b.u0).epsilon(eps));
    CHECK(a.v0 == doctest::Approx(b.v0).epsilon(eps));
    CHECK(a.u1 == doctest::Approx(b.u1).epsilon(eps));
    CHECK(a.v1 == doctest::Approx(b.v1).epsilon(eps));
}

vr::PictureRect Padded(const vr::PictureRect& r) {
    return {std::fmax(0.0f, r.u0 - vr::kHudPad), std::fmax(0.0f, r.v0 - vr::kHudPad),
            std::fmin(1.0f, r.u1 + vr::kHudPad), std::fmin(1.0f, r.v1 + vr::kHudPad)};
}

}  // namespace

TEST_CASE("a HUD draw's rectangle of the picture: its corners through its camera, padded") {
    FrameCapture f = HudFrame({});
    vr::PictureRect r;
    REQUIRE(vr::HudDrawRect(f, CardDraw(kHudCam, {0.25f, 0.1f, 0.5f, 0.3f}), r));
    CheckRect(r, Padded({0.25f, 0.1f, 0.5f, 0.3f}));
    // through a camera's viewport: the right half of the picture
    f.cameras.push_back(Camera(0x55, 0.1f, 640, 640));
    REQUIRE(vr::HudDrawRect(f, CardDraw(0x55, {0, 0.5f, 0.5f, 1}), r));
    CheckRect(r, Padded({0.5f, 0.5f, 0.75f, 1}));
    // a DrawRect quad's rect, in the back buffer's pixels
    DrawItem rect = CardDraw(0, {0, 0, 1, 1});
    rect.rect_shader = 6;
    rect.rect[0] = 128;
    rect.rect[1] = 72;
    rect.rect[2] = 256;
    rect.rect[3] = 144;
    REQUIRE(vr::HudDrawRect(f, rect, r));
    CheckRect(r, Padded({0.1f, 0.1f, 0.3f, 0.3f}));
    // nothing shows: blended away at alpha 0, Dest, a point
    CHECK_FALSE(vr::HudDrawRect(f, CardDraw(kHudCam, kScoreBox, 0), r));
    CHECK_FALSE(vr::HudDrawRect(f, CardDraw(kHudCam, kScoreBox, 0, 4), r));
    CHECK_FALSE(vr::HudDrawRect(f, CardDraw(kHudCam, kScoreBox, 1, 0), r));
    CHECK_FALSE(vr::HudDrawRect(f, CardDraw(kHudCam, {0.6f, 0.9f, 0.6f, 0.9f}), r));
    // Add shows whatever its alpha
    CHECK(vr::HudDrawRect(f, CardDraw(kHudCam, kScoreBox, 0, 2), r));
    // kept within the picture
    REQUIRE(vr::HudDrawRect(f, CardDraw(kHudCam, {-0.1f, -0.2f, 0.2f, 0.3f}), r));
    CHECK(r.u0 == 0);
    CHECK(r.v0 == 0);
}

TEST_CASE("HUD draws go to pieces by where RB3 lays them out") {
    using vr::HudPiece;
    for (const vr::PictureRect& r : {kMeter, kMeterIcon})
        CHECK(vr::HudPieceOf(Padded(r)) == HudPiece::kMeter);
    for (const vr::PictureRect& r : {kScoreBox, kProgressIcon, kTime})
        CHECK(vr::HudPieceOf(Padded(r)) == HudPiece::kScore);
    // the song's title's lines right-aligned against the score, however
    // short; the lyrics; the vocals' strip and its pills; a Unison Bonus
    for (const vr::PictureRect& r :
         {kTitle, kArtist, kLyrics, kVocals, kPills, kUnison, kUnisonIcon}) {
        CAPTURE(r.u0);
        CAPTURE(r.v0);
        CHECK(vr::HudPieceOf(Padded(r)) == HudPiece::kTop);
    }
    CHECK(vr::HudPieceOf(Padded(kPauseMenu)) == HudPiece::kRest);
    CHECK(vr::HudPieceOf(Padded({0.4f, 0.6f, 0.6f, 0.7f})) == HudPiece::kRest);
}

TEST_CASE("a song's HUD in pieces: the tracks' draws aren't, a menu or a fade over it keeps it whole") {
    const std::vector<vr::PictureRect> cards = {kMeter, kMeterIcon, kScoreBox, kProgressIcon,
                                                kTime,  kTitle,     kLyrics};
    FrameCapture f = HudFrame(cards);
    vr::HudPieces p = vr::HudPiecesOf(f, SongPanel(f));
    CHECK(p.split);
    CHECK(p.full == 0);
    CHECK(p.menu == 0);
    CHECK(p.draws[int(vr::HudPiece::kMeter)] == 2);
    CHECK(p.draws[int(vr::HudPiece::kScore)] == 3);
    CHECK(p.draws[int(vr::HudPiece::kTop)] == 2);
    CHECK(p.draws[int(vr::HudPiece::kRest)] == 0);
    CheckRect(p.rect[int(vr::HudPiece::kMeter)], Padded(vr::Union(kMeter, kMeterIcon)));
    CheckRect(p.rect[int(vr::HudPiece::kScore)],
              Padded(vr::Union(kScoreBox, vr::Union(kProgressIcon, kTime))));
    const vr::PictureRect top = Padded(vr::Union(kTitle, kLyrics));
    CheckRect(p.rect[int(vr::HudPiece::kTop)], top);
    // each hung by a corner: the meter's bottom right, the score's bottom
    // left, the top's bottom edge at the picture's middle
    CHECK(p.anchor[0][0] == doctest::Approx(Padded(kMeterIcon).u1));
    CHECK(p.anchor[0][1] == doctest::Approx(Padded(kMeter).v1));
    CHECK(p.anchor[1][0] == doctest::Approx(Padded(kProgressIcon).u0));
    CHECK(p.anchor[1][1] == doctest::Approx(Padded(kTime).v1));
    CHECK(p.anchor[2][0] == 0.5f);
    CHECK(p.anchor[2][1] == doctest::Approx(top.v1));
    // the track's own draws are the eyes'; given to the panel they'd be the
    // rest's, too much of it to split
    RasterOptions flat = SongPanel(f);
    flat.eye_cams[0] = 0;
    p = vr::HudPiecesOf(f, flat);
    CHECK(p.draws[int(vr::HudPiece::kRest)] == 3);
    CHECK_FALSE(p.split);
    // the fade at work covers the whole picture: whole
    f = HudFrame(cards, 0.5f);
    p = vr::HudPiecesOf(f, SongPanel(f));
    CHECK(p.full == 1);
    CHECK_FALSE(p.split);
    // the pause menu, a 0..1 camera after the flat HUD's: whole
    f = HudFrame(cards, 0, true);
    p = vr::HudPiecesOf(f, SongPanel(f));
    CHECK(p.menu == 1);
    CHECK_FALSE(p.split);
    // a panel that's the whole picture (no world the eyes see): whole
    RasterOptions all = SongPanel(f);
    all.hud_from = 0;
    CHECK_FALSE(vr::HudPiecesOf(f, all).split);
}

TEST_CASE("the pieces held through a song: grown, never shrunk, hung where first seen") {
    vr::HudPieceHold hold;
    FrameCapture f = HudFrame({kMeter, kScoreBox, kLyrics});
    vr::HudPieces held = hold.Hold(vr::HudPiecesOf(f, SongPanel(f)));
    REQUIRE(held.split);
    const float lyrics_bottom = Padded(kLyrics).v1;
    CHECK(held.anchor[2][1] == doctest::Approx(lyrics_bottom));
    // a shorter line, the title, then a Unison Bonus under the lyrics: the
    // top grows, still hung by the lyrics' bottom
    const vr::PictureRect short_line{0.49f, 0.106f, 0.51f, 0.138f};
    f = HudFrame({kMeter, kScoreBox, short_line, kTitle, kUnison});
    held = hold.Hold(vr::HudPiecesOf(f, SongPanel(f)));
    CheckRect(held.rect[int(vr::HudPiece::kTop)],
              Padded(vr::Union(kLyrics, vr::Union(kTitle, kUnison))));
    CHECK(held.anchor[2][1] == doctest::Approx(lyrics_bottom));
    // and stays so when they've gone
    f = HudFrame({kMeter, kScoreBox, short_line});
    held = hold.Hold(vr::HudPiecesOf(f, SongPanel(f)));
    CheckRect(held.rect[int(vr::HudPiece::kTop)],
              Padded(vr::Union(kLyrics, vr::Union(kTitle, kUnison))));
    // the pause menu: the pieces as they were for a frame or two, then whole
    f = HudFrame({kMeter, kScoreBox, kLyrics}, 0, true);
    const vr::HudPieces paused = vr::HudPiecesOf(f, SongPanel(f));
    REQUIRE_FALSE(paused.split);
    for (int i = 1; i < vr::kHudWholeAfter; i++) CHECK(hold.Hold(paused).split);
    CHECK_FALSE(hold.Hold(paused).split);
    CHECK_FALSE(hold.Hold(paused).split);
    // back: the pieces as before, the menu's not among them
    f = HudFrame({kMeter, kScoreBox, kLyrics});
    held = hold.Hold(vr::HudPiecesOf(f, SongPanel(f)));
    CHECK(held.split);
    CHECK(held.rect[int(vr::HudPiece::kRest)].Empty());
    CheckRect(held.rect[int(vr::HudPiece::kTop)],
              Padded(vr::Union(kLyrics, vr::Union(kTitle, kUnison))));
    // the next song starts afresh
    hold.Reset();
    held = hold.Hold(vr::HudPiecesOf(f, SongPanel(f)));
    CheckRect(held.rect[int(vr::HudPiece::kTop)], Padded(kLyrics));
}

namespace {

// a track camera's projection and view as RB3's (vr_pose_test.cpp's)
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

Mat4 TrackVp(const float eye[3], float yaw, float pitch, const Mat4& proj) {
    const float y = yaw * kPi / 180, p = pitch * kPi / 180;
    const float at[3] = {eye[0] - std::sin(y) * std::cos(p) * 100,
                         eye[1] + std::cos(y) * std::cos(p) * 100, eye[2] - std::sin(p) * 100};
    return freecam::Mul(freecam::View(freecam::LookAt(eye, at)), proj);
}

void AddTrack(vr::TrackCams& t, const float eye[3], float yaw, float pitch, const Mat4& proj,
              float x = 0, float w = 1280) {
    vr::TrackCamera c;
    c.cam = uint32_t(0x100 + t.count);
    c.view_proj = TrackVp(eye, yaw, pitch, proj);
    REQUIRE(freecam::Decompose(c.view_proj, c.pose, c.proj));
    c.viewport[0] = x;
    c.viewport[2] = w;
    c.viewport[3] = 720;
    c.target_w = 1280;
    c.target_h = 720;
    t.cams[t.count++] = c;
}

// one player's (trk_a), two players' (screen-mp2-25s), four parts'
// (screen-mp4-10s)
vr::TrackCams Tracks(int players) {
    vr::TrackCams t;
    if (players == 1) {
        const float e[3] = {0, -91.82f, 22.6f};
        AddTrack(t, e, 0, 7, TrackProj(36.56f, 30, 224));
    } else if (players == 2) {
        const float e[3] = {0, -104.82f, 26.39f};
        AddTrack(t, e, 0, 7, TrackProj(28.82f, 40.6f, 258, -0.2826f), 0, 998);
        AddTrack(t, e, 0, 7, TrackProj(28.82f, 40.6f, 258, 0.2826f), 281, 998);
    } else {
        const float a[3] = {2.63f, -122.14f, 29.54f}, b[3] = {-2.71f, -122.35f, 28.71f},
                    c[3] = {0, -123.01f, 29.69f};
        const Mat4 p = TrackProj(36.56f, 50, 305);
        AddTrack(t, a, -10, 6, p);
        AddTrack(t, b, 10, 6, p);
        AddTrack(t, c, 0, 6, p);
    }
    return t;
}

vr::XrPose StandingHead() {
    vr::XrPose h;
    h.p[1] = 1.6f;
    return h;
}

// a song's layout with the stand-in head standing at the origin facing -z:
// through the game camera's window, or from a fixed viewpoint
vr::FrameLayout SongLayout(const vr::TrackCams& t, bool window) {
    vr::FrameInputs in;
    for (int e = 0; e < 2; e++) {
        in.views[e] = StandingHead();
        in.views[e].p[0] = e ? 0.032f : -0.032f;
        in.fov[e] = {-0.9f, 0.9f, 0.9f, -0.9f};
    }
    in.head = StandingHead();
    in.hud_boundary = 10;
    in.camera = true;
    const float eye[3] = {0, -1000, 200}, at[3] = {0, 0, 200};
    in.game = freecam::LookAt(eye, at);
    in.proj.m[0][0] = 1;
    in.proj.m[2][1] = 16.0f / 9;
    in.proj.m[1][2] = 1.001f;
    in.proj.m[3][2] = -10.01f;
    in.proj.m[1][3] = 1;
    in.in_song = true;
    in.tracks = t;
    vr::Viewpoint vp;
    vp.window = window;
    return vr::ResolveFrame(vp, in);
}

// one player's pieces: the meter, the score, the lyrics and title
vr::HudPieces SongPieces(std::vector<vr::PictureRect> extra = {}) {
    std::vector<vr::PictureRect> cards = {kMeter, kMeterIcon, kScoreBox, kProgressIcon,
                                          kTime,  kTitle,     kLyrics};
    cards.insert(cards.end(), extra.begin(), extra.end());
    const FrameCapture f = HudFrame(cards);
    return vr::HudPiecesOf(f, SongPanel(f));
}

const vr::HudPieceQuad* Find(const vr::HudSplit& s, vr::HudPiece kind) {
    for (int i = 0; i < s.count; i++)
        if (s.piece[i].kind == kind) return &s.piece[i];
    return nullptr;
}

// where a piece's picture point (u, v) is in LOCAL space
void PointOf(const vr::HudPieceQuad& q, float u, float v, float out[3]) {
    const vr::PictureRect& r = q.rect;
    const float sx = q.quad.size[0] / (r.u1 - r.u0), sy = q.quad.size[1] / (r.v1 - r.v0);
    const float off[3] = {(u - (r.u0 + r.u1) / 2) * sx, -(v - (r.v0 + r.v1) / 2) * sy, 0};
    float t[3];
    vr::Rotate(q.quad.pose.q, off, t);
    for (int k = 0; k < 3; k++) out[k] = q.quad.pose.p[k] + t[k];
}

// that the quad faces the head from its anchor `at`, and is the size its
// rectangle would be in the whole picture `width_deg` across from there
void CheckFacing(const vr::HudPieceQuad& q, const float at[3], const vr::XrPose& head,
                 float width_deg) {
    const float back[3] = {0, 0, 1};
    float n[3];
    vr::Rotate(q.quad.pose.q, back, n);
    const float to[3] = {head.p[0] - at[0], head.p[1] - at[1], head.p[2] - at[2]};
    const float d = freecam::Len3(to);
    CHECK(freecam::Dot3(n, to) / d == doctest::Approx(1).epsilon(1e-4));
    // level: its x axis along the floor
    const float x[3] = {1, 0, 0};
    float right[3];
    vr::Rotate(q.quad.pose.q, x, right);
    CHECK(right[1] == doctest::Approx(0).epsilon(1e-5));
    const float sx = 2 * d * std::tan(width_deg * kPi / 360);
    CHECK(q.quad.size[0] == doctest::Approx((q.rect.u1 - q.rect.u0) * sx).epsilon(1e-4));
    CHECK(q.quad.size[1] ==
          doctest::Approx((q.rect.v1 - q.rect.v0) * sx / vr::kWindowAspect).epsilon(1e-4));
}

}  // namespace

TEST_CASE("one player's meter and score beside the track at its strikeline, facing the head") {
    const vr::FrameLayout l = SongLayout(Tracks(1), true);
    REQUIRE(l.window);
    REQUIRE(l.track_count == 1);
    const vr::HudPieces p = SongPieces();
    const vr::HudSplitLayout s;
    const vr::HudSplit split = vr::PlaceHudPieces(l, StandingHead(), p, s);
    REQUIRE(split.count == 3);
    CHECK(split.beside);
    const vr::HudPieceQuad* meter = Find(split, vr::HudPiece::kMeter);
    const vr::HudPieceQuad* score = Find(split, vr::HudPiece::kScore);
    REQUIRE(meter);
    REQUIRE(score);
    // the track is 19.6 of its units across at 30 a metre, its strikeline
    // 1.2 m ahead and 0.3 below the eyes: the meter's bottom right corner
    // side_gap left of its left edge, the score's bottom left right of its
    // right edge
    const float edge = vr::kTrackHalfWidth / 30;
    float c[3];
    PointOf(*meter, p.anchor[0][0], p.anchor[0][1], c);
    const float meter_at[3] = {-edge - s.side_gap, 1.3f, -1.2f};
    for (int k = 0; k < 3; k++) CHECK(c[k] == doctest::Approx(meter_at[k]).epsilon(1e-4));
    float corners[4][3];
    vr::QuadCorners(meter->quad, corners);
    CHECK(corners[2][0] == doctest::Approx(c[0]).epsilon(1e-3));  // its bottom right
    CHECK(corners[2][1] == doctest::Approx(c[1]).epsilon(1e-3));
    PointOf(*score, p.anchor[1][0], p.anchor[1][1], c);
    const float score_at[3] = {edge + s.side_gap, 1.3f, -1.2f};
    for (int k = 0; k < 3; k++) CHECK(c[k] == doctest::Approx(score_at[k]).epsilon(1e-4));
    vr::QuadCorners(score->quad, corners);
    CHECK(corners[3][0] == doctest::Approx(c[0]).epsilon(1e-3));  // its bottom left
    // rising from there, clear of the track
    CHECK(meter->quad.pose.p[0] < -edge);
    CHECK(meter->quad.pose.p[1] > 1.3f);
    CHECK(score->quad.pose.p[0] > edge);
    CheckFacing(*meter, meter_at, StandingHead(), s.piece_width);
    CheckFacing(*score, score_at, StandingHead(), s.piece_width);
    // as big as they look on the panel at 55 degrees: the meter's 0.38 of
    // the picture's height about 13 degrees tall from 1.3 m
    const float tall = 2 * std::atan2(meter->quad.size[1] / 2, 1.3f) * 180 / kPi;
    CHECK(tall == doctest::Approx(12.7f).epsilon(0.03));
    // a head turned to the side: the same, turned with it
    vr::XrPose turned = StandingHead();
    turned.q[1] = std::sin(0.3f);
    turned.q[3] = std::cos(0.3f);
    const vr::FrameLayout lt = [&] {
        vr::FrameLayout x = SongLayout(Tracks(1), true);
        vr::TrackPlace places[vr::kMaxTrackCams];
        x.track_count = vr::PlaceTracks(Tracks(1), turned, vr::TrackLayout{}, places);
        for (int i = 0; i < x.track_count; i++) x.tracks[i] = places[i];
        return x;
    }();
    const vr::HudSplit st = vr::PlaceHudPieces(lt, turned, p, s);
    REQUIRE(st.count == 3);
    PointOf(*Find(st, vr::HudPiece::kMeter), p.anchor[0][0], p.anchor[0][1], c);
    // 0.6 radians left: the meter's anchor turned about the head
    const float a = 0.6f, x0 = meter_at[0], z0 = meter_at[2];
    CHECK(c[0] == doctest::Approx(x0 * std::cos(a) + z0 * std::sin(a)).epsilon(1e-3));
    CHECK(c[2] == doctest::Approx(-x0 * std::sin(a) + z0 * std::cos(a)).epsilon(1e-3));
}

TEST_CASE("the top strip above the window, or ahead and up from a fixed viewpoint") {
    const vr::HudPieces p = SongPieces();
    const vr::HudSplitLayout s;
    // through the window: its bottom edge's middle 0.05 m over the window's
    // top edge, at the window's distance, facing the head
    vr::FrameLayout l = SongLayout(Tracks(1), true);
    vr::HudSplit split = vr::PlaceHudPieces(l, StandingHead(), p, s);
    const vr::HudPieceQuad* top = Find(split, vr::HudPiece::kTop);
    REQUIRE(top);
    float c[3], want[3];
    PointOf(*top, 0.5f, p.anchor[2][1], c);
    const float edge[3] = {0, l.window_frame.size[1] / 2 + s.lyrics_height, 0};
    vr::WindowToLocal(l.window_frame, edge, want);
    for (int k = 0; k < 3; k++) CHECK(c[k] == doctest::Approx(want[k]).epsilon(1e-4));
    CHECK(want[2] == doctest::Approx(-1.8f).epsilon(1e-4));
    CHECK(want[1] > 1.6f + l.window_frame.size[1] / 2);
    CheckFacing(*top, want, StandingHead(), s.piece_width);
    // the lyrics (the picture's middle) straight ahead of the head; the
    // title to their right, as in the picture
    CHECK(top->quad.pose.p[0] > 0);
    // from a fixed viewpoint: 2.5 m ahead and 15 degrees up
    l = SongLayout(Tracks(1), false);
    REQUIRE_FALSE(l.window);
    split = vr::PlaceHudPieces(l, StandingHead(), p, s);
    top = Find(split, vr::HudPiece::kTop);
    REQUIRE(top);
    PointOf(*top, 0.5f, p.anchor[2][1], c);
    CHECK(c[0] == doctest::Approx(0).epsilon(1e-4));
    CHECK(c[1] == doctest::Approx(1.6f + 2.5f * std::tan(15 * kPi / 180)).epsilon(1e-4));
    CHECK(c[2] == doctest::Approx(-2.5f).epsilon(1e-4));
    // the meter and score as through the window
    CHECK(split.beside);
    CHECK(Find(split, vr::HudPiece::kMeter));
}

TEST_CASE("two players' and four parts' meter and score above the tracks' middle") {
    const vr::HudPieces p = SongPieces();
    vr::HudSplitLayout s;
    for (int players : {2, 4}) {
        CAPTURE(players);
        const vr::FrameLayout l = SongLayout(Tracks(players), true);
        REQUIRE(l.track_count == (players == 2 ? 2 : 3));
        vr::HudSplit split = vr::PlaceHudPieces(l, StandingHead(), p, s);
        REQUIRE(split.count == 3);
        // beside the outermost tracks they'd be over 40 degrees aside
        CHECK_FALSE(split.beside);
        const vr::HudPieceQuad* meter = Find(split, vr::HudPiece::kMeter);
        const vr::HudPieceQuad* score = Find(split, vr::HudPiece::kScore);
        float m[3], sc[3];
        PointOf(*meter, p.anchor[0][0], p.anchor[0][1], m);
        PointOf(*score, p.anchor[1][0], p.anchor[1][1], sc);
        // side_gap apart about the middle (four parts' cameras aren't quite
        // symmetric: 2 mm right)
        CHECK(sc[0] - m[0] == doctest::Approx(s.side_gap).epsilon(1e-3));
        CHECK(std::fabs(m[0] + sc[0]) / 2 < 0.005f);
        CHECK(m[1] == doctest::Approx(sc[1]));
        // their bottoms over every track's far end, seen from the head
        float above = -1;
        for (int t = 0; t < l.track_count; t++) {
            const float far[3] = {0, vr::kTrackFar, 0};
            float f[3];
            vr::TrackToLocal(l.tracks[t], l.track_scale, far, f);
            above = std::fmax(above, (f[1] - 1.6f) / std::sqrt(f[0] * f[0] + f[2] * f[2]));
        }
        CHECK((m[1] - 1.6f) / std::sqrt(m[0] * m[0] + m[2] * m[2]) > above);
        // nearly straight ahead
        CHECK(std::fabs(std::atan2(meter->quad.pose.p[0], -meter->quad.pose.p[2])) < 0.2f);
        // allowed further aside (two players' at 38 and 42 degrees, four
        // parts' at 48 and 50), beside the outermost tracks
        s.side_max = 60;
        split = vr::PlaceHudPieces(l, StandingHead(), p, s);
        CHECK(split.beside);
        s.side_max = 40;
    }
}

TEST_CASE("the rest where it is on the whole panel; no split where it can't be or isn't wanted") {
    const vr::FrameLayout l = SongLayout(Tracks(1), true);
    const vr::PictureRect odd{0.45f, 0.55f, 0.55f, 0.65f};
    const vr::HudPieces p = SongPieces({odd});
    REQUIRE(p.split);
    const vr::HudSplit split = vr::PlaceHudPieces(l, StandingHead(), p, vr::HudSplitLayout{});
    REQUIRE(split.count == 4);
    const vr::HudPieceQuad* rest = Find(split, vr::HudPiece::kRest);
    REQUIRE(rest);
    // as the panel shows it: its middle where the picture's is on the panel
    const vr::PictureRect r = Padded(odd);
    const float off[3] = {((r.u0 + r.u1) / 2 - 0.5f) * l.hud_quad.size[0],
                          -((r.v0 + r.v1) / 2 - 0.5f) * l.hud_quad.size[1], 0};
    float t[3];
    vr::Rotate(l.hud_quad.pose.q, off, t);
    for (int k = 0; k < 3; k++)
        CHECK(rest->quad.pose.p[k] == doctest::Approx(l.hud_quad.pose.p[k] + t[k]).epsilon(1e-4));
    CHECK(rest->quad.size[0] == doctest::Approx((r.u1 - r.u0) * l.hud_quad.size[0]));
    // off, a HUD that can't be split, the panel in the window, no tracks
    // (menus), black eyes: the whole panel
    vr::HudSplitLayout off_layout;
    off_layout.on = false;
    CHECK(vr::PlaceHudPieces(l, StandingHead(), p, off_layout).count == 0);
    vr::HudPieces whole = p;
    whole.split = false;
    CHECK(vr::PlaceHudPieces(l, StandingHead(), whole, {}).count == 0);
    vr::FrameLayout in_window = l;
    in_window.hud_in_window = true;
    CHECK(vr::PlaceHudPieces(in_window, StandingHead(), p, {}).count == 0);
    vr::FrameLayout menu = l;
    menu.track_count = 0;
    CHECK(vr::PlaceHudPieces(menu, StandingHead(), p, {}).count == 0);
    vr::FrameLayout black = l;
    black.eyes_black = true;
    CHECK(vr::PlaceHudPieces(black, StandingHead(), p, {}).count == 0);
}

TEST_CASE("a piece's texels: whole ones, outward, within the image") {
    int32_t x = 0, y = 0, w = 0, h = 0;
    vr::PieceTexels({0.1f, 0.2f, 0.3f, 0.4f}, 2644, 2644, x, y, w, h);
    CHECK(x == 264);  // 264.4 down
    CHECK(y == 528);  // 528.8 down
    CHECK(x + w == 794);  // 793.2 up
    CHECK(y + h == 1058);  // 1057.6 up
    vr::PieceTexels({0.9f, 0.9f, 1.0f, 1.0f}, 100, 50, x, y, w, h);
    CHECK(x + w == 100);
    CHECK(y + h == 50);
    vr::PieceTexels({0, 0, 0.5f, 0.5f}, 100, 100, x, y, w, h);
    CHECK(x == 0);
    CHECK(w == 50);
}
