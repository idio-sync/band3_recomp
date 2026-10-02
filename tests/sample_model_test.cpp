// Checks how the native view reads textures as the game's samplers do: the
// sampler a texture fetch constant describes (guest_formats.h's
// DecodeSampler) and the host's anisotropy override on it, a mip chain laid
// out in guest memory as the Xenos lays it out, tiled with a packed tail and
// linear with the base in a tail of its own (DecodeTextureLevels, against offsets worked out by hand from Xenia's
// texture_util), and the sampling itself (src/Render/shaders/
// sample_model.hlsli, which soft_raster.cpp runs on the CPU as mesh.hlsl
// does on the GPU): point and bilinear filtering, the clamp modes, the LOD
// from the uv's derivatives, the mip filters and range, LOD bias and
// anisotropy; a render target's mips (BuildMips); and a draw on the CPU
// rasterizer picking the level its footprint on the screen asks for.

#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>
#include "src/Render/guest_formats.h"
#include "src/Render/sample_model.h"
#include "src/Render/soft_raster.h"

using namespace band3::render;
using namespace band3::render::guest_format;

namespace {

uint32_t Grey(uint32_t v) { return v | v << 8 | v << 16 | 0xff000000u; }

// a texture's levels, each one grey all over: level l is grey[l]
struct Levels {
    uint32_t w, h;
    std::vector<uint32_t> base;
    std::vector<std::vector<uint32_t>> mips;
    TexLevels View() const { return {w, h, base.data(), mips.empty() ? nullptr : &mips}; }
};

Levels FlatLevels(uint32_t w, uint32_t h, const std::vector<uint32_t>& grey) {
    Levels l{w, h, std::vector<uint32_t>(size_t(w) * h, Grey(grey[0])), {}};
    for (size_t k = 1; k < grey.size(); k++) {
        const uint32_t lw = std::max(w >> k, 1u), lh = std::max(h >> k, 1u);
        l.mips.emplace_back(size_t(lw) * lh, Grey(grey[k]));
    }
    return l;
}

TexSampler Trilinear(uint8_t mip_max) {
    TexSampler s;
    s.filtered = 1;
    s.mag_linear = s.min_linear = 1;
    s.mip = 1;
    s.mip_max = mip_max;
    return s;
}

// the sample's red, 0..255
float Sample(const TexLevels& t, const TexSampler& s, float u, float v, float dux, float dvx,
             float duy, float dvy) {
    uint32_t packed[4];
    PackSampler(s, t.Levels(), packed);
    const float uv[2] = {u, v}, dx[2] = {dux, dvx}, dy[2] = {duy, dvy};
    float out[4];
    SampleTextureCpu(t, packed, uv, dx, dy, out);
    return out[0] * 255.0f;
}

// a texel as the tests' texture data has it: its level, x and y
uint32_t Coded(uint32_t level, uint32_t x, uint32_t y) {
    return 0xff000000u | level << 16 | x << 8 | y;
}

void Put32(std::vector<uint8_t>& mem, uint32_t offset, uint32_t v) {
    REQUIRE(offset + 4 <= mem.size());
    std::memcpy(mem.data() + offset, &v, 4);  // k_8_8_8_8, no endian swap: R in byte 0
}

}  // namespace

TEST_CASE("a fetch constant's sampler: clamp modes, filters, anisotropy, mip range, bias") {
    uint32_t f[6] = {};
    CHECK(DecodeSampler(f).filtered == 0);  // nothing bound

    f[0] = 2u << 10 | 6u << 13;          // clamp to the edge across, the border down
    f[1] = 6 | 0x1000;                   // k_8_8_8_8 at 0x1000
    f[3] = 1u << 19 | 1u << 21 | 1u << 23 | 4u << 25;  // linear, linear, linear; 8:1
    f[4] = 1u << 2 | 5u << 6 | uint32_t(-16 & 1023) << 12;  // levels 1..5, bias -0.5
    f[5] = 1;                            // white border
    const TexSampler s = DecodeSampler(f);
    CHECK(s.filtered == 1);
    CHECK(s.clamp_x == 2);
    CHECK(s.clamp_y == 6);
    CHECK(s.mag_linear == 1);
    CHECK(s.min_linear == 1);
    CHECK(s.mip == 1);
    CHECK(s.aniso == 8);
    CHECK(s.mip_min == 1);
    CHECK(s.mip_max == 5);
    CHECK(s.lod_bias == -0.5f);
    CHECK(s.border_white == 1);

    // point filters, the base map alone, anisotropy off, a positive bias
    f[3] = 2u << 23;
    f[4] = uint32_t(40) << 12;
    const TexSampler p = DecodeSampler(f);
    CHECK(p.mag_linear == 0);
    CHECK(p.min_linear == 0);
    CHECK(p.mip == 2);
    CHECK(p.aniso == 1);
    CHECK(p.lod_bias == 1.25f);
}

TEST_CASE("the host's anisotropy override, as Xenia's texture cache applies it") {
    // a 64x64 with levels to 6 under its mip address, linear three ways
    uint32_t f[6] = {0, 6 | 0x1000, 63u | 63u << 13, 1u << 19 | 1u << 21 | 1u << 23, 6u << 6,
                     1u << 9 | 0x2000};
    CHECK(DecodeSampler(f).aniso == 1);
    CHECK(DecodeSampler(f, -1).aniso == 1);
    CHECK(DecodeSampler(f, 5).aniso == 16);
    CHECK(DecodeSampler(f, 3).aniso == 4);
    // nearest between levels is eligible too, and filters linearly once it's on
    f[3] = 1u << 19 | 1u << 21;
    TexSampler s = DecodeSampler(f, 5);
    CHECK(s.aniso == 16);
    CHECK(s.mip == 1);
    // not: point magnification, the base level alone, a texture without mips
    f[3] = 1u << 21 | 1u << 23;
    CHECK(DecodeSampler(f, 5).aniso == 1);
    f[3] = 1u << 19 | 1u << 21 | 2u << 23;
    CHECK(DecodeSampler(f, 5).aniso == 1);
    f[3] = 1u << 19 | 1u << 21 | 1u << 23;
    f[5] = 1u << 9;
    CHECK(DecodeSampler(f, 5).aniso == 1);
    // 0 turns a fetch constant's own anisotropy off
    f[5] = 1u << 9 | 0x2000;
    f[3] |= 3u << 25;
    CHECK(DecodeSampler(f).aniso == 4);
    CHECK(DecodeSampler(f, 0).aniso == 1);
}

TEST_CASE("the packed mip tail's places, as Xenia's GetPackedMipOffset gives them") {
    // a square 64x64: the tail from level 2 (16x16) on, across then down
    CHECK(PackedMipLevel(64, 64) == 2);
    uint32_t x, y;
    CHECK_FALSE(PackedMipOffset(64, 64, 1, 1, x, y));
    const uint32_t square[][3] = {{2, 16, 0}, {3, 8, 0}, {4, 4, 0}, {5, 0, 8}, {6, 0, 4}};
    for (const auto& c : square) {
        CHECK(PackedMipOffset(64, 64, 1, c[0], x, y));
        CHECK(x == c[1]);
        CHECK(y == c[2]);
    }
    // a wide 128x32: from level 1 (64x16), down then across
    CHECK(PackedMipLevel(128, 32) == 1);
    CHECK(PackedMipOffset(128, 32, 1, 1, x, y));
    CHECK(x == 0);
    CHECK(y == 16);
    CHECK(PackedMipOffset(128, 32, 1, 4, x, y));  // packed level 3: (1 << 6) >> 1
    CHECK(x == 32);
    CHECK(y == 0);
    // in blocks for a block-compressed format
    CHECK(PackedMipOffset(64, 64, 4, 3, x, y));
    CHECK(x == 2);
    // a texture 16 or less on its short side is all tail, its base too
    CHECK(PackedMipLevel(8, 8) == 0);
    CHECK(PackedMipOffset(8, 8, 1, 0, x, y));
    CHECK(x == 16);
}

TEST_CASE("a tiled texture's mip chain with a packed tail decodes level by level") {
    // 64x64 k_8_8_8_8, tiled, packed mips, levels to 1x1. Level 1 (32x32) is
    // the mips' first image, 32 texels' pitch, 4 KB; levels 2-6 share the
    // next image (32 pitch), at (16,0), (8,0), (4,0), (0,8) and (0,4)
    std::vector<uint8_t> base(64 * 64 * 4), mips(8192);
    for (uint32_t y = 0; y < 64; y++)
        for (uint32_t x = 0; x < 64; x++)
            Put32(base, uint32_t(TiledOffset2D(int32_t(x), int32_t(y), 64, 2)), Coded(0, x, y));
    for (uint32_t y = 0; y < 32; y++)
        for (uint32_t x = 0; x < 32; x++)
            Put32(mips, uint32_t(TiledOffset2D(int32_t(x), int32_t(y), 32, 2)), Coded(1, x, y));
    const uint32_t tail[][3] = {{2, 16, 0}, {3, 8, 0}, {4, 4, 0}, {5, 0, 8}, {6, 0, 4}};
    for (const auto& t : tail) {
        const uint32_t size = 64u >> t[0];
        for (uint32_t y = 0; y < size; y++)
            for (uint32_t x = 0; x < size; x++)
                Put32(mips,
                      4096 + uint32_t(TiledOffset2D(int32_t(t[1] + x), int32_t(t[2] + y), 32, 2)),
                      Coded(t[0], x, y));
    }
    const uint32_t f[6] = {1u << 31 | 2u << 22,               // tiled, pitch 64
                           6 | 0x10000,                       // k_8_8_8_8
                           63u | 63u << 13,                   // 64x64
                           0x688u << 1,                       // swizzle xyzw
                           6u << 6,                           // levels to 6
                           1u << 9 | 1u << 11 | 0x20000};     // 2D, packed, mips
    Texture t;
    REQUIRE(DecodeTextureLevels(base.data(), mips.data(), f, t));
    REQUIRE(t.width == 64);
    REQUIRE(t.mips.size() == 6);
    CHECK(t.rgba[5 * 64 + 9] == Coded(0, 9, 5));
    for (uint32_t level = 1; level <= 6; level++) {
        const uint32_t size = 64u >> level;
        const auto& px = t.mips[level - 1];
        REQUIRE(px.size() == size_t(size) * size);
        bool all = true;
        for (uint32_t y = 0; y < size; y++)
            for (uint32_t x = 0; x < size; x++) all &= px[size_t(y) * size + x] == Coded(level, x, y);
        CHECK_MESSAGE(all, "level ", level);
    }
}

TEST_CASE("a small linear texture's base and mips both come from packed tails") {
    // 8x8 k_8_8_8_8, linear, packed: the base at (16,0) of its tail (pitch 32
    // texels, rows 256 bytes), the mips' tail under the mip address laid out
    // as a level 0 would be: 4x4 at (8,0), 2x2 at (4,0), 1x1 at (0,4)
    std::vector<uint8_t> base(8 * 256), mips(8 * 256);
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 8; x++) Put32(base, y * 256 + (16 + x) * 4, Coded(0, x, y));
    const uint32_t tail[][3] = {{1, 8, 0}, {2, 4, 0}, {3, 0, 4}};
    for (const auto& t : tail) {
        const uint32_t size = 8u >> t[0];
        for (uint32_t y = 0; y < size; y++)
            for (uint32_t x = 0; x < size; x++)
                Put32(mips, (t[2] + y) * 256 + (t[1] + x) * 4, Coded(t[0], x, y));
    }
    const uint32_t f[6] = {1u << 22, 6 | 0x10000, 7u | 7u << 13, 0x688u << 1, 3u << 6,
                           1u << 9 | 1u << 11 | 0x20000};
    Texture t;
    REQUIRE(DecodeTextureLevels(base.data(), mips.data(), f, t));
    REQUIRE(t.mips.size() == 3);
    CHECK(t.rgba[3 * 8 + 6] == Coded(0, 6, 3));
    CHECK(t.mips[0][2 * 4 + 1] == Coded(1, 1, 2));
    CHECK(t.mips[1][1 * 2 + 1] == Coded(2, 1, 1));
    CHECK(t.mips[2][0] == Coded(3, 0, 0));

    // without a mip address, the base alone
    const uint32_t g[6] = {f[0], f[1], f[2], f[3], f[4], 1u << 9 | 1u << 11};
    REQUIRE(DecodeTextureLevels(base.data(), nullptr, g, t));
    CHECK(t.mips.empty());
    CHECK(t.rgba[3 * 8 + 6] == Coded(0, 6, 3));
}

TEST_CASE("point and bilinear filtering, and the clamp modes at the edges") {
    // 4x1: 0, 100, 200, 40 across
    const uint32_t row[4] = {Grey(0), Grey(100), Grey(200), Grey(40)};
    const TexLevels t{4, 1, row, nullptr};
    TexSampler s;
    s.filtered = 1;
    s.mip = 2;
    // magnified (no footprint): point takes the texel the uv is in
    CHECK(Sample(t, s, 0.30f, 0.5f, 0, 0, 0, 0) == doctest::Approx(100));
    // bilinear between texel centres: a quarter of the way from 100 to 200
    s.mag_linear = 1;
    CHECK(Sample(t, s, (1.5f + 0.25f) / 4, 0.5f, 0, 0, 0, 0) == doctest::Approx(125));
    // left of the first centre: repeat blends the last texel in, the edge
    // doesn't, the border is transparent black (or white), mirror repeats
    // the first
    const float u = 0.25f / 4;  // a quarter texel in: 3/4 of texel 0, 1/4 of "texel -1"
    s.clamp_x = 0;
    CHECK(Sample(t, s, u, 0.5f, 0, 0, 0, 0) == doctest::Approx(10));
    s.clamp_x = 2;
    CHECK(Sample(t, s, u, 0.5f, 0, 0, 0, 0) == doctest::Approx(0));
    s.clamp_x = 1;
    CHECK(Sample(t, s, u, 0.5f, 0, 0, 0, 0) == doctest::Approx(0));
    s.clamp_x = 6;
    s.border_white = 1;
    CHECK(Sample(t, s, u, 0.5f, 0, 0, 0, 0) == doctest::Approx(255.0f / 4));
    // far outside: repeat wraps, the edge holds the last texel, the border
    s.clamp_x = 0;
    CHECK(Sample(t, s, 3.0f + 2.5f / 4, 0.5f, 0, 0, 0, 0) == doctest::Approx(200));
    s.clamp_x = 2;
    CHECK(Sample(t, s, 7.3f, 0.5f, 0, 0, 0, 0) == doctest::Approx(40));
    s.clamp_x = 6;
    s.border_white = 0;
    CHECK(Sample(t, s, 7.3f, 0.5f, 0, 0, 0, 0) == doctest::Approx(0));
    // mirrored repeat: (1, 1.25) reads texel 3 then 2 back
    s.clamp_x = 1;
    s.mag_linear = 0;
    CHECK(Sample(t, s, 1.0f + 0.3f / 4, 0.5f, 0, 0, 0, 0) == doctest::Approx(40));
    CHECK(Sample(t, s, 1.0f + 1.3f / 4, 0.5f, 0, 0, 0, 0) == doctest::Approx(200));
}

TEST_CASE("the LOD from the footprint picks and blends levels, clamped, biased") {
    // 64x64, level l grey 20 l (level 0 black)
    const Levels l = FlatLevels(64, 64, {0, 20, 40, 60, 80, 100, 120});
    const TexLevels t = l.View();
    TexSampler s = Trilinear(6);
    // 4 texels a pixel: level 2
    CHECK(Sample(t, s, 0.3f, 0.3f, 4.0f / 64, 0, 0, 4.0f / 64) == doctest::Approx(40));
    // 2^2.5 texels: halfway between 2 and 3
    const float d = std::pow(2.0f, 2.5f) / 64;
    CHECK(Sample(t, s, 0.3f, 0.3f, d, 0, 0, d) == doctest::Approx(50).epsilon(0.001));
    // the longer side counts, either axis
    CHECK(Sample(t, s, 0.3f, 0.3f, 1.0f / 64, 0, 0, 8.0f / 64) == doctest::Approx(60));
    // a diagonal footprint: its length, 4
    CHECK(Sample(t, s, 0.3f, 0.3f, 2.828427f / 64, 2.828427f / 64, 0, 0) ==
          doctest::Approx(40).epsilon(0.001));
    // bias -1: a level finer
    s.lod_bias = -1.0f;
    CHECK(Sample(t, s, 0.3f, 0.3f, 4.0f / 64, 0, 0, 4.0f / 64) == doctest::Approx(20));
    s.lod_bias = 0;
    // the range: level 2 at most, level 1 at least
    s.mip_max = 2;
    CHECK(Sample(t, s, 0.3f, 0.3f, 32.0f / 64, 0, 0, 0) == doctest::Approx(40));
    s.mip_max = 6;
    s.mip_min = 1;
    CHECK(Sample(t, s, 0.3f, 0.3f, 0, 0, 0, 0) == doctest::Approx(20));
    s.mip_min = 0;
    // the levels the texture has: level 0 alone
    const TexLevels one{64, 64, l.base.data(), nullptr};
    CHECK(Sample(one, s, 0.3f, 0.3f, 32.0f / 64, 0, 0, 0) == doctest::Approx(0));
    // the nearest level, not a blend; the base level alone
    s.mip = 0;
    CHECK(Sample(t, s, 0.3f, 0.3f, d, 0, 0, d) == doctest::Approx(60));  // 2.5 rounds up
    const float d24 = std::pow(2.0f, 2.4f) / 64;
    CHECK(Sample(t, s, 0.3f, 0.3f, d24, 0, 0, d24) == doctest::Approx(40));
    s.mip = 2;
    CHECK(Sample(t, s, 0.3f, 0.3f, d, 0, 0, d) == doctest::Approx(0));
}

TEST_CASE("anisotropy samples along the footprint's long side at a finer level") {
    // 8 texels across a pixel, 1 down: isotropically level 3
    const Levels l = FlatLevels(64, 64, {0, 20, 40, 60, 80, 100, 120});
    TexSampler s = Trilinear(6);
    CHECK(Sample(l.View(), s, 0.3f, 0.3f, 8.0f / 64, 0, 0, 1.0f / 64) == doctest::Approx(60));
    // 16:1 takes 8 probes at level 0, 4:1 four at level 1
    s.aniso = 16;
    CHECK(Sample(l.View(), s, 0.3f, 0.3f, 8.0f / 64, 0, 0, 1.0f / 64) == doctest::Approx(0));
    s.aniso = 4;
    CHECK(Sample(l.View(), s, 0.3f, 0.3f, 8.0f / 64, 0, 0, 1.0f / 64) == doctest::Approx(20));

    // the probes spread along it: a ramp across a 16x1 level 0 averages to
    // what the footprint covers. 8 texels across (1 down) from texel 4 to
    // 12, a probe at the middle of each of its eight one-texel stretches, so
    // at 4.5..11.5, texels 4..11's centres: their mean
    std::vector<uint32_t> ramp(16);
    for (uint32_t x = 0; x < 16; x++) ramp[x] = Grey(x * 10);
    const TexLevels r{16, 1, ramp.data(), nullptr};
    TexSampler a = Trilinear(0);
    a.aniso = 16;
    a.clamp_x = 2;
    CHECK(Sample(r, a, 8.0f / 16, 0.5f, 8.0f / 16, 0, 0, 1.0f) == doctest::Approx(75));
}

TEST_CASE("a render target's mips are the box averages the GPU's blit makes") {
    // 4x2: each 2x2 block averaged, then the two of those
    const uint32_t px[8] = {Grey(0),  Grey(40),  Grey(100), Grey(100),
                            Grey(80), Grey(120), Grey(100), Grey(200)};
    std::vector<std::vector<uint32_t>> mips;
    BuildMips(px, 4, 2, FullMipChain(4, 2), mips);
    REQUIRE(mips.size() == 2);
    REQUIRE(mips[0].size() == 2);
    CHECK(mips[0][0] == Grey(60));
    CHECK(mips[0][1] == Grey(125));
    REQUIRE(mips[1].size() == 1);
    CHECK((mips[1][0] & 0xff) == 93);  // 92.5, rounded up
}

TEST_CASE("the CPU rasterizer reads the level a draw's footprint on the screen asks for") {
    // a 64x64 texture over an 8x8 picture: 8 texels a pixel, level 3, with
    // the game's trilinear sampler; with filtering off, level 0 as before
    auto tex = std::make_shared<Texture>();
    tex->width = tex->height = 64;
    tex->rgba.assign(64 * 64, Grey(0));
    const uint32_t grey[] = {0, 20, 40, 60, 80, 100, 120};
    for (uint32_t k = 1; k < 7; k++)
        tex->mips.emplace_back(size_t(64 >> k) * (64 >> k), Grey(grey[k]));

    ShadeState s;
    std::memset(static_cast<ShadeInputs*>(&s), 0, sizeof(ShadeInputs));
    s.shader_type = 18;
    s.options = 1ull << shader_opt::kPrelit | 1ull << shader_opt::kDiffuseMap;
    for (int c = 0; c < 4; c++) {
        s.ps[ShadeRegIndex(0)][c] = 1.0f;
        s.ps[ShadeRegIndex(1)][c] = 1.0f;
    }
    s.vs[ShadeRegIndex(20)][0] = 1.0f;
    s.vs[ShadeRegIndex(21)][1] = 1.0f;
    s.diffuse_sampler = Trilinear(6);

    auto g = std::make_shared<Geometry>();
    const float corner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (const auto& c : corner) {
        Vertex v{};
        v.pos[0] = -1 + 2 * c[0];
        v.pos[1] = 1 - 2 * c[1];
        v.uv[0] = c[0];
        v.uv[1] = c[1];
        v.color = 0xffffffffu;
        g->verts.push_back(v);
    }
    g->indices = {0, 1, 2, 0, 2, 3};
    DrawItem d{};
    d.geom = g;
    d.tex = tex;
    for (int i = 0; i < 4; i++) d.world.m[i][i] = d.view_proj.m[i][i] = 1.0f;
    for (float& c : d.color) c = 1.0f;
    d.blend = 1;
    d.prelit = true;
    d.shade = 0;

    FrameCapture f;
    f.shades = {s};
    f.draws = {d};
    RasterOptions o;
    o.width = o.height = 8;
    std::vector<uint32_t> rgba;
    Rasterize(f, o, rgba);
    CHECK((rgba[3 * 8 + 4] & 0xff) == 60);
    CHECK((rgba[7 * 8 + 0] & 0xff) == 60);
    o.filtering = false;
    Rasterize(f, o, rgba);
    CHECK((rgba[3 * 8 + 4] & 0xff) == 0);
}
