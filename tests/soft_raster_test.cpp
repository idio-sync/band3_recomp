// Checks that src/Render/soft_raster.cpp draws a capture's texture passes:
// a pass's DrawRect quad into a texture, sampled by a back-buffer draw after
// it, each draw seeing the version drawn before it, and a render target that
// nothing drew sampling transparent black.

#include <doctest/doctest.h>
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
