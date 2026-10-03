// Checks the native view's capture files (src/Render/capture_file.cpp): a frame
// with shades and their maps, passes, render targets' versions and its
// post-processing comes back as it was saved; a section from a newer build is
// skipped or refused as it should be, and geometry from a smaller Vertex (or
// post-processing from a smaller PostParams) keeps what both have; geometry
// keeps its tangents, and geometry from before them (GEOM version 1) has
// none; files from before passes (B3CAP002) and before shades (B3CAP001),
// whose Vertex ended at its weights, still load, and draws
// from before their cull mode was kept cull nothing, those from before their
// draw mode was kept are the colour pass's, files from before the display
// gamma ramp was kept have none, and textures keep their mips and shades
// their samplers, which files from before them have none of, as files from
// before the frame's clear colour and back-buffer cameras were kept have
// neither (and cameras from a smaller CameraView keep what both have). A
// movie's planes are kept whole, where other textures are kept smaller.

#include <doctest/doctest.h>
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#include "src/Render/capture_file.h"

using namespace band3::render;

namespace {

std::string TempPath(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

std::shared_ptr<Texture> MakeTexture(uint32_t w, uint32_t h, uint32_t format, uint32_t seed) {
    auto t = std::make_shared<Texture>();
    t->width = w;
    t->height = h;
    t->format = format;
    for (uint32_t i = 0; i < w * h; i++) t->rgba.push_back(seed + i);
    return t;
}

std::shared_ptr<Geometry> MakeTriangle() {
    auto g = std::make_shared<Geometry>();
    g->verts.resize(3);
    for (int i = 0; i < 3; i++) g->verts[i].pos[i] = 1.0f;
    g->indices = {0, 1, 2};
    return g;
}

// the verts as B3CAP001 and B3CAP002 kept them: Vertex up to its weights
void PutOldVerts(std::vector<uint8_t>& out, const Geometry& g) {
    for (const Vertex& v : g.verts) {
        const auto* b = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), b, b + offsetof(Vertex, tan));
    }
}

DrawItem MakeDraw(std::shared_ptr<const Geometry> geom, int32_t shade) {
    DrawItem d{};
    d.geom = std::move(geom);
    d.blend = 3;
    d.z_mode = 1;
    d.color[0] = 0.5f;
    d.color[3] = 1.0f;
    d.mesh = 0x1234;
    d.shade = shade;
    return d;
}

}  // namespace

TEST_CASE("a capture keeps its shades and their maps") {
    FrameCapture fc;
    fc.frame = 42;
    auto geom = MakeTriangle();
    auto diffuse = MakeTexture(2, 2, 18, 100);
    auto normal = MakeTexture(4, 1, 49, 200);

    ShadeState lit;
    std::memset(static_cast<ShadeInputs*>(&lit), 0, sizeof(ShadeInputs));
    lit.options = 0x0000010000030031ull;
    lit.shader_type = 18;
    lit.eye[2] = 7.0f;
    lit.vs[ShadeRegIndex(1)][0] = 0.25f;
    lit.ps[ShadeRegIndex(80)][1] = 0.75f;
    lit.use_environ = 1;
    lit.next_pass = 0x82001000;
    lit.fetch[kMapNormal][1] = 0x1000031;
    lit.maps[kMapNormal] = normal;
    // the same texture as a draw's diffuse one, kept once
    lit.maps[kMapSpecular] = diffuse;
    ShadeState unlit;
    std::memset(static_cast<ShadeInputs*>(&unlit), 0, sizeof(ShadeInputs));
    unlit.shader_type = 14;
    fc.shades = {lit, unlit};

    fc.draws.push_back(MakeDraw(geom, 0));
    fc.draws.back().tex = diffuse;
    fc.draws.push_back(MakeDraw(geom, 1));
    fc.draws.push_back(MakeDraw(geom, -1));

    const std::string path = TempPath("band3_capture_file_test.cap");
    REQUIRE(SaveCapture(path, fc));
    auto back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);

    CHECK(back->frame == 42);
    REQUIRE(back->draws.size() == 3);
    CHECK(back->draws[0].shade == 0);
    CHECK(back->draws[1].shade == 1);
    CHECK(back->draws[2].shade == -1);
    REQUIRE(back->shades.size() == 2);
    const ShadeState& s = back->shades[0];
    CHECK(std::memcmp(static_cast<const ShadeInputs*>(&s), static_cast<const ShadeInputs*>(&lit),
                      sizeof(ShadeInputs)) == 0);
    CHECK(s.Option(shader_opt::kDiffuseMap));
    CHECK(s.OptionBits(shader_opt::kNumPoint, 2) == 1);
    CHECK(s.Vs(1)[0] == 0.25f);
    CHECK(s.Ps(80)[1] == 0.75f);
    CHECK(s.Vs(92) == nullptr);
    REQUIRE(s.maps[kMapNormal]);
    CHECK(s.maps[kMapNormal]->format == 49);
    CHECK(s.maps[kMapNormal]->rgba == normal->rgba);
    CHECK(s.maps[kMapSpecular] == back->draws[0].tex);
    CHECK(!s.maps[kMapGlow]);
    CHECK(back->shades[1].shader_type == 14);
}

TEST_CASE("a capture keeps textures and maps up to 2048 a side, and pixels it has already once") {
    FrameCapture fc;
    auto geom = MakeTriangle();
    auto diffuse = MakeTexture(512, 2, 6, 1);
    ShadeState s;
    std::memset(static_cast<ShadeInputs*>(&s), 0, sizeof(ShadeInputs));
    // the colour texture again, as another object
    s.maps[kMapSpecular] = MakeTexture(512, 2, 6, 1);
    s.maps[kMapNormal] = MakeTexture(4096, 2, 49, 9);
    s.maps[kMapGlow] = MakeTexture(2048, 4, 2, 3);
    fc.shades = {s};
    fc.draws.push_back(MakeDraw(geom, 0));
    fc.draws.back().tex = diffuse;

    const std::string path = TempPath("band3_capture_file_maps_test.cap");
    REQUIRE(SaveCapture(path, fc));
    auto back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);
    REQUIRE(back->shades.size() == 1);
    const ShadeState& b = back->shades[0];
    REQUIRE(back->draws[0].tex);
    CHECK(back->draws[0].tex->width == 512);
    CHECK(b.maps[kMapSpecular] == back->draws[0].tex);
    REQUIRE(b.maps[kMapNormal]);
    CHECK(b.maps[kMapNormal]->width == 2048);
    CHECK(b.maps[kMapNormal]->height == 1);
    REQUIRE(b.maps[kMapGlow]);
    CHECK(b.maps[kMapGlow]->width == 2048);
    CHECK(b.maps[kMapGlow]->rgba == s.maps[kMapGlow]->rgba);
}

TEST_CASE("a capture keeps a movie's planes whole, other textures smaller") {
    // the intro's: Y 1280x720 as the diffuse texture, cR and cB 640x360 as
    // the specular and emissive maps; a texture as big on another draw
    FrameCapture fc;
    auto geom = MakeTriangle();
    ShadeState s;
    std::memset(static_cast<ShadeInputs*>(&s), 0, sizeof(ShadeInputs));
    s.shader_type = kMovieShader;
    s.maps[kMapSpecular] = MakeTexture(640, 360, 2, 7);
    s.maps[kMapGlow] = MakeTexture(640, 360, 2, 8);
    fc.shades = {s};
    fc.draws.push_back(MakeDraw(geom, 0));
    fc.draws.back().rect_shader = kMovieShader;
    fc.draws.back().tex = MakeTexture(1280, 720, 2, 5);
    fc.draws.push_back(MakeDraw(geom, -1));
    fc.draws.back().tex = MakeTexture(4096, 8, 2, 6);

    const std::string path = TempPath("band3_capture_file_movie_test.cap");
    REQUIRE(SaveCapture(path, fc));
    auto back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);
    REQUIRE(back->draws.size() == 2);
    REQUIRE(back->draws[0].tex);
    CHECK(back->draws[0].tex->width == 1280);
    CHECK(back->draws[0].tex->height == 720);
    CHECK(back->draws[0].tex->rgba == fc.draws[0].tex->rgba);
    REQUIRE(back->shades.size() == 1);
    for (int m : {kMapSpecular, kMapGlow}) {
        REQUIRE(back->shades[0].maps[m]);
        CHECK(back->shades[0].maps[m]->width == 640);
        CHECK(back->shades[0].maps[m]->height == 360);
    }
    REQUIRE(back->draws[1].tex);
    CHECK(back->draws[1].tex->width == 2048);
    CHECK(back->draws[1].tex->height == 4);
}

TEST_CASE("a capture from before shades still loads") {
    // B3CAP001: frame, geometry, textures, then the draws without a shade
    std::vector<uint8_t> out;
    auto raw = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };
    auto put32 = [&](uint32_t v) { raw(&v, 4); };
    raw("B3CAP001", 8);
    const uint64_t frame = 7;
    raw(&frame, 8);
    const auto geom = MakeTriangle();
    put32(1);
    put32(3);
    PutOldVerts(out, *geom);
    put32(3);
    raw(geom->indices.data(), 3 * sizeof(uint16_t));
    put32(0);  // textures
    put32(1);  // draws
    put32(0);  // geometry
    put32(0xffffffffu);  // no texture
    const Mat4 identity{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    raw(&identity, sizeof(identity));
    raw(&identity, sizeof(identity));
    put32(0);  // bones
    const float color[4] = {1, 1, 1, 1};
    raw(color, sizeof(color));
    put32(1);  // blend
    put32(1);  // z mode
    out.push_back(0);  // prelit
    out.push_back(0);  // alpha cut
    put32(0);  // alpha threshold
    put32(0xabcd);  // camera
    put32(0x5678);  // mesh

    const std::string path = TempPath("band3_capture_file_v1_test.cap");
    {
        std::ofstream f(path, std::ios::binary);
        REQUIRE(f);
        f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    }
    auto back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);
    CHECK(back->frame == 7);
    REQUIRE(back->draws.size() == 1);
    CHECK(back->draws[0].shade == -1);
    CHECK(back->draws[0].mesh == 0x5678);
    CHECK(back->draws[0].cam == 0xabcd);
    CHECK(back->shades.empty());
    REQUIRE(back->draws[0].geom->verts.size() == 3);
    CHECK(back->draws[0].geom->verts[2].pos[2] == 1.0f);
    CHECK_FALSE(back->draws[0].geom->tangents);
}

namespace {

std::vector<uint8_t> ReadAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

void WriteAll(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

// where B3CAP003's section `id` starts (its header), or npos
size_t FindSection(const std::vector<uint8_t>& data, const char id[4]) {
    size_t pos = 8;
    while (pos + 16 <= data.size()) {
        uint64_t size;
        std::memcpy(&size, data.data() + pos + 8, 8);
        if (std::memcmp(data.data() + pos, id, 4) == 0) return pos;
        pos += 16 + size_t(size);
    }
    return std::string::npos;
}

// FRAM's counts after rt_filtered (later_passes, skipped_no_mat,
// faces_elsewhere), which the builds the tests below stand in for didn't
// write either
constexpr uint32_t kCountsAfterFiltered = 3;

// FRAM without the clear colour at its end (whether it has one, then four
// floats), as builds before it wrote it
void DropClearColor(std::vector<uint8_t>& data) {
    const size_t fram = FindSection(data, "FRAM");
    REQUIRE(fram != std::string::npos);
    uint64_t size;
    std::memcpy(&size, data.data() + fram + 8, 8);
    const size_t end = fram + 16 + size_t(size), cut = 4 + 16;
    data.erase(data.begin() + std::ptrdiff_t(end - cut), data.begin() + std::ptrdiff_t(end));
    size -= cut;
    std::memcpy(data.data() + fram + 8, &size, 8);
}

// a frame with an outfit composite carried in from the menu, the crowd's
// impostor drawn, and the back buffer sampling two of its versions
FrameCapture MakePassFrame() {
    FrameCapture fc;
    fc.frame = 9;
    fc.game_frame = 2400;
    fc.post_boundary = 6;
    fc.proc_cmds = 7;
    fc.rt_sampled = 3;
    fc.rt_missing = 1;
    fc.rt_filtered = 1;
    fc.rt_filtered_keys = {uint64_t(0x20D00000) << 32 | 12};
    fc.passes_carried = 1;
    fc.passes_own = 2;
    fc.later_passes = 4;
    fc.skipped_no_mat = 5;
    fc.faces_elsewhere = 6;
    auto geom = MakeTriangle();
    ShadeState s;
    std::memset(static_cast<ShadeInputs*>(&s), 0, sizeof(ShadeInputs));
    s.shader_type = 6;
    fc.shades = {s};

    auto composite = std::make_shared<Texture>();
    composite->width = 1024;
    composite->height = 512;
    composite->tex_obj = 0x20EF6128;
    composite->tex_type = 0x22;
    composite->version = 1;
    // two versions of the impostor with the same (guest) pixels
    auto impostor1 = MakeTexture(4, 8, 6, 77);
    impostor1->tex_obj = 0x20D00000;
    impostor1->tex_type = 0x2;
    impostor1->version = 5;
    auto impostor2 = std::make_shared<Texture>(*impostor1);
    impostor2->version = 6;

    auto rect = [&](uint32_t target, int32_t shader, int32_t mip) {
        DrawItem d = MakeDraw(geom, 0);
        d.target = target;
        d.rect_shader = shader;
        d.mip_level = mip;
        d.rect[2] = 512;
        d.rect[3] = 256;
        return d;
    };
    fc.draws.push_back(rect(0x20EF6128, 6, 0));  // the composite's fill, carried
    fc.draws.push_back(MakeDraw(geom, -1));        // the impostor's character
    fc.draws.back().target = 0x20D00000;
    fc.draws.push_back(rect(0x20D00000, 3, 1));    // its mip
    fc.draws.push_back(MakeDraw(geom, 0));         // a billboard of it
    fc.draws.back().tex = impostor1;
    fc.draws.push_back(MakeDraw(geom, 0));         // the band member's body
    fc.draws.back().tex = composite;
    fc.draws.push_back(MakeDraw(geom, 0));         // the next type's billboard
    fc.draws.back().tex = impostor2;
    fc.draws.push_back(rect(0, 16, 0));            // the post copy

    Pass carried;
    carried.tex_obj = 0x20EF6128;
    carried.first_draw = 0;
    carried.draw_count = 1;
    carried.width = 1024;
    carried.height = 512;
    carried.tex_type = 0x22;
    carried.format = 0x18280186;
    carried.clear_flags = 0x0f;
    carried.viewport[2] = 1024;
    carried.viewport[3] = 512;
    carried.cam = 0x241B91C0;
    carried.version = 1;
    carried.from_frame = 995;
    carried.name = "torso_skin_diffuse_output.tex";
    Pass impostor;
    impostor.tex_obj = 0x20D00000;
    impostor.first_draw = 1;
    impostor.draw_count = 2;
    impostor.width = 256;
    impostor.height = 512;
    impostor.tex_type = 0x2;
    impostor.num_mips = 4;
    impostor.clear_flags = 0x3f;
    impostor.version = 5;
    impostor.from_frame = 2400;
    Pass back;
    back.first_draw = 3;
    back.draw_count = 4;
    back.from_frame = 2400;
    fc.passes = {carried, impostor, back};
    return fc;
}

}  // namespace

TEST_CASE("a capture keeps its passes and which render target version each draw samples") {
    const FrameCapture fc = MakePassFrame();
    const std::string path = TempPath("band3_capture_file_passes_test.cap");
    REQUIRE(SaveCapture(path, fc));
    auto back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);

    CHECK(back->frame == 9);
    CHECK(back->game_frame == 2400);
    CHECK(back->post_boundary == 6);
    CHECK(back->proc_cmds == 7);
    CHECK(back->rt_sampled == 3);
    CHECK(back->rt_missing == 1);
    CHECK(back->rt_filtered == 1);
    CHECK(back->rt_filtered_keys == std::vector<uint64_t>{uint64_t(0x20D00000) << 32 | 12});
    CHECK(back->passes_carried == 1);
    CHECK(back->passes_own == 2);
    CHECK(back->later_passes == 4);
    CHECK(back->skipped_no_mat == 5);
    CHECK(back->faces_elsewhere == 6);

    REQUIRE(back->passes.size() == 3);
    const Pass& c = back->passes[0];
    CHECK(c.tex_obj == 0x20EF6128);
    CHECK(c.draw_count == 1);
    CHECK(c.width == 1024);
    CHECK(c.tex_type == 0x22);
    CHECK(c.format == 0x18280186);
    CHECK(c.clear_flags == 0x0f);
    CHECK(c.viewport[2] == 1024);
    CHECK(c.cam == 0x241B91C0);
    CHECK(c.version == 1);
    CHECK(c.from_frame == 995);
    CHECK(c.name == "torso_skin_diffuse_output.tex");
    CHECK(back->passes[1].num_mips == 4);
    CHECK(back->passes[1].first_draw == 1);
    CHECK(back->passes[2].tex_obj == 0);
    CHECK(back->passes[2].draw_count == 4);
    CHECK(back->passes[2].name.empty());

    REQUIRE(back->draws.size() == 7);
    CHECK(back->draws[0].rect_shader == 6);
    CHECK(back->draws[0].target == 0x20EF6128);
    CHECK(back->draws[0].rect[2] == 512);
    CHECK(back->draws[2].mip_level == 1);
    CHECK(back->draws[3].rect_shader == -1);
    CHECK_FALSE(DrawnToBackBuffer(back->draws[0]));
    CHECK_FALSE(DrawnToBackBuffer(back->draws[1]));
    CHECK(DrawnToBackBuffer(back->draws[3]));
    CHECK_FALSE(DrawnToBackBuffer(back->draws[6]));

    // the composite kept by identity alone, at its own size
    const auto& body = back->draws[4].tex;
    REQUIRE(body);
    CHECK(body->rgba.empty());
    CHECK(body->tex_obj == 0x20EF6128);
    CHECK(body->version == 1);
    CHECK(body->width == 1024);
    // the impostor's two versions stay two, with the same pixels
    const auto& v5 = back->draws[3].tex;
    const auto& v6 = back->draws[5].tex;
    REQUIRE(v5);
    REQUIRE(v6);
    CHECK(v5 != v6);
    CHECK(v5->version == 5);
    CHECK(v6->version == 6);
    CHECK(v6->tex_type == 0x2);
    CHECK(v5->rgba == v6->rgba);
    CHECK(v6->rgba.size() == 32);
}

TEST_CASE("a capture loads without a section a newer build wrote, unless it's one it needs") {
    const FrameCapture fc = MakePassFrame();
    const std::string path = TempPath("band3_capture_file_sections_test.cap");
    REQUIRE(SaveCapture(path, fc));
    const std::vector<uint8_t> data = ReadAll(path);

    // a section this build doesn't know, at the end, is skipped
    std::vector<uint8_t> more = data;
    const uint8_t unknown[20] = {'N', 'E', 'W', 'S', 1, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0,
                                 1, 2, 3, 4};
    more.insert(more.end(), unknown, unknown + sizeof(unknown));
    WriteAll(path, more);
    auto back = LoadCapture(path);
    REQUIRE(back);
    CHECK(back->draws.size() == 7);
    CHECK(back->passes.size() == 3);

    // shades of a newer version: the draws load without them
    std::vector<uint8_t> newer = data;
    const size_t shad = FindSection(newer, "SHAD");
    REQUIRE(shad != std::string::npos);
    newer[shad + 4] = 99;
    WriteAll(path, newer);
    back = LoadCapture(path);
    REQUIRE(back);
    CHECK(back->shades.empty());
    CHECK(back->draws[0].shade == -1);
    CHECK(back->draws.size() == 7);

    // draws of a newer version: not loadable
    std::vector<uint8_t> draws = data;
    const size_t at = FindSection(draws, "DRAW");
    REQUIRE(at != std::string::npos);
    draws[at + 4] = 99;
    WriteAll(path, draws);
    CHECK_FALSE(LoadCapture(path));

    // cut short: not loadable
    WriteAll(path, std::vector<uint8_t>(data.begin(), data.end() - 9));
    CHECK_FALSE(LoadCapture(path));
    std::remove(path.c_str());
}

TEST_CASE("a capture says whose world it has, and one from before that says its own") {
    FrameCapture fc = MakePassFrame();
    fc.proc_cmds = 2;
    fc.composed = 1;
    fc.world_frame = 2399;
    const std::string path = TempPath("band3_capture_file_composed_test.cap");
    REQUIRE(SaveCapture(path, fc));
    auto back = LoadCapture(path);
    REQUIRE(back);
    CHECK(back->proc_cmds == 2);
    CHECK(back->composed == 1);
    CHECK(back->world_frame == 2399);

    // FRAM as builds before composition wrote it: two counts fewer (composed
    // and rt_filtered), and no world frame, filtered render targets (one) or
    // clear colour after them
    std::vector<uint8_t> data = ReadAll(path);
    DropClearColor(data);
    const size_t fram = FindSection(data, "FRAM");
    REQUIRE(fram != std::string::npos);
    uint64_t size;
    std::memcpy(&size, data.data() + fram + 8, 8);
    const size_t counts_at = fram + 16 + 8 + 8 + 4 + 4;
    uint32_t counts;
    std::memcpy(&counts, data.data() + counts_at, 4);
    counts -= 2 + kCountsAfterFiltered;
    std::memcpy(data.data() + counts_at, &counts, 4);
    const size_t end = fram + 16 + size_t(size);
    const size_t cut = 4 + 4 + 4 * kCountsAfterFiltered + 8 + 4 + 8;
    data.erase(data.begin() + std::ptrdiff_t(end - cut), data.begin() + std::ptrdiff_t(end));
    size -= cut;
    std::memcpy(data.data() + fram + 8, &size, 8);
    WriteAll(path, data);
    back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);
    CHECK(back->composed == 0);
    CHECK(back->world_frame == 2400);
    CHECK(back->rt_missing == 1);
    CHECK(back->rt_filtered == 0);
    CHECK(back->rt_filtered_keys.empty());
    CHECK(back->later_passes == 0);
    CHECK(back->faces_elsewhere == 0);
    CHECK(back->draws.size() == 7);
}

TEST_CASE("a capture from before filtered render targets has none") {
    FrameCapture fc = MakePassFrame();
    fc.composed = 1;
    fc.world_frame = 2399;
    const std::string path = TempPath("band3_capture_file_filtered_test.cap");
    REQUIRE(SaveCapture(path, fc));

    // FRAM as builds before them wrote it: a count fewer (rt_filtered), and
    // nothing after the world frame
    std::vector<uint8_t> data = ReadAll(path);
    DropClearColor(data);
    const size_t fram = FindSection(data, "FRAM");
    REQUIRE(fram != std::string::npos);
    uint64_t size;
    std::memcpy(&size, data.data() + fram + 8, 8);
    const size_t counts_at = fram + 16 + 8 + 8 + 4 + 4;
    uint32_t counts;
    std::memcpy(&counts, data.data() + counts_at, 4);
    counts -= 1 + kCountsAfterFiltered;
    std::memcpy(data.data() + counts_at, &counts, 4);
    const size_t end = fram + 16 + size_t(size);
    // the keys (a count and one), then the rt_filtered count and those after
    // it before the world frame
    data.erase(data.begin() + std::ptrdiff_t(end - 12), data.begin() + std::ptrdiff_t(end));
    const size_t count_at = end - 12 - 8 - 4 - 4 * kCountsAfterFiltered;
    data.erase(data.begin() + std::ptrdiff_t(count_at),
               data.begin() + std::ptrdiff_t(count_at + 4 + 4 * kCountsAfterFiltered));
    size -= 16 + 4 * kCountsAfterFiltered;
    std::memcpy(data.data() + fram + 8, &size, 8);
    WriteAll(path, data);
    auto back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);
    CHECK(back->composed == 1);
    CHECK(back->world_frame == 2399);
    CHECK(back->rt_missing == 1);
    CHECK(back->rt_filtered == 0);
    CHECK(back->rt_filtered_keys.empty());
    CHECK(back->later_passes == 0);
    CHECK(back->faces_elsewhere == 0);
}

TEST_CASE("a capture's geometry loads from a build whose Vertex was smaller") {
    const FrameCapture fc = MakePassFrame();
    const std::string path = TempPath("band3_capture_file_stride_test.cap");
    REQUIRE(SaveCapture(path, fc));
    const std::vector<uint8_t> data = ReadAll(path);
    // GEOM written again as version 1, with each vertex cut to its first 32
    // bytes (pos, nrm, uv), as an older Vertex would have been, and without
    // each geometry's tangents flag
    const size_t geom = FindSection(data, "GEOM");
    REQUIRE(geom != std::string::npos);
    uint64_t size;
    std::memcpy(&size, data.data() + geom + 8, 8);
    const uint8_t* in = data.data() + geom + 16;
    std::vector<uint8_t> sec;
    auto put32 = [&](uint32_t v) {
        const auto* b = reinterpret_cast<const uint8_t*>(&v);
        sec.insert(sec.end(), b, b + 4);
    };
    constexpr uint32_t kOld = 32;
    put32(kOld);
    uint32_t count;
    std::memcpy(&count, in + 4, 4);
    put32(count);
    size_t pos = 8;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t nv;
        std::memcpy(&nv, in + pos, 4);
        pos += 4;
        put32(nv);
        for (uint32_t v = 0; v < nv; v++, pos += sizeof(Vertex))
            sec.insert(sec.end(), in + pos, in + pos + kOld);
        uint32_t ni;
        std::memcpy(&ni, in + pos, 4);
        pos += 4;
        put32(ni);
        sec.insert(sec.end(), in + pos, in + pos + ni * 2);
        pos += ni * 2;
        pos += 1;  // tangents
    }
    REQUIRE(pos == size);
    std::vector<uint8_t> out(data.begin(), data.begin() + geom + 8);
    out[geom + 4] = 1;  // the version: before tangents
    const uint64_t new_size = sec.size();
    const auto* ns = reinterpret_cast<const uint8_t*>(&new_size);
    out.insert(out.end(), ns, ns + 8);
    out.insert(out.end(), sec.begin(), sec.end());
    out.insert(out.end(), data.begin() + geom + 16 + size_t(size), data.end());
    WriteAll(path, out);
    auto back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);
    const Geometry& g = *back->draws[0].geom;
    REQUIRE(g.verts.size() == 3);
    CHECK(g.verts[1].pos[1] == 1.0f);
    CHECK(g.verts[1].color == 0);  // past the old size: zero
    CHECK(g.indices.size() == 3);
    CHECK_FALSE(g.tangents);
}

TEST_CASE("a capture keeps its geometry's tangents and whether it has them") {
    FrameCapture fc;
    auto with = MakeTriangle();
    with->tangents = true;
    for (int i = 0; i < 3; i++) {
        with->verts[i].tan[0] = 0.25f * float(i);
        with->verts[i].tan[1] = 0.5f;
        with->verts[i].tan[3] = i == 1 ? -1.0f : 1.0f;
    }
    fc.draws.push_back(MakeDraw(with, -1));
    fc.draws.push_back(MakeDraw(MakeTriangle(), -1));  // band3's own: none
    const std::string path = TempPath("band3_capture_file_tangents_test.cap");
    REQUIRE(SaveCapture(path, fc));
    auto back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);
    REQUIRE(back->draws.size() == 2);
    const Geometry& a = *back->draws[0].geom;
    CHECK(a.tangents);
    CHECK(a.verts[2].tan[0] == 0.5f);
    CHECK(a.verts[2].tan[1] == 0.5f);
    CHECK(a.verts[1].tan[3] == -1.0f);
    CHECK(a.verts[0].tan[3] == 1.0f);
    CHECK_FALSE(back->draws[1].geom->tangents);
}

TEST_CASE("a capture from before passes (B3CAP002) still loads, with its shades") {
    std::vector<uint8_t> out;
    auto raw = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        out.insert(out.end(), b, b + n);
    };
    auto put32 = [&](uint32_t v) { raw(&v, 4); };
    raw("B3CAP002", 8);
    const uint64_t frame = 11;
    raw(&frame, 8);
    const auto geom = MakeTriangle();
    put32(1);
    put32(3);
    PutOldVerts(out, *geom);
    put32(3);
    raw(geom->indices.data(), 3 * sizeof(uint16_t));
    const auto tex = MakeTexture(2, 1, 18, 5);
    put32(1);  // textures
    put32(tex->width);
    put32(tex->height);
    put32(tex->format);
    raw(tex->rgba.data(), tex->rgba.size() * 4);
    put32(uint32_t(sizeof(ShadeInputs)));
    put32(uint32_t(kNumShadeMaps));
    put32(1);  // shades
    ShadeInputs in;
    std::memset(&in, 0, sizeof(in));
    in.shader_type = 18;
    in.ps[ShadeRegIndex(124)][2] = 3.5f;
    raw(&in, sizeof(in));
    for (int m = 0; m < kNumShadeMaps; m++) put32(m == kMapGlow ? 0 : 0xffffffffu);
    put32(1);  // draws
    put32(0);  // geometry
    put32(0);  // texture
    const Mat4 identity{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
    raw(&identity, sizeof(identity));
    raw(&identity, sizeof(identity));
    put32(0);  // bones
    const float color[4] = {1, 1, 1, 1};
    raw(color, sizeof(color));
    put32(1);  // blend
    put32(1);  // z mode
    out.push_back(0);  // prelit
    out.push_back(0);  // alpha cut
    put32(0);  // alpha threshold
    put32(0xabcd);  // camera
    put32(0x5678);  // mesh
    put32(0);  // shade

    const std::string path = TempPath("band3_capture_file_v2_test.cap");
    WriteAll(path, out);
    auto back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);
    CHECK(back->frame == 11);
    CHECK(back->passes.empty());
    CHECK(back->post_boundary == FrameCapture::kNoPost);
    REQUIRE(back->draws.size() == 1);
    CHECK(DrawnToBackBuffer(back->draws[0]));
    CHECK(back->draws[0].shade == 0);
    REQUIRE(back->draws[0].tex);
    CHECK(back->draws[0].tex->rgba == tex->rgba);
    CHECK(back->draws[0].tex->tex_obj == 0);
    REQUIRE(back->shades.size() == 1);
    CHECK(back->shades[0].Ps(124)[2] == 3.5f);
    CHECK(back->shades[0].maps[kMapGlow] == back->draws[0].tex);
}

TEST_CASE("a capture keeps its post-processing, and one from before has none") {
    FrameCapture fc = MakePassFrame();
    fc.post.valid = 1;
    fc.post.proc = 0x31F00000;
    fc.post.xfm[1][2] = 0.25f;
    fc.post.saturation = -60;
    fc.post.bloom_intensity = 1.5f;
    fc.post.dof_enabled = 1;
    fc.post.dof_focal = 210.5f;
    fc.post.cam_far = 5000;
    fc.post_consts.valid = 1;
    fc.post_consts.c24[1] = -3.5f;
    fc.post_consts.flags[kPostFlagColorXfm] = 1;
    fc.post_consts.dof_survey = 1;
    fc.post_consts.dof_offsets[7][1] = 0.009f;
    fc.shades[0].alpha_write = 1;
    const std::string path = TempPath("band3_capture_file_post_test.cap");
    REQUIRE(SaveCapture(path, fc));
    auto back = LoadCapture(path);
    REQUIRE(back);
    CHECK(std::memcmp(&back->post, &fc.post, sizeof(PostParams)) == 0);
    CHECK(std::memcmp(&back->post_consts, &fc.post_consts, sizeof(PostConsts)) == 0);
    CHECK(back->shades[0].alpha_write == 1);

    // without the section, as builds before it wrote: nothing read
    std::vector<uint8_t> data = ReadAll(path);
    const size_t post = FindSection(data, "POST");
    REQUIRE(post != std::string::npos);
    uint64_t size;
    std::memcpy(&size, data.data() + post + 8, 8);
    data.erase(data.begin() + std::ptrdiff_t(post),
               data.begin() + std::ptrdiff_t(post + 16 + size_t(size)));
    WriteAll(path, data);
    back = LoadCapture(path);
    REQUIRE(back);
    CHECK(back->post.valid == 0);
    CHECK(back->post.color_mod == 1.0f);
    CHECK(back->post_consts.valid == 0);
    CHECK(back->draws.size() == fc.draws.size());

    // from a build whose PostParams was smaller: the fields both have
    REQUIRE(SaveCapture(path, fc));
    data = ReadAll(path);
    const size_t at = FindSection(data, "POST");
    REQUIRE(at != std::string::npos);
    std::memcpy(&size, data.data() + at + 8, 8);
    const uint32_t shorter = uint32_t(offsetof(PostParams, dof));
    std::memcpy(data.data() + at + 16, &shorter, 4);
    const size_t cut = sizeof(PostParams) - shorter;
    data.erase(data.begin() + std::ptrdiff_t(at + 20 + shorter),
               data.begin() + std::ptrdiff_t(at + 20 + sizeof(PostParams)));
    size -= cut;
    std::memcpy(data.data() + at + 8, &size, 8);
    WriteAll(path, data);
    back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);
    CHECK(back->post.saturation == -60.0f);
    CHECK(back->post.bloom_intensity == 1.5f);
    CHECK(back->post.dof_focal == 0.0f);
    CHECK(back->post.cam_far == 0.0f);
    CHECK(back->post_consts.c24[1] == -3.5f);
}

TEST_CASE("a capture keeps each draw's cull and draw modes; one from before has none") {
    FrameCapture fc = MakePassFrame();
    fc.draws[3].cull = kCullBack;                   // D3DCULL_CW, RndMat's cull
    fc.draws[4].cull = kCullBack | kCullFrontIsCw;  // D3DCULL_CCW, a reflection's
    fc.draws[1].draw_mode = kDrawModeShadowDepth;
    fc.draws[2].draw_mode = kDrawModeShadowCasters;
    const std::string path = TempPath("band3_capture_file_cull_test.cap");
    REQUIRE(SaveCapture(path, fc));
    auto back = LoadCapture(path);
    REQUIRE(back);
    REQUIRE(back->draws.size() == fc.draws.size());
    for (size_t i = 0; i < fc.draws.size(); i++) {
        CHECK(back->draws[i].cull == fc.draws[i].cull);
        CHECK(back->draws[i].draw_mode == fc.draws[i].draw_mode);
    }

    // DRAW as older builds wrote it: version 2 without each draw's draw mode
    // at its end, version 1 without its cull mode before that either
    const std::vector<uint8_t> saved = ReadAll(path);
    for (uint8_t version : {uint8_t(2), uint8_t(1)}) {
        CAPTURE(int(version));
        std::vector<uint8_t> data = saved;
        const size_t at = FindSection(data, "DRAW");
        REQUIRE(at != std::string::npos);
        data[at + 4] = version;
        const size_t cut = version == 2 ? 1 : 2;
        uint64_t size;
        std::memcpy(&size, data.data() + at + 8, 8);
        size_t pos = at + 16 + 4;  // past the draw count
        for (size_t i = 0; i < fc.draws.size(); i++) {
            uint32_t bones;
            std::memcpy(&bones, data.data() + pos + 4 + 4 + 2 * sizeof(Mat4), 4);
            pos += 4 + 4 + 2 * sizeof(Mat4) + 4 + bones * sizeof(Mat4) + 16 + 4 + 4 + 1 + 1 + 4 +
                   4 + 4 + 4 + 4 + 4 + 16 + 4 + 2 - cut;
            // its draw mode, and its cull mode
            data.erase(data.begin() + std::ptrdiff_t(pos),
                       data.begin() + std::ptrdiff_t(pos + cut));
            size -= cut;
        }
        CHECK(pos == at + 16 + size_t(size));
        std::memcpy(data.data() + at + 8, &size, 8);
        WriteAll(path, data);
        back = LoadCapture(path);
        REQUIRE(back);
        REQUIRE(back->draws.size() == fc.draws.size());
        for (size_t i = 0; i < fc.draws.size(); i++) {
            CHECK(back->draws[i].cull == (version == 2 ? fc.draws[i].cull : 0));
            CHECK(back->draws[i].draw_mode == 0);
        }
        CHECK(back->draws[4].tex);
        CHECK(back->passes.size() == 3);
    }
    std::remove(path.c_str());
}

TEST_CASE("a shade's s5 as a render target keeps its identity and version, without pixels") {
    // a SHADOW_BUFFER draw's shadow map, as scene_capture.cpp keeps it: the
    // texture RB3 draws, the version it read
    FrameCapture fc = MakePassFrame();
    auto map = std::make_shared<Texture>();
    map->width = map->height = 512;
    map->tex_obj = 0x2251A0C0;
    map->tex_type = kTexTypeShadowMap;
    map->version = 41;
    fc.shades[0].options = 1ull << shader_opt::kShadowBuffer;
    fc.shades[0].maps[kMapProjected] = map;
    const std::string path = TempPath("band3_capture_file_shadow_map_test.cap");
    REQUIRE(SaveCapture(path, fc));
    auto back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);
    REQUIRE(back->shades.size() == 1);
    const Texture* got = ShadowMapOf(&back->shades[0]);
    REQUIRE(got);
    CHECK(got->tex_obj == 0x2251A0C0);
    CHECK(got->version == 41);
    CHECK(got->width == 512);
    CHECK(got->rgba.empty());
}

TEST_CASE("a capture keeps its display gamma ramp, and one from before has none") {
    FrameCapture fc = MakePassFrame();
    fc.gamma.mode = GammaRamp::kTable;
    for (uint32_t v = 0; v < 256; v++) {
        const uint32_t ten = v * 1023 / 255;
        fc.gamma.table[v] = ten << 20 | (ten / 2) << 10 | v;
    }
    fc.gamma.pwl[127][2] = 0x0400FBC0;
    const std::string path = TempPath("band3_capture_file_gamma_test.cap");
    REQUIRE(SaveCapture(path, fc));
    auto back = LoadCapture(path);
    REQUIRE(back);
    CHECK(back->gamma == fc.gamma);

    // without the section, as builds before it wrote: none, which draws the
    // picture as it is
    std::vector<uint8_t> data = ReadAll(path);
    const size_t at = FindSection(data, "GAMA");
    REQUIRE(at != std::string::npos);
    uint64_t size;
    std::memcpy(&size, data.data() + at + 8, 8);
    data.erase(data.begin() + std::ptrdiff_t(at),
               data.begin() + std::ptrdiff_t(at + 16 + size_t(size)));
    WriteAll(path, data);
    back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);
    CHECK(back->gamma.mode == GammaRamp::kNone);
    CHECK(back->gamma == GammaRamp{});
    CHECK(back->draws.size() == fc.draws.size());
    std::vector<uint32_t> rgba = {0xff102030u, 0x80fefdfcu};
    const std::vector<uint32_t> before = rgba;
    ApplyGamma(back->gamma, rgba);
    CHECK(rgba == before);
}

TEST_CASE("a capture keeps its textures' mips and its shades' samplers") {
    FrameCapture fc;
    auto geom = MakeTriangle();
    // 4096x4 with its chain: kept at 2048, from its first mip, with the rest
    auto diffuse = MakeTexture(4096, 4, 6, 1);
    for (uint32_t l = 1; l <= 12; l++) {
        const uint32_t w = std::max(4096u >> l, 1u), h = std::max(4u >> l, 1u);
        diffuse->mips.emplace_back(size_t(w) * h, 0x1000u * l);
    }
    ShadeState s;
    std::memset(static_cast<ShadeInputs*>(&s), 0, sizeof(ShadeInputs));
    // the same pixels and mips again, as another object
    auto again = std::make_shared<Texture>(*diffuse);
    s.maps[kMapSpecular] = again;
    s.diffuse_sampler.filtered = 1;
    s.diffuse_sampler.mag_linear = s.diffuse_sampler.min_linear = 1;
    s.diffuse_sampler.mip = 1;
    s.diffuse_sampler.mip_max = 10;
    s.diffuse_sampler.lod_bias = -0.25f;
    s.samplers[kMapSpecular].filtered = 1;
    s.samplers[kMapSpecular].clamp_x = 2;
    s.samplers[kMapSpecular].aniso = 4;
    fc.shades = {s};
    fc.draws.push_back(MakeDraw(geom, 0));
    fc.draws.back().tex = diffuse;

    const std::string path = TempPath("band3_capture_file_mips_test.cap");
    REQUIRE(SaveCapture(path, fc));
    auto back = LoadCapture(path);
    REQUIRE(back);
    const Texture& t = *back->draws[0].tex;
    CHECK(t.width == 2048);
    CHECK(t.height == 2);
    CHECK(t.rgba == diffuse->mips[0]);
    REQUIRE(t.mips.size() == 11);
    CHECK(t.mips[0] == diffuse->mips[1]);
    CHECK(t.mips[10] == diffuse->mips[11]);
    // the map with the same pixels and mips is kept once with them
    REQUIRE(back->shades.size() == 1);
    const ShadeState& b = back->shades[0];
    REQUIRE(b.maps[kMapSpecular]);
    CHECK(!b.maps[kMapSpecular]->mips.empty());
    CHECK(b.diffuse_sampler.filtered == 1);
    CHECK(b.diffuse_sampler.mip == 1);
    CHECK(b.diffuse_sampler.mip_max == 10);
    CHECK(b.diffuse_sampler.lod_bias == -0.25f);
    CHECK(b.samplers[kMapSpecular].clamp_x == 2);
    CHECK(b.samplers[kMapSpecular].aniso == 4);
    CHECK(b.samplers[kMapNormal].filtered == 0);

    // a file without the two sections (as from before them) loads with no
    // mips and the old nearest sampler
    std::vector<uint8_t> data = ReadAll(path);
    for (const char* id : {"MIPS", "SMPL"}) {
        const size_t at = FindSection(data, id);
        REQUIRE(at != std::string::npos);
        data[at] = 'X';  // a section this build doesn't know: skipped
    }
    WriteAll(path, data);
    back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);
    CHECK(back->draws[0].tex->mips.empty());
    CHECK(back->draws[0].tex->width == 2048);
    CHECK(back->shades[0].diffuse_sampler.filtered == 0);
}

TEST_CASE("a capture keeps its clear colour and cameras; one from before has neither") {
    FrameCapture fc = MakePassFrame();
    fc.has_clear_color = 1;
    const float black[4] = {0, 0, 0, 1};
    std::copy(std::begin(black), std::end(black), fc.clear_color);
    CameraView venue, track;
    venue.cam = 0x24F5A140;
    venue.viewport[2] = 1280;
    venue.viewport[3] = 720;
    venue.target_w = 1280;
    venue.target_h = 720;
    track.cam = 0x24F5B2C0;
    // a two-player track camera's (+0.22, 0, 1, 1), as DxCam::SetViewport clamps it
    track.viewport[0] = 281;
    track.viewport[2] = 998;
    track.viewport[3] = 720;
    track.target_w = 1280;
    track.target_h = 720;
    track.zrange[0] = 0.0f;
    track.zrange[1] = 0.1f;
    fc.cameras = {venue, track};
    const std::string path = TempPath("band3_capture_file_cameras_test.cap");
    REQUIRE(SaveCapture(path, fc));
    auto back = LoadCapture(path);
    REQUIRE(back);
    CHECK(back->has_clear_color == 1);
    CHECK(std::equal(std::begin(black), std::end(black), back->clear_color));
    REQUIRE(back->cameras.size() == 2);
    CHECK(std::memcmp(&back->cameras[0], &venue, sizeof(CameraView)) == 0);
    CHECK(std::memcmp(&back->cameras[1], &track, sizeof(CameraView)) == 0);
    CHECK(CameraOf(*back, 0x24F5B2C0) == &back->cameras[1]);
    CHECK(back->rt_filtered_keys == fc.rt_filtered_keys);

    // CAMS from a build whose CameraView was smaller: the fields both have,
    // the rest as a CameraView starts (z range 0..1)
    std::vector<uint8_t> data = ReadAll(path);
    size_t at = FindSection(data, "CAMS");
    REQUIRE(at != std::string::npos);
    {
        std::vector<uint8_t> small(data.begin(), data.begin() + std::ptrdiff_t(at + 16));
        const uint32_t each = offsetof(CameraView, zrange), count = 2;
        small.insert(small.end(), reinterpret_cast<const uint8_t*>(&each),
                     reinterpret_cast<const uint8_t*>(&each) + 4);
        small.insert(small.end(), reinterpret_cast<const uint8_t*>(&count),
                     reinterpret_cast<const uint8_t*>(&count) + 4);
        for (const CameraView& c : fc.cameras) {
            const auto* b = reinterpret_cast<const uint8_t*>(&c);
            small.insert(small.end(), b, b + each);
        }
        const uint64_t size = small.size() - (at + 16);
        std::memcpy(small.data() + at + 8, &size, 8);
        uint64_t old_size;
        std::memcpy(&old_size, data.data() + at + 8, 8);
        small.insert(small.end(), data.begin() + std::ptrdiff_t(at + 16 + size_t(old_size)),
                     data.end());
        WriteAll(path, small);
        back = LoadCapture(path);
        REQUIRE(back);
        REQUIRE(back->cameras.size() == 2);
        CHECK(back->cameras[1].viewport[0] == 281);
        CHECK(back->cameras[1].target_h == 720);
        CHECK(back->cameras[1].zrange[1] == 1.0f);
    }

    // without CAMS and without the clear colour, as builds before them wrote
    // it: none of either
    uint64_t size;
    std::memcpy(&size, data.data() + at + 8, 8);
    data.erase(data.begin() + std::ptrdiff_t(at),
               data.begin() + std::ptrdiff_t(at + 16 + size_t(size)));
    DropClearColor(data);
    WriteAll(path, data);
    back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);
    CHECK(back->has_clear_color == 0);
    CHECK(back->cameras.empty());
    CHECK(back->rt_filtered_keys == fc.rt_filtered_keys);
    CHECK(back->draws.size() == fc.draws.size());
}

TEST_CASE("a capture keeps the composite's noise map whole, its mips and its sampler") {
    // noise_mono.tex as sampler 13 has it: wrapping, linear, with its chain
    FrameCapture fc;
    auto geom = MakeTriangle();
    fc.draws.push_back(MakeDraw(geom, -1));
    auto noise = MakeTexture(4096, 4, 2, 11);
    for (uint32_t l = 1; l <= 12; l++) {
        const uint32_t w = std::max(4096u >> l, 1u), h = std::max(4u >> l, 1u);
        noise->mips.emplace_back(size_t(w) * h, 0x2000u * l);
    }
    fc.noise_map = noise;
    fc.noise_sampler.filtered = 1;
    fc.noise_sampler.mag_linear = fc.noise_sampler.min_linear = 1;
    fc.noise_sampler.mip = 1;
    fc.noise_sampler.mip_max = 12;
    fc.post_consts.noise_fetch[1] = 0x1234000u;

    const std::string path = TempPath("band3_capture_file_noise_test.cap");
    REQUIRE(SaveCapture(path, fc));
    auto back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);
    REQUIRE(back->noise_map);
    CHECK(back->noise_map->width == 4096);
    CHECK(back->noise_map->rgba == noise->rgba);
    REQUIRE(back->noise_map->mips.size() == 12);
    CHECK(back->noise_map->mips[11] == noise->mips[11]);
    CHECK(back->noise_sampler.filtered == 1);
    CHECK(back->noise_sampler.mip == 1);
    CHECK(back->noise_sampler.mip_max == 12);
    CHECK(back->post_consts.noise_fetch[1] == 0x1234000u);

    // a frame without: none
    fc.noise_map = nullptr;
    REQUIRE(SaveCapture(path, fc));
    back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);
    CHECK(!back->noise_map);
}

TEST_CASE("a capture keeps the motion blur's object pass, and one from before has none") {
    FrameCapture fc = MakePassFrame();
    VelocityObject v;
    v.geom = MakeTriangle();  // a geometry no draw has
    v.mesh = 0x4321;
    v.cull = 6;
    v.skinned = 1;
    v.bones = 2;
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 4; c++) v.view_proj[r][c] = float(r * 4 + c);
    v.depth_range[1] = 10000;
    for (int i = 0; i < 2 * 2 * 3 * 4; i++) v.rows.push_back(float(i) * 0.5f);
    fc.velocity_objects.push_back(v);
    VelocityObject shared = v;
    shared.geom = fc.draws[0].geom;  // and one a draw has too
    shared.bones = 1;
    shared.skinned = 0;
    shared.rows.resize(2 * 3 * 4);
    fc.velocity_objects.push_back(shared);
    const std::string path = TempPath("band3_capture_file_velocity_test.cap");
    REQUIRE(SaveCapture(path, fc));
    auto back = LoadCapture(path);
    REQUIRE(back);
    REQUIRE(back->velocity_objects.size() == 2);
    const VelocityObject& a = back->velocity_objects[0];
    REQUIRE(a.geom);
    CHECK(a.geom->verts.size() == 3);
    CHECK(a.mesh == 0x4321);
    CHECK(a.cull == 6);
    CHECK(a.skinned == 1);
    CHECK(a.bones == 2);
    CHECK(a.view_proj[7][3] == 31.0f);
    CHECK(a.depth_range[1] == 10000.0f);
    CHECK(a.rows == v.rows);
    CHECK(back->velocity_objects[1].geom == back->draws[0].geom);
    CHECK(back->velocity_objects[1].rows.size() == 24);

    // without VOBJ (a capture from before it): none
    std::vector<uint8_t> data = ReadAll(path);
    const size_t at = FindSection(data, "VOBJ");
    REQUIRE(at != std::string::npos);
    uint64_t size;
    std::memcpy(&size, data.data() + at + 8, 8);
    data.erase(data.begin() + std::ptrdiff_t(at), data.begin() + std::ptrdiff_t(at + 16 + size));
    WriteAll(path, data);
    back = LoadCapture(path);
    std::remove(path.c_str());
    REQUIRE(back);
    CHECK(back->velocity_objects.empty());
    CHECK(back->draws.size() == fc.draws.size());
}
