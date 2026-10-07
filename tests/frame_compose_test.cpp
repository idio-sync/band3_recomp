// Checks src/Render/frame_compose.cpp: with even/odd rendering a post frame's
// capture gets the world frame's world (its draws before post-processing,
// in their passes) in front of its own overlay, keeps the texture passes its
// overlay samples, leaves out what the world already has and its own draws
// before post-processing, and counts render targets again, is cleared as the
// world frame was and has both frames' cameras; and which frames a render
// check pairs with the game's picture.

#include <doctest/doctest.h>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>
#include "src/Render/frame_compose.h"

using namespace band3::render;

namespace {

constexpr uint32_t kImpostor = 0x20D00000, kPost = 0x20E00000, kComposite = 0x20EF6128,
                   kDof = 0x20F00000, kGone = 0x21000000;

std::shared_ptr<const Texture> Target(uint32_t tex_obj, uint32_t version) {
    auto t = std::make_shared<Texture>();
    t->tex_obj = tex_obj;
    t->tex_type = 0x2;
    t->version = version;
    return t;
}

// a draw, `mesh` to tell it by, into `target` (0 the back buffer)
DrawItem Draw(uint32_t mesh, uint32_t target = 0, int32_t shade = -1,
              std::shared_ptr<const Texture> tex = nullptr) {
    DrawItem d{};
    d.mesh = mesh;
    d.target = target;
    d.shade = shade;
    d.tex = std::move(tex);
    return d;
}

Pass MakePass(uint32_t tex_obj, uint32_t first, uint32_t count, uint32_t version,
              uint64_t from_frame) {
    Pass p;
    p.tex_obj = tex_obj;
    p.first_draw = first;
    p.draw_count = count;
    p.version = version;
    p.width = p.height = tex_obj ? 256 : 0;
    p.from_frame = from_frame;
    return p;
}

ShadeState Shade(int32_t type) {
    ShadeState s;
    std::memset(static_cast<ShadeInputs*>(&s), 0, sizeof(ShadeInputs));
    s.shader_type = type;
    return s;
}

// a world frame (proc_cmds 1): the crowd's impostor, the venue sampling it,
// then its overlay and a post-processing pass
FrameCapture WorldFrame() {
    FrameCapture w;
    w.frame = 20;
    w.game_frame = 100;
    w.world_frame = 100;
    w.proc_cmds = kProcWorld;
    w.draws = {Draw(1, kImpostor), Draw(2, 0, 0, Target(kImpostor, 3)), Draw(3, 0, 1),
               Draw(4, 0, 1), Draw(5, kPost)};
    w.passes = {MakePass(kImpostor, 0, 1, 3, 100), MakePass(0, 1, 3, 0, 100),
                MakePass(kPost, 4, 1, 7, 100)};
    w.post_boundary = 3;
    w.shades = {Shade(18), Shade(12)};
    w.skipped_shadow = 5;
    w.passes_own = 2;
    return w;
}

// the post frame after it (proc_cmds 2): a composite carried in for its
// overlay, the impostor carried in too, a stray back-buffer draw before
// post-processing, its overlay (one draw sampling a render target nothing
// made), and depth of field
FrameCapture PostFrame() {
    FrameCapture p;
    p.frame = 21;
    p.game_frame = 101;
    p.world_frame = 101;
    p.proc_cmds = kProcPost;
    p.draws = {Draw(10, kComposite), Draw(11, kImpostor), Draw(12),
               Draw(13, 0, -1, Target(kGone, 2)), Draw(14, 0, 0, Target(kComposite, 1)),
               Draw(15, kDof)};
    p.passes = {MakePass(kComposite, 0, 1, 1, 50), MakePass(kImpostor, 1, 1, 3, 100),
                MakePass(0, 2, 3, 0, 101), MakePass(kDof, 5, 1, 4, 101)};
    p.post_boundary = 3;
    p.shades = {Shade(14)};
    p.skipped_shadow = 2;
    p.skipped_velocity = 9;
    return p;
}

std::vector<uint32_t> Meshes(const FrameCapture& fc) {
    std::vector<uint32_t> out;
    for (const DrawItem& d : fc.draws) out.push_back(d.mesh);
    return out;
}

}  // namespace

TEST_CASE("a post frame's capture gets the world frame's world in front of its overlay") {
    const FrameCapture world = WorldFrame();
    const FrameCapture post = PostFrame();
    auto fc = ComposeFrame(world, post);
    REQUIRE(fc);

    // the world's draws before its post-processing, the composite's pass, then
    // the post frame's from its post-processing on
    CHECK(Meshes(*fc) == std::vector<uint32_t>{1, 2, 3, 10, 13, 14, 15});
    CHECK(fc->post_boundary == 4);
    // the world's draws come first, the post frame's carried pass after them
    // (native_world_ahead leaves out [0, composed_world_end))
    CHECK(fc->composed_world_end == 3);
    CHECK(world.composed_world_end == 0);
    REQUIRE(fc->passes.size() == 5);
    CHECK(fc->passes[0].tex_obj == kImpostor);
    CHECK(fc->passes[0].first_draw == 0);
    CHECK(fc->passes[0].draw_count == 1);
    // the back-buffer pass that ran on past the world's post-processing is cut
    CHECK(fc->passes[1].tex_obj == 0);
    CHECK(fc->passes[1].first_draw == 1);
    CHECK(fc->passes[1].draw_count == 2);
    CHECK(fc->passes[2].tex_obj == kComposite);
    CHECK(fc->passes[2].first_draw == 3);
    CHECK(fc->passes[3].tex_obj == 0);
    CHECK(fc->passes[3].first_draw == 4);
    CHECK(fc->passes[3].draw_count == 2);
    CHECK(fc->passes[3].from_frame == 101);
    CHECK(fc->passes[4].tex_obj == kDof);
    CHECK(fc->passes[4].first_draw == 6);

    // shades: the world's, then the post frame's, its draws pointing at theirs
    REQUIRE(fc->shades.size() == 3);
    CHECK(fc->shades[2].shader_type == 14);
    CHECK(fc->draws[1].shade == 0);
    CHECK(fc->draws[2].shade == 1);
    CHECK(fc->draws[4].shade == -1);
    CHECK(fc->draws[5].shade == 2);

    CHECK(fc->frame == 21);
    CHECK(fc->game_frame == 101);
    CHECK(fc->proc_cmds == kProcPost);
    CHECK(fc->composed == 1);
    CHECK(fc->world_frame == 100);
    CHECK(fc->skipped_shadow == 7);
    CHECK(fc->skipped_velocity == 9);
    // the impostor and depth of field drawn in the two frames, the composite
    // from before
    CHECK(fc->passes_own == 2);
    CHECK(fc->passes_carried == 1);
    // the impostor and the composite are there, the one nothing made isn't
    CHECK(fc->rt_sampled == 3);
    CHECK(fc->rt_missing == 1);

    // the frames it's made from are as they were
    CHECK(world.draws.size() == 5);
    CHECK(post.draws[4].shade == 0);
}

TEST_CASE("a texture pass that only cleared is kept where it was when composing") {
    // the post frame's depth volume cleared with no cone after its overlay's
    // draws, and again after its last draw
    constexpr uint32_t kVolume = 0x241B4508;
    const FrameCapture world = WorldFrame();
    FrameCapture post = PostFrame();
    post.passes = {MakePass(kComposite, 0, 1, 1, 50), MakePass(kImpostor, 1, 1, 3, 100),
                   MakePass(0, 2, 3, 0, 101),         MakePass(kVolume, 5, 0, 11, 101),
                   MakePass(kDof, 5, 1, 4, 101),      MakePass(kVolume, 6, 0, 12, 101)};
    auto fc = ComposeFrame(world, post);
    REQUIRE(fc);
    CHECK(Meshes(*fc) == std::vector<uint32_t>{1, 2, 3, 10, 13, 14, 15});
    REQUIRE(fc->passes.size() == 7);
    CHECK(fc->passes[4].tex_obj == kVolume);
    CHECK(fc->passes[4].version == 11);
    CHECK(fc->passes[4].first_draw == 6);
    CHECK(fc->passes[4].draw_count == 0);
    CHECK(fc->passes[5].tex_obj == kDof);
    CHECK(fc->passes[5].first_draw == 6);
    CHECK(fc->passes[6].tex_obj == kVolume);
    CHECK(fc->passes[6].version == 12);
    CHECK(fc->passes[6].first_draw == 7);
    CHECK(fc->passes[6].draw_count == 0);
    CHECK(fc->passes_own == 4);

    // one before the post frame's post-processing goes with its texture
    // passes from before (after the composite), unless the world has it
    post.passes[3].first_draw = 2;
    post.passes.erase(post.passes.begin() + 5);
    std::swap(post.passes[2], post.passes[3]);
    fc = ComposeFrame(world, post);
    REQUIRE(fc);
    REQUIRE(fc->passes.size() == 6);
    CHECK(fc->passes[2].tex_obj == kComposite);
    CHECK(fc->passes[3].tex_obj == kVolume);
    CHECK(fc->passes[3].first_draw == 4);
    CHECK(fc->passes[3].draw_count == 0);
    CHECK(fc->passes[4].tex_obj == 0);
    FrameCapture world_has = WorldFrame();
    world_has.passes.insert(world_has.passes.begin() + 1, MakePass(kVolume, 1, 0, 11, 100));
    fc = ComposeFrame(world_has, post);
    REQUIRE(fc);
    REQUIRE(fc->passes.size() == 6);
    CHECK(fc->passes[1].tex_obj == kVolume);
    for (size_t i = 2; i < fc->passes.size(); i++) CHECK(fc->passes[i].tex_obj != kVolume);
}

TEST_CASE("render targets sampled are counted by version") {
    FrameCapture fc;
    fc.draws = {Draw(1, kImpostor), Draw(2, 0, -1, Target(kImpostor, 3)),
                Draw(3, 0, -1, Target(kImpostor, 3)), Draw(4, 0, -1, Target(kImpostor, 4))};
    fc.passes = {MakePass(kImpostor, 0, 1, 3, 1), MakePass(0, 1, 3, 0, 1)};
    uint32_t sampled = 0, missing = 0;
    std::vector<uint64_t> filtered;
    CountRenderTargets(fc, {}, sampled, missing, filtered);
    CHECK(sampled == 2);
    CHECK(missing == 1);
    CHECK(filtered.empty());
}

TEST_CASE("a render target whose pass's draws were all left out isn't counted missing") {
    FrameCapture fc;
    fc.draws = {Draw(1, 0, -1, Target(kGone, 2)), Draw(2, 0, -1, Target(kGone, 2)),
                Draw(3, 0, -1, Target(kDof, 5))};
    fc.passes = {MakePass(0, 0, 3, 0, 1)};
    uint32_t sampled = 0, missing = 0;
    std::vector<uint64_t> filtered;
    // kGone's version 2 was a shadow map's pass; another version of it isn't
    const std::vector<uint64_t> left_out = {uint64_t(kGone) << 32 | 2, uint64_t(kGone) << 32 | 1};
    CountRenderTargets(fc, left_out, sampled, missing, filtered);
    CHECK(sampled == 2);
    CHECK(missing == 1);
    CHECK(filtered == std::vector<uint64_t>{uint64_t(kGone) << 32 | 2});
}

TEST_CASE("a composed frame counts the targets either frame left out as filtered") {
    FrameCapture world = WorldFrame();
    FrameCapture post = PostFrame();
    // the overlay's target nothing made was a pass whose draws were all left
    // out in the post frame
    post.rt_filtered_keys = {uint64_t(kGone) << 32 | 2};
    post.rt_filtered = 1;
    // and the world frame left one out that the composed frame doesn't sample
    world.rt_filtered_keys = {uint64_t(kDof) << 32 | 9};
    world.rt_filtered = 1;
    auto fc = ComposeFrame(world, post);
    REQUIRE(fc);
    CHECK(fc->rt_sampled == 3);
    CHECK(fc->rt_missing == 0);
    CHECK(fc->rt_filtered == 1);
    CHECK(fc->rt_filtered_keys == std::vector<uint64_t>{uint64_t(kGone) << 32 | 2});
}

TEST_CASE("a render check pairs the game's picture with a frame that shows its world") {
    FrameCapture fc;
    // a frame that didn't run DoPostProcess doesn't say
    CHECK_FALSE(ProcKnown(fc));
    CHECK(DrawsWorld(fc));

    fc.post_boundary = 0;
    fc.proc_cmds = 7;  // even/odd rendering off: every frame
    CHECK(ProcKnown(fc));
    CHECK(DrawsWorld(fc));
    CHECK(PresentsCapturedWorld(fc));

    fc.proc_cmds = kProcWorld;  // presents the world before it
    CHECK(DrawsWorld(fc));
    CHECK_FALSE(PresentsCapturedWorld(fc));

    fc.proc_cmds = kProcPost;  // presents the world before it, which it lacks
    CHECK_FALSE(DrawsWorld(fc));
    CHECK_FALSE(PresentsCapturedWorld(fc));
    fc.composed = 1;  // until composed with it
    CHECK(PresentsCapturedWorld(fc));

    fc.proc_cmds = 0;  // neither, emulating a lower rate
    CHECK_FALSE(DrawsWorld(fc));
    CHECK_FALSE(PresentsCapturedWorld(fc));
}

TEST_CASE("the native renderer starts the window's picture from a whole one") {
    FrameCapture fc;
    // a frame that doesn't say, begun with capture on
    CHECK(fc.whole);
    CHECK(StartsPicture(fc));
    // the first frame after capture turned on has only part of its draws
    fc.whole = 0;
    CHECK_FALSE(StartsPicture(fc));
    fc.whole = 1;
    fc.post_boundary = 0;
    fc.proc_cmds = 7;
    CHECK(StartsPicture(fc));
    // even/odd rendering: a world frame's picture is a post buffer, and a
    // post frame needs the world before it
    fc.proc_cmds = kProcWorld;
    CHECK_FALSE(StartsPicture(fc));
    fc.proc_cmds = kProcPost;
    CHECK_FALSE(StartsPicture(fc));
    fc.composed = 1;
    CHECK(StartsPicture(fc));
    fc.proc_cmds = 0;
    fc.composed = 0;
    CHECK_FALSE(StartsPicture(fc));
}

TEST_CASE("the native renderer's numbers tell frames by what they drew") {
    FrameCapture fc;
    CHECK(KindOf(fc) == FrameKind::kFull);  // doesn't say
    fc.post_boundary = 0;
    fc.proc_cmds = 7;
    CHECK(KindOf(fc) == FrameKind::kFull);
    fc.proc_cmds = kProcWorld;
    CHECK(KindOf(fc) == FrameKind::kWorld);
    fc.proc_cmds = kProcPost;
    CHECK(KindOf(fc) == FrameKind::kPost);
    fc.composed = 1;  // composed or not
    CHECK(KindOf(fc) == FrameKind::kPost);
    fc.proc_cmds = 0;
    CHECK(KindOf(fc) == FrameKind::kBetween);
    CHECK(std::strcmp(FrameKindName(FrameKind::kBetween), "between") == 0);
}

TEST_CASE("a composed frame has its post frame's post-processing and gamma ramp") {
    FrameCapture world = WorldFrame();
    FrameCapture post = PostFrame();
    world.post.valid = post.post.valid = 1;
    post.cost.draws = 12;
    world.post.saturation = -10;
    post.post.saturation = -80;
    post.post_consts.valid = 1;
    post.post_consts.c24[0] = 4.0f;
    post.gamma.mode = GammaRamp::kTable;
    post.gamma.table[255] = 0x3FFFFFFF;
    auto fc = ComposeFrame(world, post);
    REQUIRE(fc);
    CHECK(fc->post.saturation == -80.0f);
    CHECK(fc->post_consts.valid == 1);
    CHECK(fc->post_consts.c24[0] == 4.0f);
    CHECK(fc->gamma == post.gamma);
    // and what capturing it cost the game's thread
    CHECK(fc->cost.draws == 12);
}

TEST_CASE("a composed frame is cleared as its world frame was, with both frames' cameras") {
    FrameCapture world = WorldFrame();
    FrameCapture post = PostFrame();
    world.has_clear_color = 1;
    world.clear_color[3] = 1.0f;  // opaque black
    post.has_clear_color = 1;
    post.clear_color[0] = 0.3f;
    CameraView venue, track, hud;
    venue.cam = 0x10;
    venue.zrange[0] = 0.1f;
    track.cam = 0x20;
    hud.cam = 0x10;  // the venue's camera again, as the post frame had it
    hud.zrange[0] = 0.5f;
    world.cameras = {venue, track};
    post.cameras = {hud, CameraView{}};
    post.cameras[1].cam = 0x30;
    auto fc = ComposeFrame(world, post);
    REQUIRE(fc);
    CHECK(fc->has_clear_color == 1);
    CHECK(fc->clear_color[0] == 0.0f);
    CHECK(fc->clear_color[3] == 1.0f);
    REQUIRE(fc->cameras.size() == 3);
    CHECK(fc->cameras[0].cam == 0x10);
    CHECK(fc->cameras[0].zrange[0] == 0.1f);  // the world's
    CHECK(fc->cameras[1].cam == 0x20);
    CHECK(fc->cameras[2].cam == 0x30);
    CHECK(CameraOf(*fc, 0x30) == &fc->cameras[2]);
    CHECK(CameraOf(*fc, 0x40) == nullptr);
}
