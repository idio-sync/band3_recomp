// Checks src/Render/gpu_view.h's instancing (native_view_instancing): which
// draws SameInstanceState counts as differing only in world, and how
// InstanceRuns folds runs of them into their first draw as the plan walk
// meets them: only the very next index, never across a PassRun's start (the
// walk's Start) or into a draw the walk calls ineligible (past post_boundary,
// a cone, a soft particle).

#include <doctest/doctest.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include "src/Render/gpu_view.h"

using namespace band3::render;

namespace {

Mat4 Translate(float x) {
    Mat4 m{};
    for (int i = 0; i < 4; i++) m.m[i][i] = 1.0f;
    m.m[3][0] = x;
    return m;
}

// a multimesh instance as CaptureMultiMesh makes it: the prototype with its
// own world
DrawItem Card(const std::shared_ptr<const Geometry>& geom, float x) {
    DrawItem d{};
    d.geom = geom;
    d.world = Translate(x);
    d.view_proj = Translate(0);
    for (float& c : d.color) c = 1.0f;
    d.blend = 1;
    d.z_mode = 1;
    d.cam = 7;
    d.mesh = 0x235E2C40;
    d.shade = 3;
    return d;
}

// each draw in turn through a run started fresh: the head each joined (kNoHead
// for a draw of its own)
std::vector<uint32_t> Walk(const std::vector<DrawItem>& draws,
                           const std::vector<bool>& eligible = {}) {
    InstanceRuns runs;
    runs.Start();
    std::vector<uint32_t> heads;
    for (uint32_t d = 0; d < draws.size(); d++)
        heads.push_back(runs.Add(draws, d, eligible.empty() || eligible[d]));
    return heads;
}

constexpr uint32_t kNone = InstanceRuns::kNoHead;

}  // namespace

TEST_CASE("two draws differing only in world are one state, and the second joins the first") {
    auto geom = std::make_shared<Geometry>();
    const std::vector<DrawItem> draws = {Card(geom, 0), Card(geom, 5)};
    CHECK(SameInstanceState(draws[0], draws[1]));
    CHECK(Walk(draws) == std::vector<uint32_t>{kNone, 0});
}

TEST_CASE("a run of a multimesh's instances folds into its first") {
    auto geom = std::make_shared<Geometry>();
    std::vector<DrawItem> draws;
    for (int i = 0; i < 5; i++) draws.push_back(Card(geom, float(i)));
    CHECK(Walk(draws) == std::vector<uint32_t>{kNone, 0, 0, 0, 0});
}

TEST_CASE("each field but world breaks a run") {
    auto geom = std::make_shared<Geometry>();
    auto other_geom = std::make_shared<Geometry>();
    auto tex = std::make_shared<Texture>();
    const std::vector<std::pair<const char*, std::function<void(DrawItem&)>>> changes = {
        {"geom", [&](DrawItem& d) { d.geom = other_geom; }},
        {"tex", [&](DrawItem& d) { d.tex = tex; }},
        {"shade", [](DrawItem& d) { d.shade = 4; }},
        {"target", [](DrawItem& d) { d.target = 0x1234; }},
        {"cam", [](DrawItem& d) { d.cam = 8; }},
        {"blend", [](DrawItem& d) { d.blend = 2; }},
        {"z_mode", [](DrawItem& d) { d.z_mode = 2; }},
        {"prelit", [](DrawItem& d) { d.prelit = true; }},
        {"alpha_cut", [](DrawItem& d) { d.alpha_cut = true; }},
        {"alpha_threshold", [](DrawItem& d) { d.alpha_threshold = 128; }},
        {"cull", [](DrawItem& d) { d.cull = kCullBack; }},
        {"draw_mode", [](DrawItem& d) { d.draw_mode = kDrawModeShadowDepth; }},
        {"rect_shader", [](DrawItem& d) { d.rect_shader = 6; }},
        {"mip_level", [](DrawItem& d) { d.mip_level = 1; }},
        {"bones", [](DrawItem& d) { d.bones.push_back(Translate(1)); }},
        {"view_proj", [](DrawItem& d) { d.view_proj.m[2][3] = 0.5f; }},
        {"color", [](DrawItem& d) { d.color[3] = 0.5f; }},
    };
    for (const auto& [name, change] : changes) {
        CAPTURE(name);
        std::vector<DrawItem> draws = {Card(geom, 0), Card(geom, 1)};
        change(draws[1]);
        CHECK_FALSE(SameInstanceState(draws[0], draws[1]));
        CHECK_FALSE(SameInstanceState(draws[1], draws[0]));
        CHECK(Walk(draws) == std::vector<uint32_t>{kNone, kNone});
        // and a third like the second starts its own run from it
        draws.push_back(draws[1]);
        draws[2].world = Translate(2);
        const bool joins = SameInstanceState(draws[1], draws[2]);
        CHECK(Walk(draws) == std::vector<uint32_t>{kNone, kNone, joins ? 1u : kNone});
    }
}

TEST_CASE("skinned draws, DrawRect quads and mip downsamples are never instanced") {
    auto geom = std::make_shared<Geometry>();
    DrawItem a = Card(geom, 0), b = Card(geom, 1);
    a.bones = b.bones = {Translate(0)};
    CHECK_FALSE(SameInstanceState(a, b));
    a = Card(geom, 0);
    b = Card(geom, 1);
    a.rect_shader = b.rect_shader = 6;
    CHECK_FALSE(SameInstanceState(a, b));
    a = Card(geom, 0);
    b = Card(geom, 1);
    a.mip_level = b.mip_level = 2;
    CHECK_FALSE(SameInstanceState(a, b));
    // no geometry: nothing to draw instanced
    a = Card(nullptr, 0);
    b = Card(nullptr, 1);
    CHECK_FALSE(SameInstanceState(a, b));
}

TEST_CASE("a gap in the indices breaks a run") {
    auto geom = std::make_shared<Geometry>();
    std::vector<DrawItem> draws;
    for (int i = 0; i < 4; i++) draws.push_back(Card(geom, float(i)));
    // the walk left out draw 1 (another pass's, not drawn): 2 doesn't join 0
    InstanceRuns runs;
    runs.Start();
    CHECK(runs.Add(draws, 0, true) == kNone);
    CHECK(runs.Add(draws, 2, true) == kNone);
    CHECK(runs.Add(draws, 3, true) == 2);
}

TEST_CASE("an ineligible draw closes the run and doesn't start one") {
    auto geom = std::make_shared<Geometry>();
    std::vector<DrawItem> draws;
    for (int i = 0; i < 5; i++) draws.push_back(Card(geom, float(i)));
    // draw 2 a cone or soft particle: 3 can't join 0's run through it
    CHECK(Walk(draws, {true, true, false, true, true}) ==
          std::vector<uint32_t>{kNone, 0, kNone, kNone, 3});
}

TEST_CASE("runs never cross post_boundary") {
    auto geom = std::make_shared<Geometry>();
    std::vector<DrawItem> draws;
    for (int i = 0; i < 6; i++) draws.push_back(Card(geom, float(i)));
    // the walk's eligibility for a back-buffer run: before post_boundary only
    const uint32_t post_boundary = 3;
    std::vector<bool> eligible;
    for (uint32_t d = 0; d < draws.size(); d++) eligible.push_back(d < post_boundary);
    CHECK(Walk(draws, eligible) == std::vector<uint32_t>{kNone, 0, 0, kNone, kNone, kNone});
}

TEST_CASE("runs never cross a PassRun's end") {
    auto geom = std::make_shared<Geometry>();
    std::vector<DrawItem> draws;
    for (int i = 0; i < 4; i++) draws.push_back(Card(geom, float(i)));
    // PassRuns {0, 2} and {2, 4}, the same state throughout and no index gap
    InstanceRuns runs;
    runs.Start();
    CHECK(runs.Add(draws, 0, true) == kNone);
    CHECK(runs.Add(draws, 1, true) == 0);
    runs.Start();
    CHECK(runs.Add(draws, 2, true) == kNone);
    CHECK(runs.Add(draws, 3, true) == 2);
}

TEST_CASE("a pass walked twice gives the same runs again") {
    // PlanPasses can draw a pass twice (an earlier draw samples a version a
    // pass makes later): the walk sees its draws again from Start
    auto geom = std::make_shared<Geometry>();
    std::vector<DrawItem> draws;
    for (int i = 0; i < 3; i++) draws.push_back(Card(geom, float(i)));
    InstanceRuns runs;
    for (int pass = 0; pass < 2; pass++) {
        runs.Start();
        CHECK(runs.Add(draws, 0, true) == kNone);
        CHECK(runs.Add(draws, 1, true) == 0);
        CHECK(runs.Add(draws, 2, true) == 0);
    }
}

TEST_CASE("PackInstancing: on with more than one instance, the first world's index") {
    uint32_t u[4] = {9, 9, 9, 9};
    PackInstancing(1, 40, u);
    CHECK(u[0] == 0);
    CHECK(u[1] == 0);
    PackInstancing(12, 40, u);
    CHECK(u[0] == 1);
    CHECK(u[1] == 40);
    // SV_InstanceID's low bits left for a stereo eye: none yet
    CHECK(u[2] == 0);
    CHECK(u[3] == 0);
}
