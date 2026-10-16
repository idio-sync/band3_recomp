// Checks src/Render/gpu_view.h's pass hoisting (native_view_hoist_passes):
// which texture passes HoistablePass lets move, which targets PlanHoist gives
// a layer per version and which layers, and how it reorders a frame's runs:
// passes ahead of the world's first back-buffer run, in order, runs before it
// (Plan's stand-ins) left first, and never a move that changes what any run
// reads.

#include <doctest/doctest.h>
#include <cstdint>
#include <vector>
#include "src/Render/gpu_view.h"

using namespace band3::render;

namespace {

constexpr uint32_t kCrowd = 0x26734F88, kOutfit = 0x20BE58B0, kOther = 0x11110000;

HoistRun PassRunOf(uint32_t target, uint32_t version, bool movable = true) {
    HoistRun r;
    r.pass = true;
    r.movable = movable;
    r.target = target;
    r.version = version;
    return r;
}

HoistRun WorldRun(std::vector<HoistSample> samples = {}, bool before_post = true) {
    HoistRun r;
    r.world = before_post;
    r.samples = std::move(samples);
    return r;
}

HoistSample Diffuse(uint32_t target, uint32_t version, bool world = true) {
    return {target, version, true, world};
}

// WorldCrowd::DrawShowing's pattern: per type, its picture then its cards
std::vector<HoistRun> Crowd(uint32_t types) {
    std::vector<HoistRun> runs = {WorldRun()};
    for (uint32_t v = 1; v <= types; v++) {
        runs.push_back(PassRunOf(kCrowd, v));
        runs.push_back(WorldRun({Diffuse(kCrowd, v)}));
    }
    return runs;
}

}  // namespace

TEST_CASE("HoistablePass: cleared, before post-processing, not carried or special") {
    CHECK(HoistablePass(false, true, 0x3f, false, false));
    // without depth, a colour clear is enough
    CHECK(HoistablePass(false, true, 0x0f, true, false));
    CHECK_FALSE(HoistablePass(false, true, 0x0f, false, false));  // depth kept
    CHECK_FALSE(HoistablePass(false, true, 0x30, false, false));  // colour kept
    CHECK_FALSE(HoistablePass(false, true, 0x00, true, false));
    CHECK_FALSE(HoistablePass(true, true, 0x3f, false, false));   // carried
    CHECK_FALSE(HoistablePass(false, false, 0x3f, false, false)); // from post_boundary
    CHECK_FALSE(HoistablePass(false, true, 0x3f, false, true));   // shadow, spot, soft
}

TEST_CASE("the crowd's passes get a layer each, the last in layer 0, and move ahead") {
    const std::vector<HoistRun> runs = Crowd(4);
    const HoistPlan plan = PlanHoist(runs, true, true, 2048);
    REQUIRE(plan.layers.count(kCrowd));
    CHECK(plan.layers.at(kCrowd) == 4);
    // runs 1, 3, 5, 7 draw versions 1..4
    CHECK(plan.layer[1] == 1);
    CHECK(plan.layer[3] == 2);
    CHECK(plan.layer[5] == 3);
    CHECK(plan.layer[7] == 0);
    CHECK(plan.LayerOf(kCrowd, 1) == 1);
    CHECK(plan.LayerOf(kCrowd, 4) == 0);
    CHECK(plan.LayerOf(kOther, 1) == 0);
    // the passes go just before the world's first run, in their order
    CHECK(plan.order == std::vector<uint32_t>{1, 3, 5, 7, 0, 2, 4, 6, 8});
    CHECK(plan.passes_hoisted == 4);
    CHECK(plan.last_hoisted == 7);
}

TEST_CASE("without renaming, a target's later versions can't pass the draws reading earlier ones") {
    const std::vector<HoistRun> runs = Crowd(3);
    const HoistPlan plan = PlanHoist(runs, false, true, 2048);
    CHECK(plan.layers.empty());
    // the first version's pass moves; the second's target is read in between
    CHECK(plan.order == std::vector<uint32_t>{1, 0, 2, 3, 4, 5, 6});
    CHECK(plan.passes_hoisted == 1);
}

TEST_CASE("hoisting off: the runs as they are, the layers still given") {
    const std::vector<HoistRun> runs = Crowd(3);
    const HoistPlan plan = PlanHoist(runs, true, false, 2048);
    CHECK(plan.order == std::vector<uint32_t>{0, 1, 2, 3, 4, 5, 6});
    CHECK(plan.passes_hoisted == 0);
    CHECK(plan.last_hoisted == HoistPlan::kNone);
    CHECK(plan.layers.at(kCrowd) == 3);
}

TEST_CASE("runs before the world's first stay first: Plan's stand-ins, carried passes") {
    std::vector<HoistRun> runs = {PassRunOf(kOutfit, 7, false), PassRunOf(kOther, 1)};
    for (const HoistRun& r : Crowd(2)) runs.push_back(r);
    const HoistPlan plan = PlanHoist(runs, true, true, 2048);
    CHECK(plan.order == std::vector<uint32_t>{0, 1, 3, 5, 2, 4, 6});
    CHECK_FALSE(plan.hoisted[0]);
    CHECK_FALSE(plan.hoisted[1]);
}

TEST_CASE("a target that isn't renamed: a pass whose target the world reads before it stays") {
    // the world reads kOther's older content, then a pass redraws it
    const std::vector<HoistRun> runs = {WorldRun({Diffuse(kOther, 1)}), PassRunOf(kOther, 2),
                                        WorldRun({Diffuse(kOther, 2)})};
    const HoistPlan plan = PlanHoist(runs, true, true, 2048);
    CHECK(plan.order == std::vector<uint32_t>{0, 1, 2});
    CHECK(plan.passes_hoisted == 0);
}

TEST_CASE("a pass reading a target a pass left in place drew stays after it") {
    // run 1 can't move (a shadow map, say); run 3 reads what it made
    const std::vector<HoistRun> runs = {WorldRun(), PassRunOf(kOther, 5, false), WorldRun(),
                                        [] {
                                            HoistRun r = PassRunOf(kOutfit, 1);
                                            r.samples = {{kOther, 5, false, true}};
                                            return r;
                                        }(),
                                        WorldRun({Diffuse(kOutfit, 1)})};
    const HoistPlan plan = PlanHoist(runs, true, true, 2048);
    CHECK(plan.order == std::vector<uint32_t>{0, 1, 2, 3, 4});
    CHECK(plan.passes_hoisted == 0);
}

TEST_CASE("a pass reading what a moved pass made moves after it") {
    HoistRun reader = PassRunOf(kOutfit, 1);
    reader.samples = {Diffuse(kOther, 1)};
    const std::vector<HoistRun> runs = {WorldRun(), PassRunOf(kOther, 1), WorldRun(), reader,
                                        WorldRun({Diffuse(kOutfit, 1)})};
    const HoistPlan plan = PlanHoist(runs, true, true, 2048);
    CHECK(plan.order == std::vector<uint32_t>{1, 3, 0, 2, 4});
}

TEST_CASE("renaming needs every read a diffuse one, before post, of a version made before it") {
    SUBCASE("read as a normal map") {
        std::vector<HoistRun> runs = Crowd(2);
        runs[2].samples[0].diffuse = false;
        CHECK(PlanHoist(runs, true, true, 2048).layers.empty());
    }
    SUBCASE("read in the overlay") {
        std::vector<HoistRun> runs = Crowd(2);
        runs.push_back(WorldRun({Diffuse(kCrowd, 2, false)}, false));
        CHECK(PlanHoist(runs, true, true, 2048).layers.empty());
    }
    SUBCASE("a version no pass makes this frame") {
        std::vector<HoistRun> runs = Crowd(2);
        runs[0].samples = {Diffuse(kCrowd, 0)};
        CHECK(PlanHoist(runs, true, true, 2048).layers.empty());
    }
    SUBCASE("a version made after the read") {
        std::vector<HoistRun> runs = Crowd(2);
        runs[2].samples = {Diffuse(kCrowd, 2)};
        CHECK(PlanHoist(runs, true, true, 2048).layers.empty());
    }
    SUBCASE("a pass that can't move") {
        std::vector<HoistRun> runs = Crowd(2);
        runs[3].movable = false;
        CHECK(PlanHoist(runs, true, true, 2048).layers.empty());
    }
    SUBCASE("two passes making the same version") {
        std::vector<HoistRun> runs = Crowd(2);
        runs[3].version = 1;
        CHECK(PlanHoist(runs, true, true, 2048).layers.empty());
    }
    SUBCASE("more versions than layers") {
        CHECK(PlanHoist(Crowd(3), true, true, 2).layers.empty());
        CHECK(PlanHoist(Crowd(2), true, true, 2).layers.at(kCrowd) == 2);
    }
    SUBCASE("one version: one layer, as ever") {
        CHECK(PlanHoist(Crowd(1), true, true, 2048).layers.empty());
    }
}

TEST_CASE("a renamed target's passes move even with its earlier versions read in between") {
    // and a pass that stays keeps its place among the world's runs
    std::vector<HoistRun> runs = Crowd(2);
    runs.insert(runs.begin() + 2, PassRunOf(kOther, 1, false));
    // 0 world, 1 crowd v1, 2 other (stays), 3 world (v1), 4 crowd v2, 5 world (v2)
    const HoistPlan plan = PlanHoist(runs, true, true, 2048);
    CHECK(plan.order == std::vector<uint32_t>{1, 4, 0, 2, 3, 5});
    CHECK(plan.layer[1] == 1);
    CHECK(plan.layer[4] == 0);
}

TEST_CASE("no world run: nothing moves") {
    const std::vector<HoistRun> runs = {PassRunOf(kOther, 1), PassRunOf(kOutfit, 1)};
    const HoistPlan plan = PlanHoist(runs, true, true, 2048);
    CHECK(plan.order == std::vector<uint32_t>{0, 1});
    CHECK(plan.passes_hoisted == 0);
}
