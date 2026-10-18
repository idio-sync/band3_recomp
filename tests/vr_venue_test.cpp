// Checks src/Render/vr_venue.h, VR's fixed viewpoints: the venue found from
// the band's and the crowd's draws (VenueRefOf), the places front_row,
// on_stage and venue put the head (PresetAnchor), the same against the band
// wherever and however turned a venue lays it out, the song's reference
// kept and taken again (VenueTracker), and the crowd left out near the eye
// (RePose with Eye::crowd_clear).

#include <doctest/doctest.h>
#include <cmath>
#include <memory>
#include <vector>
#include "src/Render/free_cam.h"
#include "src/Render/vr_pose.h"
#include "src/Render/vr_venue.h"

using namespace band3::render;

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kM = vr::kUnitsPerMetre;

// a box `w` wide and `h` tall standing on its origin
std::shared_ptr<Geometry> Box(float w, float h) {
    auto g = std::make_shared<Geometry>();
    for (int i = 0; i < 8; i++) {
        Vertex v{};
        v.pos[0] = (i & 1 ? 1 : -1) * w / 2;
        v.pos[1] = (i & 2 ? 1 : -1) * w / 2;
        v.pos[2] = i & 4 ? h : 0;
        g->verts.push_back(v);
    }
    g->indices = {0, 1, 2, 1, 3, 2, 4, 5, 6, 5, 7, 6};
    return g;
}

// A venue laid out as arena_04 is, then turned `turn` radians about z and
// moved by `shift`: four members on a stage floor at z 256 (the drummer at
// the back), a crowd of cards on a floor at z 200 from 4 m to 16 m in front
// and a few either side of the stage, all seen by one camera from behind the
// crowd
struct Layout {
    float turn = 0;
    float shift[3] = {0, 0, 0};
    bool crowd = true;
    int members = 4;
    void Place(const float in[3], float out[3]) const {
        const float c = std::cos(turn), s = std::sin(turn);
        out[0] = in[0] * c - in[1] * s + shift[0];
        out[1] = in[0] * s + in[1] * c + shift[1];
        out[2] = in[2] + shift[2];
    }
};

// the members' feet before the layout's turn: guitar, vocals, bass, drums
const float kMembers[4][3] = {
    {-110, -530, 256}, {0, -560, 256}, {110, -530, 256}, {0, -420, 256}};

FrameCapture VenueFrame(const Layout& l, std::vector<uint32_t>* crowd_meshes = nullptr) {
    FrameCapture f;
    float eye[3], at[3];
    const float eye0[3] = {0, -1600, 500}, at0[3] = {0, -500, 256};
    l.Place(eye0, eye);
    l.Place(at0, at);
    Mat4 proj{};
    proj.m[0][0] = 1;
    proj.m[2][1] = 16.0f / 9;
    proj.m[1][2] = 1.001f;
    proj.m[3][2] = -10.01f;
    proj.m[1][3] = 1;
    const Mat4 vp = freecam::Mul(freecam::View(freecam::LookAt(eye, at)), proj);
    auto add = [&](const std::shared_ptr<Geometry>& g, const float pos[3], uint32_t mesh) {
        DrawItem d{};
        d.geom = g;
        d.view_proj = vp;
        d.cam = 1;
        d.mesh = mesh;
        for (int i = 0; i < 4; i++) d.world.m[i][i] = 1;
        float p[3];
        l.Place(pos, p);
        for (int c = 0; c < 3; c++) d.world.m[3][c] = p[c];
        f.draws.push_back(d);
    };
    // the venue itself: a floor nobody owns
    const float origin[3] = {0, 0, 0};
    add(Box(4000, 10), origin, 0x1000);
    // each member two meshes (a body and a guitar), each its own owner
    const auto body = Box(20, 70), guitar = Box(10, 10);
    for (int m = 0; m < l.members; m++) {
        const uint32_t owner = 0x5000 + m * 0x100;
        const uint32_t mesh = 0x2000 + m * 2;
        add(body, kMembers[m], mesh);
        const float hands[3] = {kMembers[m][0], kMembers[m][1], kMembers[m][2] + 40};
        add(guitar, hands, mesh + 1);
        f.mesh_owners.push_back({mesh, owner, kOwnerBand});
        f.mesh_owners.push_back({mesh + 1, owner, kOwnerBand});
    }
    if (l.crowd) {
        // cards a metre apart: 13 rows of 21 in front, 4 m to 16 m from the
        // band's middle (y -500), and a block beside the stage; one mesh per
        // four rows, as multimeshes share theirs
        const auto card = Box(16, 74);
        for (int row = 0; row < 13; row++)
            for (int col = -10; col <= 10; col++) {
                const float p[3] = {col * kM, -500 - (4 + row) * kM, 200};
                const uint32_t mesh = 0x3000 + row / 4;
                add(card, p, mesh);
            }
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 8; col++) {
                const float side = col < 4 ? 1.0f : -1.0f;
                const float p[3] = {side * (12 + col % 4) * kM, -500 + (row - 2) * kM, 200};
                add(card, p, 0x3100);
            }
        for (uint32_t mesh : {0x3000u, 0x3001u, 0x3002u, 0x3003u, 0x3100u}) {
            f.mesh_owners.push_back({mesh, 0x7000, kOwnerCrowd});
            if (crowd_meshes) crowd_meshes->push_back(mesh);
        }
    }
    f.post_boundary = uint32_t(f.draws.size());
    f.proc_cmds = 7;
    return f;
}

// `p` (venue) back through the layout's turn and shift
void Unplace(const Layout& l, const float p[3], float out[3]) {
    const float d[3] = {p[0] - l.shift[0], p[1] - l.shift[1], p[2] - l.shift[2]};
    const float c = std::cos(-l.turn), s = std::sin(-l.turn);
    out[0] = d[0] * c - d[1] * s;
    out[1] = d[0] * s + d[1] * c;
    out[2] = d[2];
}

// the anchor's facing along the floor
void Facing(const vr::Anchor& a, float f[2]) {
    f[0] = -std::sin(a.yaw);
    f[1] = std::cos(a.yaw);
}

}  // namespace

TEST_CASE("the venue from the band's and the crowd's draws: the stage, its floor, the audience") {
    const Layout l;
    const vr::VenueRef r = vr::VenueRefOf(VenueFrame(l));
    REQUIRE(r.members == 4);
    // the members' middles (a guitar at the hands doesn't move the feet)
    CHECK(r.member[0][0] == doctest::Approx(-110));
    CHECK(r.member[0][1] == doctest::Approx(-530));
    CHECK(r.member[0][2] == doctest::Approx(256));
    CHECK(r.centre[0] == doctest::Approx(0));
    CHECK(r.centre[1] == doctest::Approx(-510));
    CHECK(r.centre[2] == doctest::Approx(256));
    // the audience is toward the crowd's middle: -y here
    CHECK(r.toward_crowd);
    CHECK(std::fabs(r.toward[0]) < 1e-4f);
    CHECK(r.toward[1] == doctest::Approx(-1));
    CHECK(r.band_front == doctest::Approx(50).epsilon(1e-2));    // the singer, 50 ahead
    CHECK(r.band_back == doctest::Approx(-90).epsilon(1e-2));    // the drummer
    CHECK(r.band_half == doctest::Approx(110));
    // the cards beside the stage aren't the front crowd; the front's first
    // row is 4 m in front of y -500, its floor at z 200
    CHECK(r.crowd == 13 * 21 + 40);
    // the columns within the band's width and 4 m (-6..6)
    CHECK(r.crowd_ahead == 13 * 13);
    CHECK(r.crowd_front == doctest::Approx(4 * kM - 10));
    CHECK(r.crowd_back == doctest::Approx(15 * kM - 10));
    CHECK(r.audience_floor == doctest::Approx(200));
    // without the band's marks, nothing
    FrameCapture none = VenueFrame(l);
    none.mesh_owners.clear();
    CHECK_FALSE(vr::VenueRefOf(none).Valid());
}

TEST_CASE("the fixed viewpoints: at the barrier, among the band, behind the crowd") {
    const Layout l;
    const vr::VenueRef r = vr::VenueRefOf(VenueFrame(l));
    const float units = kM;
    vr::Anchor front, stage, venue;
    REQUIRE(vr::PresetAnchor(vr::ViewMode::kFrontRow, r, {}, units, front));
    REQUIRE(vr::PresetAnchor(vr::ViewMode::kOnStage, r, {}, units, stage));
    REQUIRE(vr::PresetAnchor(vr::ViewMode::kVenue, r, {}, units, venue));
    CHECK_FALSE(vr::PresetAnchor(vr::ViewMode::kGameCamera, r, {}, units, front));
    // front_row: on the band's middle line, 0.3 m in front of the first row,
    // eyes 1.7 m over the audience's floor, facing the band (+y: yaw 0)
    CHECK(std::fabs(front.pos[0] - r.centre[0]) < 1e-3f);
    CHECK(front.pos[1] == doctest::Approx(r.centre[1] - (r.crowd_front - 0.3f * kM)));
    CHECK(front.pos[2] == doctest::Approx(200 + 1.7f * kM));
    CHECK(std::fabs(front.yaw) < 1e-4f);
    // under the stage's floor, above the audience's
    CHECK(front.pos[2] < 256 + 1.7f * kM);
    // on_stage: eyes 1.7 m over the stage floor, facing the audience (-y:
    // yaw pi), at least 0.9 m from every member, between the drummer and a
    // little past the singer
    CHECK(stage.pos[2] == doctest::Approx(256 + 1.7f * kM));
    CHECK(std::fabs(std::fabs(stage.yaw) - kPi) < 1e-4f);
    for (int m = 0; m < r.members; m++)
        CHECK(std::hypot(stage.pos[0] - r.member[m][0], stage.pos[1] - r.member[m][1]) >=
              0.9f * kM - 1e-3f);
    const float stage_depth = r.centre[1] - stage.pos[1];
    CHECK(stage_depth >= r.band_back - 1e-3f);
    CHECK(stage_depth <= r.band_front + 0.5f * kM + 1e-3f);
    CHECK(std::fabs(stage.pos[0] - r.centre[0]) <= r.band_half + kM);
    // nobody between it and the audience: no member downstage of it within
    // 0.5 m across (the singer, 50 ahead of the middle, is beside it)
    for (int m = 0; m < r.members; m++) {
        const float ahead = (r.centre[1] - r.member[m][1]) - stage_depth;
        if (ahead > 0) CHECK(std::fabs(r.member[m][0] - stage.pos[0]) >= 0.5f * kM);
    }
    // on the band's front line (the median depth: 20 ahead of the middle,
    // between the guitarists and the singer), within the grid's step
    CHECK(std::fabs(stage_depth - (20 + 0.5f * kM)) <= 0.25f * kM + 1e-3f);
    // venue: 2 m behind the crowd's last rows, raised 0.35 of the way from
    // the band (within 2.5 to 7 m) over the stage floor, facing the band
    const float venue_depth = r.centre[1] - venue.pos[1];
    CHECK(venue_depth == doctest::Approx(r.crowd_back + 2 * kM));
    const float rise = std::fmin(std::fmax(0.35f * (venue_depth - r.band_front), 2.5f * kM), 7 * kM);
    CHECK(venue.pos[2] == doctest::Approx(256 + rise));
    CHECK(std::fabs(venue.yaw) < 1e-4f);
    // a bigger viewer (vr_world_scale 2) stands as tall again, in the same place
    vr::Anchor tall;
    REQUIRE(vr::PresetAnchor(vr::ViewMode::kFrontRow, r, {}, 2 * kM, tall));
    CHECK(tall.pos[1] == doctest::Approx(front.pos[1]));
    CHECK(tall.pos[2] == doctest::Approx(200 + 3.4f * kM));
}

TEST_CASE("the viewpoints keep their place against the band in a venue moved and turned") {
    const Layout plain;
    Layout moved;
    moved.turn = 2.1f;
    moved.shift[0] = 3000;
    moved.shift[1] = -700;
    moved.shift[2] = -150;
    const vr::VenueRef a = vr::VenueRefOf(VenueFrame(plain));
    const vr::VenueRef b = vr::VenueRefOf(VenueFrame(moved));
    REQUIRE(b.members == 4);
    CHECK(b.toward_crowd);
    for (vr::ViewMode mode : {vr::ViewMode::kFrontRow, vr::ViewMode::kOnStage, vr::ViewMode::kVenue}) {
        CAPTURE(vr::ViewModeName(mode));
        vr::Anchor pa, pb;
        REQUIRE(vr::PresetAnchor(mode, a, {}, kM, pa));
        REQUIRE(vr::PresetAnchor(mode, b, {}, kM, pb));
        float back[3];
        Unplace(moved, pb.pos, back);
        // the grid's on_stage may pick the mirror place: |x| alike
        CHECK(std::fabs(std::fabs(back[0]) - std::fabs(pa.pos[0])) < 0.5f);
        CHECK(std::fabs(back[1] - pa.pos[1]) < 0.5f);
        CHECK(std::fabs(back[2] - pa.pos[2]) < 0.01f);
        float d = pb.yaw - pa.yaw - moved.turn;
        d = std::remainder(d, 2 * kPi);
        CHECK(std::fabs(d) < 1e-3f);
    }
}

TEST_CASE("without a crowd: the audience on -y, the barrier 5 m out, the floor under the stage") {
    Layout l;
    l.crowd = false;
    const vr::VenueRef r = vr::VenueRefOf(VenueFrame(l));
    REQUIRE(r.members == 4);
    CHECK_FALSE(r.toward_crowd);
    CHECK(r.toward[1] == -1);
    CHECK(r.crowd_ahead == 0);
    vr::Anchor front, venue;
    REQUIRE(vr::PresetAnchor(vr::ViewMode::kFrontRow, r, {}, kM, front));
    CHECK(r.centre[1] - front.pos[1] == doctest::Approx(r.band_front + 5 * kM));
    CHECK(front.pos[2] == doctest::Approx(256 - 1.2f * kM + 1.7f * kM));
    REQUIRE(vr::PresetAnchor(vr::ViewMode::kVenue, r, {}, kM, venue));
    CHECK(r.centre[1] - venue.pos[1] == doctest::Approx(r.band_front + 15 * kM));
    // one member is still a band
    l.members = 1;
    const vr::VenueRef one = vr::VenueRefOf(VenueFrame(l));
    CHECK(one.members == 1);
    vr::Anchor stage;
    REQUIRE(vr::PresetAnchor(vr::ViewMode::kOnStage, one, {}, kM, stage));
    CHECK(std::hypot(stage.pos[0] - one.member[0][0], stage.pos[1] - one.member[0][1]) >=
          0.9f * kM - 1e-3f);
}

TEST_CASE("vr_viewpoint_offset moves a viewpoint in its own frame") {
    vr::ViewOffset o;
    CHECK(vr::ParseViewOffset("1,-0.5,0.3", o));
    CHECK(o.right == 1);
    CHECK(o.ahead == -0.5f);
    CHECK(o.up == doctest::Approx(0.3f));
    CHECK(o.yaw_deg == 0);
    CHECK(vr::ParseViewOffset("0, 2, 0, 15", o));
    CHECK(o.ahead == 2);
    CHECK(o.yaw_deg == 15);
    CHECK_FALSE(vr::ParseViewOffset("1,2", o));
    CHECK_FALSE(vr::ParseViewOffset("1,2,3,4,5", o));
    CHECK_FALSE(vr::ParseViewOffset("a,b,c", o));
    const vr::VenueRef r = vr::VenueRefOf(VenueFrame(Layout{}));
    vr::Anchor plain, moved;
    REQUIRE(vr::PresetAnchor(vr::ViewMode::kFrontRow, r, {}, kM, plain));
    // front_row faces +y: its right is +x, ahead +y
    REQUIRE(vr::PresetAnchor(vr::ViewMode::kFrontRow, r, {1, 2, 0.5f, 90}, kM, moved));
    CHECK(moved.pos[0] - plain.pos[0] == doctest::Approx(kM));
    CHECK(moved.pos[1] - plain.pos[1] == doctest::Approx(2 * kM));
    CHECK(moved.pos[2] - plain.pos[2] == doctest::Approx(0.5f * kM));
    CHECK(moved.yaw - plain.yaw == doctest::Approx(kPi / 2));
    // on_stage faces -y: its right is -x
    vr::Anchor s0, s1;
    REQUIRE(vr::PresetAnchor(vr::ViewMode::kOnStage, r, {}, kM, s0));
    REQUIRE(vr::PresetAnchor(vr::ViewMode::kOnStage, r, {1, 0, 0, 0}, kM, s1));
    CHECK(s1.pos[0] - s0.pos[0] == doctest::Approx(-kM));
    float f[2];
    Facing(s0, f);
    CHECK(f[1] == doctest::Approx(-1).epsilon(1e-3));
}

TEST_CASE("a song's venue: kept, taken again for more of the band or a first crowd, forgotten after") {
    vr::VenueTracker t;
    Layout two;
    two.members = 2;
    two.crowd = false;
    CHECK_FALSE(t.Next(VenueFrame(two), false));
    CHECK_FALSE(t.Ref().Valid());
    CHECK(t.Next(VenueFrame(two), true));
    CHECK(t.Ref().members == 2);
    CHECK_FALSE(t.Next(VenueFrame(two), true));  // nothing more
    Layout crowd = two;
    crowd.crowd = true;
    CHECK(t.Next(VenueFrame(crowd), true));       // a first crowd
    CHECK(t.Ref().crowd > 0);
    CHECK_FALSE(t.Next(VenueFrame(crowd), true));
    Layout all;
    CHECK(t.Next(VenueFrame(all), true));          // the whole band
    CHECK(t.Ref().members == 4);
    CHECK_FALSE(t.Next(VenueFrame(two), true));    // fewer: kept
    CHECK(t.Ref().members == 4);
    // the next song, in another venue: found afresh
    CHECK_FALSE(t.Next(VenueFrame(all), false));
    Layout elsewhere;
    elsewhere.shift[0] = 5000;
    CHECK(t.Next(VenueFrame(elsewhere), true));
    CHECK(t.Ref().centre[0] == doctest::Approx(5000));
}

TEST_CASE("from a fixed viewpoint the crowd near the eye is left out, the rest and the band kept") {
    const Layout l;
    std::vector<uint32_t> crowd_meshes;
    const FrameCapture f = VenueFrame(l, &crowd_meshes);
    const vr::VenueRef r = vr::VenueRefOf(f);
    vr::Anchor a;
    REQUIRE(vr::PresetAnchor(vr::ViewMode::kFrontRow, r, {}, kM, a));
    vr::Eye e;
    e.pose = vr::EyePose(vr::XrPose{}, a);
    e.fov = {-0.9f, 0.9f, 0.9f, -0.9f};
    e.crowd_clear = vr::CrowdClear(vr::ViewMode::kFrontRow, 1.5f);
    CHECK(e.crowd_clear == doctest::Approx(1.5f * kM));
    FrameCapture out;
    const vr::RePoseStats st = vr::RePose(f, e, out);
    REQUIRE(st.ok);
    // the first row's cards within 1.5 m across the floor: the eye stands
    // 0.3 m before the row at x ~0, so columns -1..1 of the first row and
    // column 0 of the second
    uint32_t squashed = 0;
    for (size_t i = 0; i < out.draws.size(); i++) {
        const DrawItem& d = out.draws[i];
        const bool flat = d.world.m[0][0] == 0 && d.world.m[1][1] == 0 && d.world.m[2][2] == 0;
        bool is_crowd = false;
        for (uint32_t m : crowd_meshes) is_crowd |= d.mesh == m;
        if (flat) {
            CHECK(is_crowd);
            const float dx = d.world.m[3][0] - e.pose.eye[0], dy = d.world.m[3][1] - e.pose.eye[1];
            CHECK(std::hypot(dx, dy) < 1.5f * kM);
            // still where it stood
            CHECK(d.world.m[3][2] == f.draws[i].world.m[3][2]);
            squashed++;
        } else if (is_crowd) {
            const float dx = d.world.m[3][0] - e.pose.eye[0], dy = d.world.m[3][1] - e.pose.eye[1];
            CHECK(std::hypot(dx, dy) >= 1.5f * kM - 1e-2f);
        }
    }
    CHECK(squashed == st.crowd_cleared);
    CHECK(squashed == 4);
    CHECK(out.draws.size() == f.draws.size());
    // the game camera's window, or no clearing asked: nothing left out
    CHECK(vr::CrowdClear(vr::ViewMode::kGameCamera, 1.5f) == 0);
    e.crowd_clear = 0;
    CHECK(vr::RePose(f, e, out).crowd_cleared == 0);
}

TEST_CASE("a fixed viewpoint in a song: from the anchor, no window, clear of the crowd; menus the window") {
    const FrameCapture f = VenueFrame(Layout{});
    vr::FrameInputs in;
    for (int e = 0; e < 2; e++) {
        in.views[e].p[0] = e ? 0.032f : -0.032f;
        in.fov[e] = {-0.9f, 0.9f, 0.9f, -0.9f};
    }
    in.hud_boundary = vr::HudBoundary(f);
    REQUIRE(in.hud_boundary > 0);
    const WorldCamera wc = WorldCameraOf(f);
    in.camera = freecam::Decompose(wc.view_proj, in.game, in.proj);
    REQUIRE(in.camera);
    vr::Viewpoint vp;
    vp.mode = vr::ViewMode::kOnStage;
    vp.window = false;
    REQUIRE(vr::PresetAnchor(vp.mode, vr::VenueRefOf(f), {}, kM, vp.anchor));
    vp.crowd_clear = vr::CrowdClear(vp.mode, 1.5f);
    in.in_song = true;
    vr::FrameLayout l = vr::ResolveFrame(vp, in);
    CHECK_FALSE(l.window);
    CHECK(l.frame_quads == 0);
    CHECK_FALSE(l.anchor_follows);
    CHECK(l.anchor.pos[2] == vp.anchor.pos[2]);
    CHECK(l.eyes[0].crowd_clear == doctest::Approx(1.5f * kM));
    CHECK(l.eyes[0].pose.eye[2] == doctest::Approx(vp.anchor.pos[2]));
    in.in_song = false;
    l = vr::ResolveFrame(vp, in);
    CHECK(l.window);
    CHECK(l.eyes[0].crowd_clear == 0);
}
