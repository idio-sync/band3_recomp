// Checks that src/Render/soft_raster.cpp draws a capture's texture passes:
// a pass's DrawRect quad into a texture, sampled by a back-buffer draw after
// it, each draw seeing the version drawn before it, and a render target that
// nothing drew sampling transparent black; and that the world's draws, those
// before post_boundary, leave their alpha (the bloom weight) and depth in the
// scene target as RB3's back buffer has them, under the overlay's; that a
// draw culls the triangles its cull mode says (an outline's near side);
// that PreMultAlpha (blend 7) blends as RB3 sets it, ONE INVSRCALPHA; that
// a REFRACT_WORLD draw over the overlay reads the picture as the resolve
// left it; that a mesh's edges land on the pixels the game's do (D3D9's
// pixel centres), a DrawRect quad's on D3D10's; and that a soft particle
// fades by the scene's depth behind it, and the soft-particle buffer's blur
// takes its taps from the other surface; that a shadow map's pass draws
// depth alone, which a SHADOW_BUFFER draw after it reads; that NgLight's
// shadow is its casters' white silhouettes, cleared first and blurred twice
// in place, which the projected light's draws read; that a normal map a
// texture pass draws (a head's) is that pass's target, which tilts the
// normal of the draw after it in the frame its tangents give; and that a
// crowd billboard's quad is turned to the camera.

#include <doctest/doctest.h>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>
#include "src/Render/soft_raster.h"
#include "src/Render/spot_model.h"

using namespace band3::render;

namespace {

constexpr uint32_t kTex = 0x20CA66D8;  // a DxTex, as the capture keeps it
constexpr uint32_t kRed = 0xff0000ffu, kGreen = 0xff00ff00u;

Mat4 Identity() {
    Mat4 m{};
    for (int i = 0; i < 4; i++) m.m[i][i] = 1.0f;
    return m;
}

// a quad from x0 to x1 in clip space, full height unless y0 (its top) and
// y1 say, uv 0..1, in one colour
std::shared_ptr<const Geometry> Quad(float x0, float x1, uint32_t color, float y0 = 1,
                                     float y1 = -1) {
    auto g = std::make_shared<Geometry>();
    const float corner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (const auto& c : corner) {
        Vertex v{};
        v.pos[0] = x0 + c[0] * (x1 - x0);
        v.pos[1] = y0 + c[1] * (y1 - y0);
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

TEST_CASE("PreMultAlpha adds its colour as it is over what's there, by 1 - alpha") {
    // RndMat's blend 7, ONE INVSRCALPHA, as the outfit layers draw: green
    // already scaled by its half alpha, over red on the texture's left half
    // and the clear's transparent black on its right
    constexpr uint32_t kHalfGreen = 0x80004000u;  // G 64, alpha 128
    FrameCapture f;
    DrawItem red = Item(Quad(-1, 0, kRed), kTex);
    red.rect_shader = 6;
    DrawItem layer = Fill(kHalfGreen);
    layer.blend = 7;
    f.draws = {red, layer, Sample(-1, 1, 1)};
    Pass pass = TexturePass(0, 1);
    pass.draw_count = 2;
    f.passes = {pass, BackBuffer(2, 1)};
    std::vector<uint32_t> tex;
    uint32_t w = 0, h = 0;
    REQUIRE(RasterizeTarget(f, Small(), kTex, 1, tex, w, h));
    // over red: red * (1 - 0.5) plus the green, alpha 0.5 + 1 * 0.5
    CHECK((tex[1 * 4 + 1] & 0xff) == 127);
    CHECK(((tex[1 * 4 + 1] >> 8) & 0xff) == 64);
    CHECK((tex[1 * 4 + 1] >> 24) == 255);
    // over nothing: the green as it is (not scaled again), alpha its own
    CHECK((tex[1 * 4 + 3] & 0xffffff) == 0x004000u);
    CHECK((tex[1 * 4 + 3] >> 24) == 128);

    // the same into the back buffer; Screen, Lighten and Darken (8..10),
    // which NgMat sets no blend state for, draw as Src
    FrameCapture b;
    DrawItem over = Item(Quad(-1, 1, kHalfGreen), 0);
    over.blend = 7;
    b.draws = {Item(Quad(-1, 1, kRed), 0), over};
    b.passes = {BackBuffer(0, 2)};
    std::vector<uint32_t> rgba;
    Rasterize(b, Small(), rgba);
    CHECK(rgba[1 * 8 + 1] == 0xff00407fu);
    for (int mode = 8; mode <= 10; mode++) {
        b.draws[1].blend = mode;
        Rasterize(b, Small(), rgba);
        CHECK(rgba[1 * 8 + 1] == 0xff004000u);
    }
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

TEST_CASE("a draw culls the side its cull mode says, as the game's device did") {
    // Quad's triangles go clockwise on the screen; the right half's are turned
    // around, counter-clockwise
    auto turned = std::make_shared<Geometry>(*Quad(0, 1, kGreen));
    turned->indices = {0, 2, 1, 0, 3, 2};
    FrameCapture f;
    f.draws = {Item(Quad(-1, 0, kRed), 0), Item(turned, 0)};
    f.passes = {BackBuffer(0, 2)};
    std::vector<uint32_t> rgba;
    auto drawn = [&](const RasterOptions& o, uint8_t cull, bool& left, bool& right) {
        for (DrawItem& d : f.draws) d.cull = cull;
        Rasterize(f, o, rgba);
        left = rgba[1 * 8 + 1] == kRed;
        right = rgba[1 * 8 + 6] == kGreen;
    };
    bool left, right;
    drawn(Small(), 0, left, right);
    CHECK(left);
    CHECK(right);
    // D3DCULL_CW, what RndMat's cull flag sets: the clockwise ones go
    drawn(Small(), kCullBack, left, right);
    CHECK_FALSE(left);
    CHECK(right);
    // D3DCULL_CCW, a reflection's
    drawn(Small(), kCullBack | kCullFrontIsCw, left, right);
    CHECK(left);
    CHECK_FALSE(right);
    // culling off draws both sides, as the native view did before it culled
    RasterOptions o = Small();
    o.culling = false;
    drawn(o, kCullBack, left, right);
    CHECK(left);
    CHECK(right);
}

TEST_CASE("PreMultAlpha keeps the larger of the two alphas in the scene, as it blends") {
    // RB3's back buffer blends alpha ONE ONE MAX wherever the colour blends:
    // a PreMultAlpha layer that writes alpha (alpha_write) 1/16 over red's
    // PSEUDO_HDR 0.3 leaves 0.3
    FrameCapture f;
    f.shades = {FlatShade(true), FlatShade(false)};
    f.shades[1].alpha_write = 1;
    f.draws = {Shaded(-1, 1, 0xff0000ffu, 0, 1), Shaded(-1, 1, 0x10ff0000u, 1, 7)};
    f.passes = {BackBuffer(0, 2)};
    f.post_boundary = 2;
    RasterOptions o = Small();
    o.view = RasterView::kSceneAlpha;
    std::vector<uint32_t> rgba;
    Rasterize(f, o, rgba);
    CHECK((rgba[1 * 8 + 1] & 0xff) == 77);  // 0.3 * 255
}

TEST_CASE("a REFRACT_WORLD draw in the overlay is its colour times the picture behind it") {
    // the world red; the overlay green over all of it, then a half grey
    // REFRACT_WORLD quad, which reads the picture as the resolve left it
    // (red), not the green drawn over it since
    FrameCapture f;
    f.shades = {FlatShade(false), FlatShade(false)};
    f.shades[1].options |= 1ull << 46;
    f.draws = {Shaded(-1, 1, kRed, 0, 1), Shaded(-1, 1, kGreen, 0, 1),
               Shaded(-1, 1, 0xff808080u, 1, 1)};
    f.passes = {BackBuffer(0, 3)};
    f.post_boundary = 1;
    std::vector<uint32_t> rgba;
    Rasterize(f, Small(), rgba);
    CHECK(rgba[1 * 8 + 3] == 0xff000080u);  // red 255 * 128 / 255

    // in the world, before the resolve, it draws as it is
    f.post_boundary = 3;
    Rasterize(f, Small(), rgba);
    CHECK(rgba[1 * 8 + 3] == 0xff808080u);
}

TEST_CASE("a crowd billboard's quad, in its mesh's XZ, is turned to the camera") {
    // the quad from x -1..0, z 1..-1 (y 0), at an instance moved 1 right and
    // scaled 2: drawn as a mesh, with the identity view-projection, it's
    // edge-on (no height on the screen) and draws nothing; BILLBOARD turns it
    // to the camera (VS c16..c18: right x, up y, forward z, as clip space
    // has them here) and leaves the scale out, so it covers the right half,
    // its winding turned counter-clockwise there: kept by D3DCULL_CW, as the
    // game's crowd draws are
    auto quad = std::make_shared<Geometry>(*Quad(-1, 0, kGreen));
    for (Vertex& v : quad->verts) std::swap(v.pos[1], v.pos[2]);
    quad->indices = {0, 2, 1, 0, 3, 2};
    FrameCapture f;
    f.shades = {FlatShade(false)};
    ShadeState& s = f.shades[0];
    s.shader_type = 12;
    for (int i = 0; i < 3; i++) s.vs[ShadeRegIndex(16 + i)][i] = 1.0f;
    DrawItem d = Item(quad, 0);
    d.shade = 0;
    d.cull = kCullBack;
    for (int i = 0; i < 3; i++) d.world.m[i][i] = 2.0f;
    d.world.m[3][0] = 1.0f;
    f.draws = {d};
    f.passes = {BackBuffer(0, 1)};
    std::vector<uint32_t> rgba;
    Rasterize(f, Small(), rgba);
    CHECK(std::count(rgba.begin(), rgba.end(), kGreen) == 0);
    s.options |= 1ull << shader_opt::kBillboard;
    Rasterize(f, Small(), rgba);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 8; x++) CHECK((rgba[y * 8 + x] == kGreen) == (x >= 4));
}

TEST_CASE("a mesh's edges land on the game's pixels, D3D9's; a DrawRect quad's on D3D10's") {
    // RB3's device samples pixel x at x (HalfPixelOffset off): a quad from
    // the screen's left edge to x 2.25 and from y 1.25 down covers columns 0
    // to 2 and rows 2 and 3, where D3D10's centres (x + .5) would give it
    // columns 0 and 1 and rows 1 to 3
    auto at = [](float px, float size) { return px / size * 2.0f - 1.0f; };
    FrameCapture f;
    f.draws = {Item(Quad(-1, at(2.25f, 8), kRed, 1.0f - 1.25f / 4 * 2, -1), 0)};
    f.passes = {BackBuffer(0, 1)};
    std::vector<uint32_t> rgba;
    Rasterize(f, Small(), rgba);
    CHECK(rgba[2 * 8 + 2] == kRed);
    CHECK(rgba[2 * 8 + 3] != kRed);
    CHECK(rgba[1 * 8 + 0] != kRed);
    CHECK(rgba[3 * 8 + 0] == kRed);

    // an edge on a pixel's centre is the right quad's only (the top-left
    // rule: its left edge): two quads meeting at x 3 add there once
    DrawItem left = Item(Quad(-1, at(3, 8), 0xff404040u), 0);
    DrawItem right = Item(Quad(at(3, 8), 1, 0xff404040u), 0);
    left.blend = right.blend = 2;  // Add, over the clear's grey 0x20
    f.draws = {left, right};
    f.passes = {BackBuffer(0, 2)};
    Rasterize(f, Small(), rgba);
    for (int x = 0; x < 8; x++) CHECK(rgba[1 * 8 + x] == 0xff606060u);

    // DrawRect's quads sample at .5 (HalfPixelOffset on): a rect from 0 to
    // 1.25 into the 4x4 texture is its first column only
    FrameCapture r;
    DrawItem rect = Item(Quad(-1, at(1.25f, 4), kRed), kTex);
    rect.rect_shader = 6;
    r.draws = {rect};
    r.passes = {TexturePass(0, 1)};
    std::vector<uint32_t> tex;
    uint32_t w = 0, h = 0;
    REQUIRE(RasterizeTarget(r, Small(), kTex, 1, tex, w, h));
    CHECK(tex[1 * 4 + 0] == kRed);
    CHECK(tex[1 * 4 + 1] == 0);
}

namespace {

constexpr uint32_t kSoft0 = 0x2387D148, kSoft1 = 0x2387D1F8;

// a view-projection that puts world (x w, y w, w) at clip (x, y) with clip
// w = w: a quad's view depth
Mat4 Perspective() {
    Mat4 m{};
    m.m[0][0] = m.m[1][1] = 1.0f;
    m.m[2][3] = 1.0f;
    return m;
}

// Quad(x0, x1) at view depth w, through Perspective()
std::shared_ptr<const Geometry> QuadAt(float x0, float x1, float w, uint32_t color) {
    auto g = std::make_shared<Geometry>(*Quad(x0, x1, color));
    for (Vertex& v : g->verts) {
        v.pos[0] *= w;
        v.pos[1] *= w;
        v.pos[2] = w;
    }
    return g;
}

// a soft particle's shade: the particle shader (unlit, the vertex colour
// times VS c0 and c1) with option bit 45, and the camera's range in PS c89
ShadeState SoftShade() {
    ShadeState s;
    std::memset(static_cast<ShadeInputs*>(&s), 0, sizeof(ShadeInputs));
    s.shader_type = kParticleShader;
    s.options = 1ull << shader_opt::kPrelit | 1ull << shader_opt::kSoftParticles;
    for (int c = 0; c < 4; c++) {
        s.vs[ShadeRegIndex(0)][c] = 1.0f;
        s.vs[ShadeRegIndex(1)][c] = 1.0f;
    }
    const float c89[4] = {10, 10000, 1.0f / 0.9f, 0.1f / 0.9f};
    for (int c = 0; c < 4; c++) s.ps[ShadeRegIndex(89)][c] = c89[c];
    s.vs[ShadeRegIndex(20)][0] = 1.0f;
    s.vs[ShadeRegIndex(21)][1] = 1.0f;
    return s;
}

// BlurSurface's first pass's taps into an 8x4 surface: -1.5..2.5 texels
// across, half a texel down, weights .1 .25 .3 .25 .1
ShadeState SoftBlurShade() {
    ShadeState s;
    std::memset(static_cast<ShadeInputs*>(&s), 0, sizeof(ShadeInputs));
    s.shader_type = 1;
    const float weights[5] = {0.1f, 0.25f, 0.3f, 0.25f, 0.1f};
    for (int i = 0; i < 5; i++) {
        s.ps[ShadeRegIndex(31 + i)][0] = (float(i) - 1.5f) / 8.0f;
        s.ps[ShadeRegIndex(31 + i)][1] = 0.5f / 4.0f;
        for (int c = 0; c < 4; c++) s.ps[ShadeRegIndex(47 + i)][c] = weights[i];
    }
    return s;
}

Pass SoftPass(uint32_t tex, uint32_t first, uint32_t clear, uint32_t version) {
    Pass p;
    p.tex_obj = tex;
    p.first_draw = first;
    p.draw_count = 1;
    p.width = 8;
    p.height = 4;
    p.tex_type = 0x22;
    p.clear_flags = clear;
    p.version = version;
    p.viewport[2] = 8;
    p.viewport[3] = 4;
    return p;
}

}  // namespace

TEST_CASE("a soft particle fades by the scene's depth behind it; its buffer blurs across") {
    // the world: an opaque quad over the left half at view depth 100; then,
    // after post-processing starts, RndSoftParticleBuffer's pass: a white
    // particle over all of its 8x4 surface at depth 76, SrcAlphaAdd
    FrameCapture f;
    f.shades = {FlatShade(false), SoftShade(), SoftBlurShade()};
    DrawItem world = Shaded(-1, 0, 0xffffffffu, 0, 1);
    world.geom = QuadAt(-1, 0, 100, 0xffffffffu);
    world.view_proj = Perspective();
    world.z_mode = 1;
    DrawItem particle = Item(QuadAt(-1, 1, 76, 0xffffffffu), kSoft0);
    particle.view_proj = Perspective();
    particle.shade = 1;
    particle.blend = 4;
    particle.z_mode = 2;
    // BlurSurface's first pass: the first surface across into the second
    DrawItem blur = Item(Quad(-1, 1, 0xffffffffu), kSoft1);
    blur.rect_shader = 1;
    blur.rect[2] = 8;
    blur.rect[3] = 4;
    blur.shade = 2;
    auto src = std::make_shared<Texture>();
    src->width = 8;
    src->height = 4;
    src->tex_obj = kSoft0;
    src->tex_type = 0x22;
    src->version = 1;
    blur.tex = src;
    f.draws = {world, particle, blur};
    f.passes = {BackBuffer(0, 1), SoftPass(kSoft0, 1, 0x0f, 1), SoftPass(kSoft1, 2, 0, 1)};
    f.post_boundary = 1;
    f.post_consts.soft_surface[0] = kSoft0;
    f.post_consts.soft_surface[1] = kSoft1;
    REQUIRE(IsSoftParticle(particle, &f.shades[1]));

    // in front of the world by 24 of the fade's 48: half its alpha, which
    // SrcAlphaAdd scales the white by; where the world drew nothing (the far
    // plane) all of it. The surface's pixel x reads the scene's depth at x
    // of the 8 across.
    std::vector<uint32_t> tex;
    uint32_t w = 0, h = 0;
    REQUIRE(RasterizeTarget(f, Small(), kSoft0, 1, tex, w, h));
    REQUIRE(w == 8);
    for (int x = 0; x < 8; x++) {
        CAPTURE(x);
        // (128 but for float rounding at exactly half)
        const int r = int(tex[1 * 8 + x] & 0xff);
        if (x < 4)
            CHECK((r == 127 || r == 128));
        else
            CHECK(r == 255);
    }

    // the blur: taps half a texel apart from the texels' centres, so each
    // pixel takes .05 .175 .275 .275 .175 .05 of texels x-2..x+3 (clamped)
    REQUIRE(RasterizeTarget(f, Small(), kSoft1, 1, tex, w, h));
    const float kernel[6] = {0.05f, 0.175f, 0.275f, 0.275f, 0.175f, 0.05f};
    for (int x = 0; x < 8; x++) {
        float want = 0;
        for (int k = 0; k < 6; k++) {
            const int t = std::clamp(x - 2 + k, 0, 7);
            want += kernel[k] * (t < 4 ? 128.0f : 255.0f);
        }
        CAPTURE(x);
        CHECK(float(tex[2 * 8 + x] & 0xff) == doctest::Approx(want).epsilon(0.01));
    }
}

namespace {

constexpr uint32_t kShadowTex = 0x2251A0C0;

// RndShadowMap's pass into its 4x4 shadow map: a quad over all of it at
// clip z 0.5 (w 1), drawn in draw mode 1, cleared to depth 1 as DxCam::Select
// clears it
DrawItem ShadowCaster() {
    auto g = std::make_shared<Geometry>(*Quad(-1, 1, 0xffffffffu));
    for (Vertex& v : g->verts) v.pos[2] = 0.5f;
    DrawItem d = Item(g, kShadowTex);
    d.draw_mode = kDrawModeShadowDepth;
    d.z_mode = 1;
    return d;
}

Pass ShadowPass(uint32_t first, uint32_t version) {
    Pass p;
    p.tex_obj = kShadowTex;
    p.first_draw = first;
    p.draw_count = 1;
    p.width = p.height = 4;
    p.tex_type = kTexTypeShadowMap;
    p.clear_flags = 0x30;
    p.clear_z = 1.0f;
    p.viewport[2] = 4;
    p.viewport[3] = 4;
    p.version = version;
    return p;
}

// A lit SHADOW_BUFFER material: white, ambient 0.25, a light far above of
// 0.5 (N.L 1 to a hair), the light camera looking down (c108 -z) and c107
// (1, 0.5, 0). Its shadow coordinate is (0.5, 0.5, depth, 1) everywhere:
// the map's middle, the four taps all in the caster's quad.
ShadeState ShadowedShade(float depth, uint32_t version) {
    ShadeState s;
    std::memset(static_cast<ShadeInputs*>(&s), 0, sizeof(ShadeInputs));
    s.shader_type = 18;
    s.options = 1ull << shader_opt::kRealLights | 1ull << shader_opt::kPerPixel |
                1ull << shader_opt::kShadowBuffer | 1ull << shader_opt::kNumPoint;
    auto set = [&](float (&bank)[kNumShadeRegs][4], int reg, float x, float y, float z, float w) {
        float* r = bank[ShadeRegIndex(reg)];
        r[0] = x;
        r[1] = y;
        r[2] = z;
        r[3] = w;
    };
    set(s.ps, 0, 1, 1, 1, 1);
    set(s.ps, 1, 0.25f, 0.25f, 0.25f, 1);
    set(s.ps, 64, 0, 0, 1e5f, 0);
    set(s.ps, 67, 0.5f, 0.5f, 0.5f, 1);
    set(s.ps, 107, 1, 0.5f, 0, 0);
    set(s.ps, 108, 0, 0, -1, 1);
    set(s.vs, 20, 1, 0, 0, 0);
    set(s.vs, 21, 0, 1, 0, 0);
    set(s.vs, 40, 0, 0, 0, 0.5f);
    set(s.vs, 41, 0, 0, 0, 0.5f);
    set(s.vs, 42, 0, 0, 0, depth);
    set(s.vs, 43, 0, 0, 0, 1);
    auto map = std::make_shared<Texture>();
    map->width = map->height = 4;
    map->tex_obj = kShadowTex;
    map->tex_type = kTexTypeShadowMap;
    map->version = version;
    s.maps[kMapProjected] = map;
    return s;
}

// the back buffer's draw of it, over all of the picture, facing up
DrawItem Shadowed() {
    auto g = std::make_shared<Geometry>(*Quad(-1, 1, 0xffffffffu));
    for (Vertex& v : g->verts) v.nrm[2] = 1.0f;
    DrawItem d = Item(g, 0);
    d.prelit = false;
    d.shade = 0;
    return d;
}

}  // namespace

TEST_CASE("a shadow map's pass draws depth, which a SHADOW_BUFFER draw after it reads") {
    FrameCapture f;
    f.shades = {ShadowedShade(0.75f, 1)};
    f.draws = {ShadowCaster(), Shadowed()};
    f.passes = {ShadowPass(0, 1), BackBuffer(1, 1)};
    std::vector<uint32_t> rgba;
    RasterStats st = Rasterize(f, Small(), rgba);
    CHECK(st.passes == 1);
    // behind the caster: the light, 0.5, times 1 - 0.75 c107 (0.25, 0.625,
    // 1), over the ambient 0.25
    auto channel = [&](int c) { return int((rgba[1 * 8 + 3] >> (8 * c)) & 0xff); };
    CHECK(std::abs(channel(0) - 96) <= 1);   // 0.375
    CHECK(std::abs(channel(1) - 143) <= 1);  // 0.5625
    CHECK(std::abs(channel(2) - 191) <= 1);  // 0.75
    const uint32_t shadowed = rgba[1 * 8 + 3];
    for (uint32_t c : rgba) CHECK(c == shadowed);

    // in front of it: lit, as without the shadow map
    f.shades = {ShadowedShade(0.25f, 1)};
    Rasterize(f, Small(), rgba);
    const uint32_t lit = rgba[1 * 8 + 3];
    CHECK(std::abs(int(lit & 0xff) - 191) <= 1);
    CHECK((lit & 0xff) == ((lit >> 16) & 0xff));
    f.shades = {ShadowedShade(0.75f, 1)};
    RasterOptions o = Small();
    o.self_shadow = false;
    st = Rasterize(f, o, rgba);
    CHECK(st.passes == 0);
    CHECK(rgba[1 * 8 + 3] == lit);

    // a version of the map no pass here drew (the character's before): lit
    f.shades = {ShadowedShade(0.75f, 7)};
    Rasterize(f, Small(), rgba);
    CHECK(rgba[1 * 8 + 3] == lit);

    // the map itself: the caster's depth, 0.5, as grey; where it's culled
    // (its cull mode drops clockwise triangles, which Quad's are) the clear
    std::vector<uint32_t> tex;
    uint32_t w = 0, h = 0;
    REQUIRE(RasterizeTarget(f, Small(), kShadowTex, 1, tex, w, h));
    CHECK(w == 4);
    CHECK(tex[5] == 0xff808080u);
    f.draws[0].cull = kCullBack;
    REQUIRE(RasterizeTarget(f, Small(), kShadowTex, 1, tex, w, h));
    CHECK(tex[5] == 0xff000000u);
    Rasterize(f, Small(), rgba);
    CHECK(rgba[1 * 8 + 3] == lit);

    // a caster partly in front of the light camera's near plane (z < 0) is
    // cut there, as the device clips it, not stored nearer than anything:
    // the quad from z -0.5 on the left to 1.5 on the right holds 0..1 in the
    // middle (pixel x samples z -0.5 + x / 2)
    auto slope = std::make_shared<Geometry>(*f.draws[0].geom);
    for (Vertex& v : slope->verts) v.pos[2] = v.pos[0] < 0 ? -0.5f : 1.5f;
    f.draws[0].geom = slope;
    f.draws[0].cull = 0;
    REQUIRE(RasterizeTarget(f, Small(), kShadowTex, 1, tex, w, h));
    CHECK(tex[4 + 0] == 0xff000000u);  // pixel 0: z -0.5, clipped
    CHECK(tex[4 + 2] == 0xff808080u);  // pixel 2: z 0.5
    CHECK(tex[4 + 3] == 0xff000000u);  // pixel 3: z 1, no nearer than the clear
}

namespace {

constexpr uint32_t kLightTex = 0x2261B598;  // NgLight's mShadowRT, 8x4 here

// a texture of NgLight's shadow, as a draw's s5 or a blur's quad keeps it
std::shared_ptr<Texture> LightShadow(uint32_t version) {
    auto t = std::make_shared<Texture>();
    t->width = 8;
    t->height = 4;
    t->tex_obj = kLightTex;
    t->tex_type = 0x22;
    t->version = version;
    return t;
}

// NgLight::RenderShadows' pass, as the capture records it: no camera, so no
// clear and no viewport
Pass LightPass(uint32_t first, uint32_t version) {
    Pass p = SoftPass(kLightTex, first, 0, version);
    p.viewport[2] = p.viewport[3] = 0;
    return p;
}

// a shadow caster's shade in draw mode 3: the standard shader with no
// options (RndShaderStandard::CalcShaderOpts), its material's brown colour
// half transparent in c0
ShadeState CasterShade() {
    ShadeState s;
    std::memset(static_cast<ShadeInputs*>(&s), 0, sizeof(ShadeInputs));
    s.shader_type = 18;
    const float c0[4] = {0.51f, 0.42f, 0.23f, 0.5f};
    for (int c = 0; c < 4; c++) {
        s.ps[ShadeRegIndex(0)][c] = s.vs[ShadeRegIndex(0)][c] = c0[c];
        s.ps[ShadeRegIndex(1)][c] = s.vs[ShadeRegIndex(1)][c] = 1.0f;
    }
    s.vs[ShadeRegIndex(20)][0] = 1.0f;
    s.vs[ShadeRegIndex(21)][1] = 1.0f;
    return s;
}

// BlurShadowRT's taps into the 8x4 shadow: a texel apart, across or down,
// weights .1 .25 .3 .25 .1
ShadeState LightBlurShade(bool down) {
    ShadeState s;
    std::memset(static_cast<ShadeInputs*>(&s), 0, sizeof(ShadeInputs));
    s.shader_type = 1;
    const float weights[5] = {0.1f, 0.25f, 0.3f, 0.25f, 0.1f};
    for (int i = 0; i < 5; i++) {
        float* off = s.ps[ShadeRegIndex(31 + i)];
        off[down ? 1 : 0] = float(i - 2) / (down ? 4.0f : 8.0f);
        off[2] = off[3] = 1;
        for (int c = 0; c < 4; c++) s.ps[ShadeRegIndex(47 + i)][c] = weights[i];
    }
    return s;
}

// A lit PROJ_MULTIPLY material reading the shadow as its s5 (`map`, null
// none): white, ambient 0.25, a light far above of 0.5, the projected light
// white from above, its uv the world position's (x, -y) from 0..1 over the
// picture's -1..1
ShadeState ProjectedShade(std::shared_ptr<Texture> map) {
    ShadeState s;
    std::memset(static_cast<ShadeInputs*>(&s), 0, sizeof(ShadeInputs));
    s.shader_type = 18;
    s.options = 1ull << shader_opt::kRealLights | 1ull << shader_opt::kPerPixel |
                1ull << shader_opt::kNumPoint | 1ull << shader_opt::kNumProj |
                1ull << shader_opt::kProjLightMultiply;
    auto set = [&](int reg, float x, float y, float z, float w) {
        float* r = s.ps[ShadeRegIndex(reg)];
        r[0] = x;
        r[1] = y;
        r[2] = z;
        r[3] = w;
    };
    set(0, 1, 1, 1, 1);
    set(1, 0.25f, 0.25f, 0.25f, 1);
    set(64, 0, 0, 1e5f, 0);
    set(67, 0.5f, 0.5f, 0.5f, 1);
    set(66, 0, 0, 1, 0);
    set(69, 1, 1, 1, 1);
    set(95, 0.5f, 0, 0, 0.5f);
    set(96, 0, -0.5f, 0, 0.5f);
    set(97, 0, 0, 0, 1);
    s.vs[ShadeRegIndex(20)][0] = 1.0f;
    s.vs[ShadeRegIndex(21)][1] = 1.0f;
    s.maps[kMapProjected] = std::move(map);
    return s;
}

}  // namespace

TEST_CASE("NgLight's shadow: its casters' silhouettes, blurred twice, darken the projected light") {
    // the shadow pass: a caster over the left half, its material's texture
    // and colour, in draw mode 3; two in-place blurs, across then down; the
    // back buffer's projected light over all of the picture, reading the
    // last version. Before them a pass that filled the shadow red.
    FrameCapture f;
    f.shades = {CasterShade(), LightBlurShade(false), LightBlurShade(true),
                ProjectedShade(LightShadow(4))};
    DrawItem fill = Fill(kRed);
    fill.target = kLightTex;
    DrawItem caster = Item(Quad(-1, 0, 0xff0000ffu), kLightTex);
    caster.draw_mode = kDrawModeShadowCasters;
    caster.shade = 0;
    caster.prelit = false;
    caster.tex = std::make_shared<Texture>(Texture{1, 1, {kGreen}});
    DrawItem blurs[2];
    for (int k = 0; k < 2; k++) {
        blurs[k] = Item(Quad(-1, 1, 0xffffffffu), kLightTex);
        blurs[k].rect_shader = 1;
        blurs[k].rect[2] = 8;
        blurs[k].rect[3] = 4;
        blurs[k].tex = LightShadow(2 + uint32_t(k));
        blurs[k].shade = 1 + k;
    }
    auto up = std::make_shared<Geometry>(*Quad(-1, 1, 0xffffffffu));
    for (Vertex& v : up->verts) v.nrm[2] = 1.0f;
    DrawItem lit = Item(up, 0);
    lit.prelit = false;
    lit.shade = 3;
    f.draws = {fill, caster, blurs[0], blurs[1], lit};
    Pass filled = LightPass(0, 1);
    filled.clear_flags = 0x0f;
    f.passes = {filled, LightPass(1, 2), LightPass(2, 3), LightPass(3, 4), BackBuffer(4, 1)};
    REQUIRE(spot::SpotBlur(blurs[0], &f.shades[1], f.passes[2]));
    CHECK(ShadowCasterPass(f, f.passes[1]));
    CHECK_FALSE(ShadowCasterPass(f, f.passes[2]));
    // NgLight clears it though no camera does: what the red pass drew isn't
    // seen, so it isn't drawn
    CHECK(PassClearFlags(f, f.passes[1]) == 0x0f);
    const std::vector<PassRun> runs = PlanPasses(f, Small());
    REQUIRE(runs.size() == 4);
    CHECK(runs[0].pass == &f.passes[1]);

    // the casters: opaque white whatever their texture and colour, over
    // transparent black
    std::vector<uint32_t> tex;
    uint32_t w = 0, h = 0;
    REQUIRE(RasterizeTarget(f, Small(), kLightTex, 2, tex, w, h));
    REQUIRE(w == 8);
    for (int x = 0; x < 8; x++) CHECK(tex[1 * 8 + x] == (x < 4 ? 0xffffffffu : 0u));
    // blurred across: x takes texels x-2..x+2 (clamped), .1 .25 .3 .25 .1
    REQUIRE(RasterizeTarget(f, Small(), kLightTex, 3, tex, w, h));
    const float kernel[5] = {0.1f, 0.25f, 0.3f, 0.25f, 0.1f};
    uint32_t across[8];
    for (int x = 0; x < 8; x++) {
        float want = 0;
        for (int k = 0; k < 5; k++) want += std::clamp(x - 2 + k, 0, 7) < 4 ? kernel[k] : 0.0f;
        CAPTURE(x);
        across[x] = tex[2 * 8 + x];
        CHECK(std::abs(int(Alpha(across[x])) - int(want * 255 + 0.5f)) <= 1);
        CHECK((across[x] & 0xff) == Alpha(across[x]));
    }
    // and down, which a shadow the same all the way down keeps
    REQUIRE(RasterizeTarget(f, Small(), kLightTex, 4, tex, w, h));
    for (int x = 0; x < 8; x++) CHECK(tex[0 * 8 + x] == across[x]);

    // the picture: pixel x samples the shadow at texel x - 0.5, bilinear;
    // where its alpha is 1 the light is 1 - 0.75 of itself (0.25 + 0.5 x
    // 0.25), where it's 0 all of it (0.25 + 0.5)
    std::vector<uint32_t> rgba;
    RasterStats st = Rasterize(f, Small(), rgba);
    CHECK(st.passes == 3);
    CHECK(st.rt_missing == 0);
    CHECK(std::abs(int(rgba[1 * 8 + 1] & 0xff) - 96) <= 1);
    CHECK(std::abs(int(rgba[1 * 8 + 7] & 0xff) - 191) <= 1);
    const float a = (float(Alpha(across[3])) + float(Alpha(across[4]))) / 2 / 255;
    CHECK(std::abs(int(rgba[1 * 8 + 4] & 0xff) - int((0.25f + 0.5f * (1 - 0.75f * a)) * 255 + 0.5f)) <=
          1);

    // a version no pass here drew (the frame before's) reads guest memory's
    // pixels where they're kept (here none of it shadowed), else leaves the
    // light out, counted as missing
    auto guest = LightShadow(9);
    guest->rgba.assign(8 * 4, 0x00ffffffu);
    f.shades[3] = ProjectedShade(guest);
    Rasterize(f, Small(), rgba);
    CHECK(std::abs(int(rgba[1 * 8 + 1] & 0xff) - 191) <= 1);
    f.shades[3] = ProjectedShade(LightShadow(9));
    st = Rasterize(f, Small(), rgba);
    CHECK(st.rt_missing == 1);
    CHECK(std::abs(int(rgba[1 * 8 + 1] & 0xff) - 191) <= 1);
}

TEST_CASE("a head's normal map is a texture pass's target, which tilts the draw after it") {
    // the pass fills the texture with x 1, y 0.5 (red 255, green 128); a lit
    // quad facing +z, a white light far along +x, which grazes it. Its
    // tangent (0, -1, 0), w 1, makes the bitangent the normal map's x pairs
    // with (0, 0, 1) x (0, -1, 0) = +x: the normal tilts to the light.
    const uint32_t kNormal = 0xff0080ffu;
    ShadeState s;
    std::memset(static_cast<ShadeInputs*>(&s), 0, sizeof(ShadeInputs));
    s.shader_type = 18;
    s.options = 1ull << shader_opt::kRealLights | 1ull << shader_opt::kPerPixel |
                1ull << shader_opt::kNumPoint | 1ull << shader_opt::kNormalMap;
    auto set = [&](float* r, float x, float y, float z, float w) {
        r[0] = x;
        r[1] = y;
        r[2] = z;
        r[3] = w;
    };
    set(s.ps[ShadeRegIndex(0)], 1, 1, 1, 1);
    set(s.ps[ShadeRegIndex(1)], 0, 0, 0, 1);
    set(s.ps[ShadeRegIndex(14)], 1, 0, 0, 0);
    set(s.ps[ShadeRegIndex(64)], 1e5f, 0, 0, 0);
    set(s.ps[ShadeRegIndex(67)], 1, 1, 1, 1);
    set(s.vs[ShadeRegIndex(20)], 1, 0, 0, 0);
    set(s.vs[ShadeRegIndex(21)], 0, 1, 0, 0);
    set(s.vs[ShadeRegIndex(22)], 0, 0, 1, 0);
    s.eye[2] = 10;
    auto map = std::make_shared<Texture>();
    map->width = map->height = 4;
    map->tex_obj = kTex;
    map->tex_type = 0x22;
    map->version = 1;
    s.maps[kMapNormal] = map;
    auto up = std::make_shared<Geometry>(*Quad(-1, 1, 0xffffffffu));
    up->tangents = true;
    for (Vertex& v : up->verts) {
        v.nrm[2] = 1.0f;
        v.tan[1] = -1.0f;
        v.tan[3] = 1.0f;
    }
    DrawItem lit = Item(up, 0);
    lit.prelit = false;
    lit.shade = 0;
    FrameCapture f;
    f.shades = {s};
    f.draws = {Fill(kNormal), lit};
    f.passes = {TexturePass(0, 1), BackBuffer(1, 1)};
    CHECK(MapTargetOf(&f.shades[0], kMapNormal) == map.get());
    const std::vector<PassRun> runs = PlanPasses(f, Small());
    REQUIRE(runs.size() == 2);
    CHECK(runs[0].pass == &f.passes[0]);

    // N = normalize(0 n + (1 (+x) + 0.004 u)): lit as it faces the light
    std::vector<uint32_t> rgba;
    RasterStats st = Rasterize(f, Small(), rgba);
    CHECK(st.passes == 1);
    CHECK(st.rt_missing == 0);
    CHECK((rgba[1 * 8 + 3] & 0xff) >= 254);

    // without normal maps, or with the geometry's tangents not kept, the
    // vertex normal, which the light grazes, and the pass isn't drawn
    RasterOptions o = Small();
    o.normal_maps = false;
    CHECK(PlanPasses(f, o).size() == 1);
    st = Rasterize(f, o, rgba);
    CHECK((rgba[1 * 8 + 3] & 0xff) == 0);
    up->tangents = false;
    Rasterize(f, Small(), rgba);
    CHECK((rgba[1 * 8 + 3] & 0xff) == 0);
    up->tangents = true;

    // with no pass of it, guest memory's pixels where they're kept (here
    // flat: the vertex normal), else the map is left out, counted as missing
    f.draws = {lit};
    f.passes = {BackBuffer(0, 1)};
    map->rgba.assign(16, 0xff008080u);
    st = Rasterize(f, Small(), rgba);
    CHECK(st.rt_missing == 0);
    CHECK((rgba[1 * 8 + 3] & 0xff) <= 2);
    map->rgba.clear();
    st = Rasterize(f, Small(), rgba);
    CHECK(st.rt_missing == 1);
    CHECK((rgba[1 * 8 + 3] & 0xff) == 0);
}

TEST_CASE("the display gamma ramp maps each value as the presenter shows it") {
    // RB3's sort of table: 10-bit, lifting the darks (a pow under 1), red
    // apart from green and blue to tell the channels apart
    GammaRamp g;
    g.mode = GammaRamp::kTable;
    for (uint32_t v = 0; v < 256; v++) {
        const uint32_t lift = std::min<uint32_t>(1023, v * 1023 / 255 + (v < 128 ? 24 : 0));
        const uint32_t red = 1023 - (255 - v) * 1023 / 255;
        g.table[v] = red << 20 | lift << 10 | lift;
    }
    uint8_t lut[3][256];
    GammaLut(g, lut);
    // 10 bits to 8 as CaptureGuestOutput: v * 255/1023 + 0.5, truncated
    CHECK(lut[1][0] == 6);  // 24 -> 5.98 + .5
    CHECK(lut[1][1] == 7);  // 28 -> 6.98 + .5
    CHECK(lut[1][128] == 128);
    CHECK(lut[2][4] == lut[1][4]);
    for (int v = 0; v < 256; v++) CHECK(lut[0][v] == v);
    CHECK_FALSE(IsIdentity(lut));
    CHECK(Unorm10To8(0x48) == 18);  // 17.95
    CHECK(Unorm10To8(1023) == 255);
    CHECK(Unorm10To8(2) == 0);  // 0.4985

    // D3D's PWL ramp for a 10-bit front buffer before a game sets its own
    // (register_table.inc): base step << 9, delta 8, identity but for its
    // last step, flat at 1023, where 254 (1019 in 10 bits) lands
    GammaRamp pwl;
    pwl.mode = GammaRamp::kPwl;
    for (uint32_t i = 0; i < 128; i++)
        for (int c = 0; c < 3; c++) pwl.pwl[i][c] = 0x200u << 16 | i << 9;
    pwl.pwl[127][0] = pwl.pwl[127][1] = pwl.pwl[127][2] = 0x0000FFC0;
    GammaLut(pwl, lut);
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++) CHECK(lut[c][v] == (v == 254 ? 255 : v));

    // over the finished picture, overlay and all, the CPU's and so the GPU's
    // (gamma.hlsl's pass reads the same lookup)
    FrameCapture f;
    f.draws.push_back(Item(Quad(-1, 0, 0xff804020u), 0));
    f.post_boundary = 1;
    f.draws.push_back(Item(Quad(0, 1, 0xff000001u), 0));
    f.gamma = g;
    std::vector<uint32_t> raw, graded;
    RasterOptions o = Small();
    o.gamma = false;
    Rasterize(f, o, raw);
    o.gamma = true;
    Rasterize(f, o, graded);
    REQUIRE(raw.size() == graded.size());
    GammaLut(g, lut);
    for (size_t i = 0; i < raw.size(); i++) {
        const uint32_t c = raw[i];
        const uint32_t want = lut[0][c & 0xff] | lut[1][c >> 8 & 0xff] << 8 |
                              lut[2][c >> 16 & 0xff] << 16 | (c & 0xff000000u);
        CAPTURE(i);
        CHECK(graded[i] == want);
    }
    CHECK(graded[0] != raw[0]);
    CHECK(graded[7] != raw[7]);

    // not on views of the scene target, and nothing from a capture from
    // before the ramp was kept
    o.view = RasterView::kSceneAlpha;
    std::vector<uint32_t> a, b;
    Rasterize(f, o, a);
    o.gamma = false;
    Rasterize(f, o, b);
    CHECK(a == b);
    o.view = RasterView::kFinal;
    o.gamma = true;
    f.gamma = GammaRamp{};
    Rasterize(f, o, graded);
    CHECK(graded == raw);
}
