// Checks what capture copies of a texture and a mesh on the game's thread to
// decode later (src/Render/deferred_decode.h): that the bytes it copies
// (guest_formats.h's BaseLevelBytes and MipChainBytes; a mesh's buffers)
// hold every byte the decoders read, and that decoding the copies gives what
// decoding guest memory in place did, texel for texel and vertex for vertex.

#include <doctest/doctest.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "src/Render/deferred_decode.h"
#include "src/Render/guest_formats.h"

using namespace band3::render;
using namespace band3::render::guest_format;

namespace {

void Fill(std::vector<uint8_t>& mem, uint32_t seed) {
    uint32_t r = seed;
    for (uint8_t& b : mem) {
        r = r * 1664525u + 1013904223u;
        b = uint8_t(r >> 24);
    }
}

// a 2D texture's fetch constant: base at 0x10000 (only what ReadFetchLayout
// reads of it matters here), mips at 0x20000 when `mip_max`
void Fetch(uint32_t f[6], uint32_t format, uint32_t w, uint32_t h, uint32_t pitch, bool tiled,
           bool packed, uint32_t mip_max) {
    f[0] = uint32_t(tiled) << 31 | (pitch >> 5) << 22;
    f[1] = format | 0x10000;
    f[2] = (w - 1) | (h - 1) << 13;
    f[3] = 0x688u << 1;  // swizzle xyzw
    f[4] = mip_max << 6;
    f[5] = 1u << 9 | uint32_t(packed) << 11 | (mip_max ? 0x20000u : 0);
}

// past the last byte DecodeLevel reads of a w x h level placed at p
uint32_t LevelEnd(const FetchLayout& l, const FormatInfo& info, const LevelPlace& p, uint32_t w,
                  uint32_t h) {
    const uint32_t blocks_x = (w + info.block - 1) / info.block;
    const uint32_t blocks_y = (h + info.block - 1) / info.block;
    uint32_t end = 0;
    for (uint32_t by = 0; by < blocks_y; by++) {
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            const uint32_t x = p.x_blocks + bx, y = p.y_blocks + by;
            const uint32_t at =
                p.offset + (l.tiled ? uint32_t(TiledOffset2D(int32_t(x), int32_t(y),
                                                             p.pitch_blocks, info.bpb_log2))
                                    : y * p.row_bytes + x * info.bpb);
            end = std::max(end, at + info.bpb);
        }
    }
    return end;
}

struct Size {
    uint32_t w, h;
};
constexpr uint32_t kFormats[] = {2, 4, 6, 10, 18, 19, 20, 49};

}  // namespace

TEST_CASE("the base level's and the mip chain's copies hold every byte their levels' decode reads") {
    // every format decoded, tiled and linear, with packed mips and without,
    // pitches at the width and past it, sizes square, wide, tall, odd and a
    // texture 16 or less on its short side (its base in a tail of its own)
    for (uint32_t format : kFormats) {
        FormatInfo info{};
        REQUIRE(GetFormatInfo(format, info));
        for (bool tiled : {true, false}) {
            for (bool packed : {true, false}) {
                for (const Size s : {Size{1, 1}, Size{3, 5}, Size{8, 8}, Size{13, 9}, Size{16, 16},
                                     Size{17, 33}, Size{37, 21}, Size{64, 64}, Size{100, 50},
                                     Size{128, 32}, Size{32, 128}, Size{200, 33}, Size{256, 256},
                                     Size{640, 360}, Size{1024, 1024}, Size{2048, 16}}) {
                    for (uint32_t extra : {0u, 64u}) {
                        const uint32_t pitch = AlignUp(s.w, 32) + extra;
                        uint32_t f[6];
                        Fetch(f, format, s.w, s.h, pitch, tiled, packed, 15);
                        const FetchLayout l = ReadFetchLayout(f);
                        const uint32_t base_end = LevelEnd(l, info, PlaceLevel(l, info, 0), s.w, s.h);
                        CHECK_MESSAGE(base_end <= BaseLevelBytes(f), "format ", format, " tiled ",
                                      tiled, " packed ", packed, " ", s.w, "x", s.h, " pitch ",
                                      pitch);
                        uint32_t mips_end = 0;
                        for (uint32_t level = 1; level <= l.mip_max; level++) {
                            const uint32_t w = std::max(s.w >> level, 1u);
                            const uint32_t h = std::max(s.h >> level, 1u);
                            mips_end = std::max(
                                mips_end, LevelEnd(l, info, PlaceLevel(l, info, level), w, h));
                        }
                        CHECK_MESSAGE(mips_end <= MipChainBytes(f), "format ", format, " tiled ",
                                      tiled, " packed ", packed, " ", s.w, "x", s.h, " pitch ",
                                      pitch, " mips to ", l.mip_max);
                    }
                }
            }
        }
    }
    // none without a mip address or levels under it, or in a format not decoded
    uint32_t f[6];
    Fetch(f, 6, 64, 64, 64, true, true, 0);
    CHECK(MipChainBytes(f) == 0);
    Fetch(f, 7, 64, 64, 64, true, true, 6);
    CHECK(MipChainBytes(f) == 0);
}

TEST_CASE("a texture decodes from its copied bytes as from guest memory: base, mips and all") {
    // guest memory's bytes all random; the copies cut at BaseLevelBytes and
    // MipChainBytes and followed by other bytes, which a read past them
    // would show as a difference
    for (uint32_t format : kFormats) {
        for (bool tiled : {true, false}) {
            for (bool packed : {true, false}) {
                for (const Size s : {Size{8, 8}, Size{37, 21}, Size{64, 64}, Size{128, 32},
                                     Size{200, 33}, Size{512, 256}}) {
                    uint32_t f[6];
                    Fetch(f, format, s.w, s.h, AlignUp(s.w, 32), tiled, packed, 15);
                    const uint32_t base_bytes = BaseLevelBytes(f), mip_bytes = MipChainBytes(f);
                    REQUIRE(base_bytes > 0);
                    REQUIRE(mip_bytes > 0);
                    std::vector<uint8_t> base(base_bytes + 65536), mips(mip_bytes + 65536);
                    Fill(base, s.w * 7 + s.h + format);
                    Fill(mips, s.w * 13 + s.h + format);
                    Texture whole;
                    REQUIRE(DecodeTextureLevels(base.data(), mips.data(), f, whole));
                    REQUIRE(!whole.mips.empty());

                    // what CopyForLater keeps, decoded through DecodeDeferred
                    Texture copied;
                    copied.width = whole.width;
                    copied.height = whole.height;
                    copied.format = whole.format;
                    auto d = std::make_shared<DeferredPixels>();
                    d->bytes.assign(base.begin(), base.begin() + base_bytes);
                    d->mips.assign(mips.begin(), mips.begin() + mip_bytes);
                    std::copy(f, f + 6, d->fetch);
                    copied.deferred = d;
                    DecodeDeferred(copied);
                    CHECK_MESSAGE(copied.rgba == whole.rgba, "format ", format, " tiled ", tiled,
                                  " packed ", packed, " ", s.w, "x", s.h);
                    CHECK_MESSAGE(copied.mips == whole.mips, "format ", format, " tiled ", tiled,
                                  " packed ", packed, " ", s.w, "x", s.h);
                    // the copies let go of, and decoded once
                    CHECK(d->bytes.empty());
                    CHECK(d->mips.empty());
                    DecodeDeferred(copied);
                    CHECK(copied.rgba == whole.rgba);

                    // and from copies with other bytes right after them
                    std::vector<uint8_t> other(4096);
                    Fill(other, 99);
                    std::vector<uint8_t> base_copy(base.begin(), base.begin() + base_bytes);
                    std::vector<uint8_t> mip_copy(mips.begin(), mips.begin() + mip_bytes);
                    base_copy.insert(base_copy.end(), other.begin(), other.end());
                    mip_copy.insert(mip_copy.end(), other.begin(), other.end());
                    Texture padded;
                    REQUIRE(DecodeTextureLevels(base_copy.data(), mip_copy.data(), f, padded));
                    CHECK(padded.rgba == whole.rgba);
                    CHECK(padded.mips == whole.mips);
                }
            }
        }
    }
}

TEST_CASE("a texture without mips decodes from its base level's copy alone") {
    uint32_t f[6];
    Fetch(f, 20, 100, 60, 128, true, true, 0);
    std::vector<uint8_t> base(BaseLevelBytes(f) + 4096);
    Fill(base, 5);
    Texture whole;
    REQUIRE(DecodeTextureLevels(base.data(), nullptr, f, whole));
    Texture copied;
    auto d = std::make_shared<DeferredPixels>();
    d->bytes.assign(base.begin(), base.begin() + BaseLevelBytes(f));
    std::copy(f, f + 6, d->fetch);
    copied.deferred = d;
    DecodeDeferred(copied);
    CHECK(copied.rgba == whole.rgba);
    CHECK(copied.mips.empty());
}

namespace {

// the vertex buffer's mesh as CaptureGeometry decoded it in place, before
// the decode moved off the game's thread: DecodePacked per vertex, the
// triangles with a corner past the vertices left out
Geometry DecodeInPlace(const uint8_t* vsrc, uint32_t num_verts, const uint8_t* isrc,
                       uint32_t num_indices) {
    Geometry out;
    out.verts.resize(num_verts);
    for (uint32_t i = 0; i < num_verts; i++) out.verts[i] = DecodePacked(vsrc + i * 36);
    out.tangents = true;
    out.indices.reserve(num_indices);
    for (uint32_t i = 0; i + 2 < num_indices; i += 3) {
        const uint16_t a = uint16_t(isrc[i * 2] << 8 | isrc[i * 2 + 1]);
        const uint16_t b = uint16_t(isrc[i * 2 + 2] << 8 | isrc[i * 2 + 3]);
        const uint16_t c = uint16_t(isrc[i * 2 + 4] << 8 | isrc[i * 2 + 5]);
        if (a >= num_verts || b >= num_verts || c >= num_verts) continue;
        out.indices.insert(out.indices.end(), {a, b, c});
    }
    return out;
}

bool SameVerts(const std::vector<Vertex>& a, const std::vector<Vertex>& b) {
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(Vertex)) == 0);
}

}  // namespace

TEST_CASE("a mesh decodes from its copied buffers as DecodePacked did in place") {
    static_assert(sizeof(Vertex) == 72, "Vertex has no padding: compared by its bytes");
    struct Case {
        uint32_t verts, indices;
        uint32_t index_range;  // indices drawn from 0..index_range-1
    };
    // indices within the vertices; some past them (left out); a partial last
    // triangle; none within them at all; no indices
    for (const Case c : {Case{1000, 3000, 1000}, Case{500, 1500, 600}, Case{64, 101, 64},
                         Case{10, 30, 65536}, Case{300, 0, 300}, Case{65536, 6000, 65536}}) {
        std::vector<uint8_t> vb(size_t(c.verts) * 36), ib(size_t(c.indices) * 2);
        Fill(vb, c.verts);
        uint32_t r = c.indices + 1;
        for (uint32_t i = 0; i < c.indices; i++) {
            r = r * 1664525u + 1013904223u;
            const uint32_t v = (r >> 8) % c.index_range;
            ib[i * 2] = uint8_t(v >> 8);
            ib[i * 2 + 1] = uint8_t(v);
        }
        const Geometry before = DecodeInPlace(vb.data(), c.verts, ib.data(), c.indices);

        Geometry bytes;
        DecodeGeometryBytes(vb.data(), c.verts, ib.data(), c.indices, bytes);
        CHECK(SameVerts(bytes.verts, before.verts));
        CHECK(bytes.indices == before.indices);
        CHECK(bytes.tangents);
        // what the game's thread asks before it's decoded
        CHECK(HasKeptFace(c.verts, ib.data(), c.indices) == !before.indices.empty());

        // through DecodeDeferred, as the worker decodes it
        Geometry later;
        later.tangents = true;
        auto d = std::make_shared<DeferredGeometry>();
        d->vb = vb;
        d->ib = ib;
        d->num_verts = c.verts;
        d->num_indices = c.indices;
        later.deferred = d;
        DecodeDeferred(later);
        CHECK(SameVerts(later.verts, before.verts));
        CHECK(later.indices == before.indices);
        CHECK(d->vb.empty());
        CHECK(d->ib.empty());
    }
}

TEST_CASE("a frame's deferred textures and geometry are all decoded before it's handed on") {
    // draws', shades' maps, the noise map and motion blur objects' geometry
    uint32_t f[6];
    Fetch(f, 6, 16, 16, 32, false, false, 0);
    std::vector<uint8_t> base(BaseLevelBytes(f));
    Fill(base, 3);
    auto tex = [&] {
        auto t = std::make_shared<Texture>();
        auto d = std::make_shared<DeferredPixels>();
        d->bytes = base;
        std::copy(f, f + 6, d->fetch);
        t->deferred = d;
        return t;
    };
    std::vector<uint8_t> vb(36 * 3), ib = {0, 0, 0, 1, 0, 2};
    Fill(vb, 4);
    auto geom = [&] {
        auto g = std::make_shared<Geometry>();
        auto d = std::make_shared<DeferredGeometry>();
        d->vb = vb;
        d->ib = ib;
        d->num_verts = 3;
        d->num_indices = 3;
        d->faces = true;
        g->deferred = d;
        return g;
    };
    FrameCapture fc;
    DrawItem item{};
    auto draw_tex = tex();
    auto draw_geom = geom();
    item.tex = draw_tex;
    item.geom = draw_geom;
    fc.draws.push_back(item);
    ShadeState shade{};
    auto map = tex();
    shade.maps[kMapSpecular] = map;
    fc.shades.push_back(shade);
    auto noise = tex();
    fc.noise_map = noise;
    VelocityObject o;
    auto moving = geom();
    o.geom = moving;
    fc.velocity_objects.push_back(o);
    const uint64_t decodes = g_deferred_decode.decodes.load();
    DecodeDeferred(fc);
    CHECK(draw_tex->rgba.size() == 256);
    CHECK(map->rgba.size() == 256);
    CHECK(noise->rgba.size() == 256);
    CHECK(draw_geom->indices.size() == 3);
    CHECK(moving->verts.size() == 3);
    CHECK(g_deferred_decode.decodes.load() == decodes + 5);
    // again: nothing more
    DecodeDeferred(fc);
    CHECK(g_deferred_decode.decodes.load() == decodes + 5);
}
