// Checks src/Video/movie_planes.h: that a plane written into a movie's k_8
// texture (WritePlane8) decodes back, through the renderer's own decoder
// (guest_formats.h's DecodeLevel8), to the texels written, linear and tiled.

#include <doctest/doctest.h>
#include <cstdint>
#include <vector>
#include "src/Render/guest_formats.h"
#include "src/Video/movie_planes.h"

using namespace band3::render::guest_format;
using band3::video::Plane;
using band3::video::PlaneSet;

namespace {

// a k_8 2D texture's fetch constant, base at 0x10000
FetchLayout Layout(uint32_t w, uint32_t h, uint32_t pitch, bool tiled) {
    uint32_t f[6] = {};
    f[0] = uint32_t(tiled) << 31 | (pitch >> 5) << 22;
    f[1] = 2 | 0x10000;
    f[2] = (w - 1) | (h - 1) << 13;
    f[3] = 0x688u << 1;
    f[5] = 1u << 9;
    return ReadFetchLayout(f);
}

Plane Noise(uint32_t w, uint32_t h, uint32_t seed) {
    Plane p;
    p.Resize(w, h, 0);
    uint32_t r = seed;
    for (uint8_t& b : p.texels) {
        r = r * 1664525u + 1013904223u;
        b = uint8_t(r >> 24);
    }
    return p;
}

// the texture's texels as the renderer decodes them (red channel)
std::vector<uint8_t> Decode(const FetchLayout& l, const std::vector<uint8_t>& mem) {
    FormatInfo info;
    GetFormatInfo(2, info);
    std::vector<uint32_t> rgba(size_t(l.width) * l.height);
    DecodeLevel8(mem.data(), l, PlaceLevel(l, info, 0), l.width, l.height, rgba.data());
    std::vector<uint8_t> out(rgba.size());
    for (size_t i = 0; i < rgba.size(); i++) out[i] = uint8_t(rgba[i]);
    return out;
}

std::vector<uint8_t> Memory(const FetchLayout& l) {
    FormatInfo info;
    GetFormatInfo(2, info);
    return std::vector<uint8_t>(
        LevelBytes(l, info, PlaceLevel(l, info, 0), l.width, l.height), 0x5a);
}

}

TEST_CASE("a plane written into a movie's texture decodes back to it") {
    // Bink's 1280x720 Y and 640x360 chroma, and sizes off the 32-texel tiles
    struct Case {
        uint32_t w, h, pitch;
        bool tiled;
    } cases[] = {{1280, 720, 1280, false}, {640, 360, 640, false}, {1280, 720, 1280, true},
                 {640, 360, 640, true},    {200, 75, 224, false},  {200, 75, 224, true}};
    for (const Case& c : cases) {
        CAPTURE(c.w);
        CAPTURE(c.h);
        CAPTURE(c.tiled);
        const FetchLayout l = Layout(c.w, c.h, c.pitch, c.tiled);
        std::vector<uint8_t> mem = Memory(l);
        const Plane src = Noise(c.w, c.h, c.w * 31 + c.h);
        REQUIRE(band3::video::WritePlane8(l, mem.data(), src));
        CHECK(Decode(l, mem) == src.texels);
    }
}

TEST_CASE("a smaller plane fills the texture's corner and keeps the rest") {
    const FetchLayout l = Layout(64, 64, 64, true);
    std::vector<uint8_t> mem = Memory(l);
    const std::vector<uint8_t> before = Decode(l, mem);
    const Plane src = Noise(40, 24, 7);
    REQUIRE(band3::video::WritePlane8(l, mem.data(), src));
    const std::vector<uint8_t> after = Decode(l, mem);
    for (uint32_t y = 0; y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            const uint8_t want = x < 40 && y < 24 ? src.Row(y)[x] : before[y * 64 + x];
            REQUIRE(after[y * 64 + x] == want);
        }
    }
}

TEST_CASE("only k_8 textures are written") {
    uint32_t f[6] = {};
    f[1] = 6 | 0x10000;  // k_8_8_8_8
    f[2] = 15 | 15 << 13;
    f[5] = 1u << 9;
    std::vector<uint8_t> mem(4096, 0);
    CHECK_FALSE(band3::video::WritePlane8(ReadFetchLayout(f), mem.data(), Noise(16, 16, 1)));
    CHECK(mem == std::vector<uint8_t>(4096, 0));
}

TEST_CASE("the test pattern has its border, ramp and quadrants") {
    PlaneSet p;
    band3::video::TestPattern(p, 1280, 720, 640, 360, 0.0);
    CHECK(p.y.Row(0)[640] == 235);
    CHECK(p.y.Row(360)[2] == 235);
    CHECK(p.y.Row(360)[1277] == 235);
    // the ramp, away from the bar (at x 0..39 at t 0)
    CHECK(p.y.Row(360)[100] < p.y.Row(360)[1000]);
    CHECK(p.cr.Row(10)[10] == 240);
    CHECK(p.cb.Row(350)[630] == 128);
}
