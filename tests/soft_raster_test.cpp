// Checks that src/Render/soft_raster.cpp draws a capture's texture passes:
// a pass's DrawRect quad into a texture, sampled by a back-buffer draw after
// it, each draw seeing the version drawn before it, and a render target that
// nothing drew sampling transparent black; and that the world's draws, those
// before post_boundary, leave their alpha (the bloom weight) and depth in the
// scene target as RB3's back buffer has them, under the overlay's.

#include <doctest/doctest.h>
#include <cstring>
#include <memory>
#include <vector>
#include "src/Render/soft_raster.h"

using namespace band3::render;

namespace {

constexpr uint32_t kTex = 0x20CA66D8;  // a DxTex, as the capture keeps it
constexpr uint32_t kRed = 0xff0000ffu, kGreen = 0xff00ff00u;

Mat4 Identity() {
    Mat4 m{};
    for (int i = 0; i < 4; i++) m.m[i][i] = 1.0f;
    return m;
}

// a quad from x0 to x1 in clip space, full height, uv 0..1, in one colour
std::shared_ptr<const Geometry> Quad(float x0, float x1, uint32_t color) {
    auto g = std::make_shared<Geometry>();
    const float corner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (const auto& c : corner) {
        Vertex v{};
        v.pos[0] = x0 + c[0] * (x1 - x0);
        v.pos[1] = 1.0f - c[1] * 2.0f;
        v.uv[0] = c[0];
        v.uv[1] = c[1];
        v.color = color;
        g->verts.push_back(v);
    }
    g->indices = {0, 1, 2, 0, 2, 3};
    return g;
}

DrawItem Item(std::shared_ptr<const Geometry> g, uint32_t target) {
    DrawItem d;
    d.geom = std::move(g);
    d.world = d.view_proj = Identity();
    for (float& c : d.color) c = 1.0f;
    d.blend = 1;  // Src
    d.z_mode = 0;
    d.prelit = true;  // the vertex colour
    d.alpha_cut = false;
    d.alpha_threshold = 0;
    d.cam = 0;
    d.mesh = 0;
    d.target = target;
    return d;
}

// DrawRect's fill of the whole texture, as scene_capture.cpp records it
DrawItem Fill(uint32_t color) {
    DrawItem d = Item(Quad(-1, 1, color), kTex);
    d.rect_shader = 6;
    return d;
}

// a back-buffer draw over x0..x1 sampling version `version` of the texture
DrawItem Sample(float x0, float x1, uint32_t version) {
    auto t = std::make_shared<Texture>();
    t->width = t->height = 4;
    t->tex_obj = kTex;
    t->tex_type = 0x22;  // kRenderedNoZ
    t->version = version;
    DrawItem d = Item(Quad(x0, x1, 0xffffffffu), 0);
    d.tex = std::move(t);
    return d;
}

Pass TexturePass(uint32_t first, uint32_t version) {
    Pass p;
    p.tex_obj = kTex;
    p.first_draw = first;
    p.draw_count = 1;
    p.width = p.height = 4;
    p.tex_type = 0x22;
    p.clear_flags = 0x0f;
    p.version = version;
    return p;
}

Pass BackBuffer(uint32_t first, uint32_t count) {
    Pass p;
    p.first_draw = first;
    p.draw_count = count;
    return p;
}

RasterOptions Small() {
    RasterOptions o;
    o.width = 8;
    o.height = 4;
    return o;
}

}  // namespace

TEST_CASE("a texture pass is drawn and sampled by the draws after it, version by version") {
    // red into the texture, sampled on the left; green, sampled on the right
    FrameCapture f;
    f.draws = {Fill(kRed), Sample(-1, 0, 1), Fill(kGreen), Sample(0, 1, 2)};
    f.passes = {TexturePass(0, 1), BackBuffer(1, 1), TexturePass(2, 2), BackBuffer(3, 1)};
    std::vector<uint32_t> rgba;
    const RasterStats st = Rasterize(f, Small(), rgba);
    CHECK(st.passes == 2);
    CHECK(st.rt_missing == 0);
    CHECK(rgba[1 * 8 + 1] == kRed);
    CHECK(rgba[1 * 8 + 6] == kGreen);

    // and the texture holds the last version
    std::vector<uint32_t> tex;
    uint32_t w = 0, h = 0;
    REQUIRE(RasterizeTarget(f, Small(), kTex, 0, tex, w, h));
    CHECK(w == 4);
    CHECK(h == 4);
    CHECK(tex[5] == kGreen);
    REQUIRE(RasterizeTarget(f, Small(), kTex, 1, tex, w, h));
    CHECK(tex[5] == kRed);
}

TEST_CASE("a render target nothing drew samples transparent black, guest pixels if kept") {
    FrameCapture f;
    f.draws = {Sample(-1, 1, 7)};
    f.passes = {BackBuffer(0, 1)};
    std::vector<uint32_t> rgba;
    RasterStats st = Rasterize(f, Small(), rgba);
    CHECK(st.rt_missing == 1);
    CHECK((rgba[9] & 0xffffff) == 0);  // black; the back buffer's alpha stays 1

    // guest memory's pixels, unless they're not wanted
    auto t = std::make_shared<Texture>(*f.draws[0].tex);
    t->rgba.assign(16, kGreen);
    f.draws[0].tex = t;
    st = Rasterize(f, Small(), rgba);
    CHECK(st.rt_missing == 0);
    CHECK(rgba[9] == kGreen);
    RasterOptions o = Small();
    o.rt_guest_pixels = false;
    st = Rasterize(f, o, rgba);
    CHECK(st.rt_missing == 1);
    CHECK((rgba[9] & 0xffffff) == 0);
}

TEST_CASE("only the texture passes something samples are drawn, before post-processing") {
    FrameCapture f;
    f.draws = {Fill(kRed), Item(Quad(-1, 1, kGreen), 0), Fill(kGreen)};
    f.passes = {TexturePass(0, 1), BackBuffer(1, 1), TexturePass(2, 2)};
    // nothing samples the texture
    std::vector<PassRun> runs = PlanPasses(f, Small());
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].pass == nullptr);

    // the back buffer samples it: the first pass is drawn, the one after
    // post-processing starts isn't
    f.draws[1] = Sample(-1, 1, 1);
    f.post_boundary = 2;
    runs = PlanPasses(f, Small());
    REQUIRE(runs.size() == 2);
    CHECK(runs[0].pass == &f.passes[0]);
    CHECK(runs[1].pass == nullptr);
}

TEST_CASE("a texture target keeps alpha: cleared 0, drawn by the colour's blend") {
    // a half-transparent SrcAlpha layer over the clear's transparent black
    FrameCapture f;
    DrawItem layer = Fill(0x80ffffffu);  // white, alpha 128
    layer.blend = 3;                     // SrcAlpha
    f.draws = {layer, Sample(-1, 1, 1)};
    f.passes = {TexturePass(0, 1), BackBuffer(1, 1)};
    std::vector<uint32_t> tex;
    uint32_t w = 0, h = 0;
    REQUIRE(RasterizeTarget(f, Small(), kTex, 1, tex, w, h));
    // a = 0.5 * 0.5 + 0 * 0.5, the colour white * 0.5
    CHECK((tex[5] >> 24) == 64);
    CHECK((tex[5] & 0xff) == 128);
}

namespace {

// an unlit material's shade, its colour the vertex colour's (PRELIT), with
// PSEUDO_HDR if `hdr`: alpha then the luminance by c7's weights
ShadeState FlatShade(bool hdr) {
    ShadeState s;
    std::memset(static_cast<ShadeInputs*>(&s), 0, sizeof(ShadeInputs));
    s.shader_type = 18;
    s.options = 1ull << shader_opt::kPrelit;
    if (hdr) s.options |= 1ull << shader_opt::kPseudoHdr;
    for (int c = 0; c < 4; c++) {
        s.ps[ShadeRegIndex(0)][c] = 1.0f;  // colour
        s.ps[ShadeRegIndex(1)][c] = 1.0f;  // ambient
    }
    // SetBloomColor's weights for a threshold of 1
    const float bloom[4] = {0.3f, 0.59f, 0.11f, 1.0f};
    for (int c = 0; c < 4; c++) s.ps[ShadeRegIndex(7)][c] = bloom[c];
    s.vs[ShadeRegIndex(20)][0] = 1.0f;  // the texture transform, identity
    s.vs[ShadeRegIndex(21)][1] = 1.0f;
    return s;
}

DrawItem Shaded(float x0, float x1, uint32_t color, int32_t shade, int blend) {
    DrawItem d = Item(Quad(x0, x1, color), 0);
    d.shade = shade;
    d.blend = blend;
    return d;
}

uint32_t Alpha(uint32_t c) { return c >> 24; }

}  // namespace

TEST_CASE("the world's alpha is the bloom weight PSEUDO_HDR draws write, kept by the rest") {
    // red, PSEUDO_HDR: alpha 0.3 everywhere; green without it over the left
    // half keeps that; blue PSEUDO_HDR added over the right half leaves the
    // larger, red's; then the overlay's white over the left quarter
    FrameCapture f;
    f.shades = {FlatShade(true), FlatShade(false)};
    f.draws = {Shaded(-1, 1, 0xff0000ffu, 0, 1), Shaded(-1, 0, 0xff00ff00u, 1, 1),
               Shaded(0, 1, 0xffff0000u, 0, 2), Shaded(-1, -0.5f, 0xffffffffu, 1, 1)};
    f.passes = {BackBuffer(0, 4)};
    f.post_boundary = 3;
    std::vector<uint32_t> rgba;
    Rasterize(f, Small(), rgba);
    // the picture: opaque, the overlay over the world
    CHECK(rgba[1 * 8 + 0] == 0xffffffffu);
    CHECK(rgba[1 * 8 + 2] == kGreen);
    CHECK(rgba[1 * 8 + 6] == 0xffff00ffu);  // red plus blue

    RasterOptions o = Small();
    o.view = RasterView::kSceneAlpha;
    Rasterize(f, o, rgba);
    // 0.3 * 255, under the overlay too (it isn't in the scene), and grey
    CHECK(Alpha(rgba[1 * 8 + 0]) == 0xff);
    CHECK((rgba[1 * 8 + 0] & 0xff) == 77);
    CHECK((rgba[1 * 8 + 2] & 0xff) == 77);
    CHECK((rgba[1 * 8 + 6] & 0xff) == 77);
    CHECK(((rgba[1 * 8 + 6] >> 8) & 0xff) == 77);

    // a draw that writes it without blending (Src) leaves its own, smaller
    f.draws[2].blend = 1;
    Rasterize(f, o, rgba);
    CHECK((rgba[1 * 8 + 6] & 0xff) == 28);  // 0.11 * 255
    // and a material that asks writes alpha without PSEUDO_HDR: green's own 1
    f.shades[1].alpha_write = 1;
    Rasterize(f, o, rgba);
    CHECK((rgba[1 * 8 + 2] & 0xff) == 255);

    // a scene nothing drew is the clear's alpha 0
    FrameCapture empty;
    Rasterize(empty, o, rgba);
    CHECK(rgba[9] == 0xff000000u);
}

TEST_CASE("the world's depth is readable where it left it, before the overlay") {
    FrameCapture f;
    f.shades = {FlatShade(false)};
    DrawItem world = Shaded(-1, 0, 0xff0000ffu, 0, 1);
    world.z_mode = 1;  // tested and written, at w 1
    DrawItem overlay = Shaded(0, 1, 0xff00ff00u, 0, 1);
    overlay.z_mode = 1;
    overlay.cam = 2;  // a camera of its own, which clears depth
    f.draws = {world, overlay};
    f.passes = {BackBuffer(0, 2)};
    f.post_boundary = 1;
    RasterOptions o = Small();
    o.view = RasterView::kSceneDepth;
    std::vector<uint32_t> rgba;
    Rasterize(f, o, rgba);
    // w 1 is near: white; where the world drew nothing, black
    CHECK(rgba[1 * 8 + 1] == 0xffffffffu);
    CHECK(rgba[1 * 8 + 6] == 0xff000000u);
    CHECK(DepthViewGrey(kNearW / 4096.0f) == 0.0f);
    CHECK(DepthViewGrey(kNearW / 256.0f) == doctest::Approx(0.5f));
}
