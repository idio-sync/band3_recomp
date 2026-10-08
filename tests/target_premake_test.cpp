// Checks src/Render/target_premake.h, the render targets the native renderer
// makes ahead as RB3 loads its rendered textures: the queue keeps one
// announcement per DxTex in order and forgets a destroyed one; the worker
// makes a few between frames and stops at its cap; only targets without
// depth share one, never a shadow map; levels follow FinishDrawTarget's
// chain; loaded textures' kinds are announced once each, for their arrays.

#include <doctest/doctest.h>
#include <cstdint>
#include "src/Render/target_premake.h"

using namespace band3::render;

namespace {

AnnouncedTarget Tex(uint32_t obj, uint32_t w = 256, uint32_t h = 256, uint32_t type = 2) {
    AnnouncedTarget t;
    t.tex_obj = obj;
    t.width = w;
    t.height = h;
    t.tex_type = type;
    t.num_mips = 1;
    return t;
}

}  // namespace

TEST_CASE("an announced target replaces its DxTex's earlier one, at the back") {
    AnnounceQueue q;
    q.Announce(Tex(1));
    q.Announce(Tex(2));
    q.Announce(Tex(1, 512, 512));
    REQUIRE(q.Size() == 2);
    CHECK(q.Front().tex_obj == 2);
    q.PopFront();
    CHECK(q.Front().tex_obj == 1);
    CHECK(q.Front().width == 512);
}

TEST_CASE("a destroyed DxTex's announcement is forgotten") {
    AnnounceQueue q;
    q.Announce(Tex(1));
    q.Announce(Tex(2));
    q.Forget(1);
    REQUIRE(q.Size() == 1);
    CHECK(q.Front().tex_obj == 2);
    q.Forget(7);  // never announced
    CHECK(q.Size() == 1);
}

TEST_CASE("the queue holds at most kMax, dropping the oldest") {
    AnnounceQueue q;
    for (uint32_t i = 1; i <= AnnounceQueue::kMax + 3; i++) q.Announce(Tex(i));
    CHECK(q.Size() == AnnounceQueue::kMax);
    CHECK(q.Front().tex_obj == 4);
}

TEST_CASE("taking announcements moves them into the worker's queue in order") {
    AnnounceQueue shared, worker;
    worker.Announce(Tex(1));
    worker.Announce(Tex(2));
    shared.Announce(Tex(3));
    shared.Announce(Tex(1, 128, 128));
    shared.TakeInto(worker);
    CHECK(shared.Empty());
    REQUIRE(worker.Size() == 3);
    CHECK(worker.Front().tex_obj == 2);
    worker.PopFront();
    CHECK(worker.Front().tex_obj == 3);
    worker.PopFront();
    CHECK(worker.Front().tex_obj == 1);
    CHECK(worker.Front().width == 128);
}

TEST_CASE("only rendered textures of a drawable size are announced") {
    CHECK(AnnouncesTarget(2, 256, 256));
    CHECK(AnnouncesTarget(kTexTypeShadowMap, 512, 512));
    CHECK(AnnouncesTarget(kTexTypeDepthVolume, 640, 360));
    CHECK_FALSE(AnnouncesTarget(0, 256, 256));  // a plain texture
    CHECK_FALSE(AnnouncesTarget(1, 256, 256));
    CHECK_FALSE(AnnouncesTarget(2, 0, 256));
    CHECK_FALSE(AnnouncesTarget(2, 256, 16384));
}

TEST_CASE("the worker makes a few between frames, within time, under its cap") {
    CHECK(PremakeMore(0, 0.0, 0));
    CHECK(PremakeMore(PremakeBudget::kPerIdle - 1, 0.0, 0));
    CHECK_FALSE(PremakeMore(PremakeBudget::kPerIdle, 0.0, 0));
    CHECK_FALSE(PremakeMore(1, PremakeBudget::kIdleMs, 0));
    CHECK(PremakeMore(1, PremakeBudget::kIdleMs - 0.5, PremakeBudget::kMaxUnused - 1));
    CHECK_FALSE(PremakeMore(0, 0.0, PremakeBudget::kMaxUnused));
}

TEST_CASE("only targets without depth share one, never a shadow map") {
    CHECK(SharesDepth(kTexTypeNoZ | 2, false));
    CHECK_FALSE(SharesDepth(2, false));  // loads its depth after a blur
    CHECK_FALSE(SharesDepth(kTexTypeShadowMap, true));
    CHECK_FALSE(SharesDepth(kTexTypeNoZ | 2, true));
    // the spotlights' targets load depth or not by their own type bits
    CHECK(SharesDepth(kTexTypeDepthVolume, false) == ((kTexTypeDepthVolume & kTexTypeNoZ) != 0));
    CHECK(SharesDepth(kTexTypeDensityMap, false) == ((kTexTypeDensityMap & kTexTypeNoZ) != 0));
}

TEST_CASE("a target's levels follow FinishDrawTarget's chain, one for a shadow map") {
    CHECK(TargetLevels(1, 256, 512, false) == 1);
    CHECK(TargetLevels(4, 256, 512, false) == 4);
    CHECK(TargetLevels(20, 256, 512, false) == 10);  // 512 down to 1
    CHECK(TargetLevels(20, 1, 1, false) == 1);
    CHECK(TargetLevels(4, 512, 512, true) == 1);
}

TEST_CASE("loaded textures are announced once per size and format, oldest first") {
    TextureAnnounceQueue shared, worker;
    shared.Announce({256, 256, 18});
    shared.Announce({128, 128, 20});
    shared.Announce({256, 256, 18});  // another texture of the same kind
    shared.Announce({256, 256, 20});
    CHECK(shared.Size() == 3);
    worker.Announce({128, 128, 20});
    shared.TakeInto(worker);
    CHECK(shared.Empty());
    REQUIRE(worker.Size() == 3);
    CHECK(worker.Front() == AnnouncedTexture{128, 128, 20});
    worker.PopFront();
    CHECK(worker.Front() == AnnouncedTexture{256, 256, 18});
    for (uint32_t i = 1; i <= TextureAnnounceQueue::kMax + 2; i++) worker.Announce({i, i, 6});
    CHECK(worker.Size() == TextureAnnounceQueue::kMax);
}

TEST_CASE("loaded textures announce their arrays, rendered ones their targets") {
    CHECK(AnnouncesTexture(0, 256, 256));
    CHECK(AnnouncesTexture(1, 8, 8));
    CHECK_FALSE(AnnouncesTexture(2, 256, 256));
    CHECK_FALSE(AnnouncesTexture(kTexTypeShadowMap, 512, 512));
    CHECK_FALSE(AnnouncesTexture(0, 0, 256));
    CHECK_FALSE(AnnouncesTexture(0, 256, 9000));
}
