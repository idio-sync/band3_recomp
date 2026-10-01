// Checks the native view's capture files (src/Render/capture_file.cpp): a frame
// with shades and their maps comes back as it was saved, and a file from
// before shades (B3CAP001) still loads, its draws without one.

#include <doctest/doctest.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

TEST_CASE("a capture keeps maps smaller, and pixels it has already once") {
    FrameCapture fc;
    auto geom = MakeTriangle();
    auto diffuse = MakeTexture(512, 2, 6, 1);
    ShadeState s;
    std::memset(static_cast<ShadeInputs*>(&s), 0, sizeof(ShadeInputs));
    // the colour texture again, as another object
    s.maps[kMapSpecular] = MakeTexture(512, 2, 6, 1);
    s.maps[kMapNormal] = MakeTexture(512, 2, 49, 9);
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
    CHECK(b.maps[kMapNormal]->width == 256);
    CHECK(b.maps[kMapNormal]->height == 1);
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
    raw(geom->verts.data(), 3 * sizeof(Vertex));
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
}
