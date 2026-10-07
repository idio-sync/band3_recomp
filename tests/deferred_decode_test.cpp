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
#include <thread>
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

namespace {

// the block-compressed formats kept as blocks for the GPU (native_bc_textures)
constexpr uint32_t kBcFormats[] = {18, 19, 20, 49};

// SetKeepBlocks(true) for a test's span: the setting is the process's
struct KeepBlocks {
    KeepBlocks() { SetKeepBlocks(true); }
    ~KeepBlocks() { SetKeepBlocks(false); }
};

// the texture's copies as CopyForLater keeps them, cut at BaseLevelBytes and
// MipChainBytes (none for the mips without `mips`)
std::shared_ptr<Texture> Deferred(const uint32_t f[6], const std::vector<uint8_t>& base,
                                  const std::vector<uint8_t>* mips) {
    auto t = std::make_shared<Texture>();
    const FetchLayout l = ReadFetchLayout(f);
    t->width = l.width;
    t->height = l.height;
    t->format = l.format;
    auto d = std::make_shared<DeferredPixels>();
    d->bytes.assign(base.begin(), base.begin() + BaseLevelBytes(f));
    if (mips) d->mips.assign(mips->begin(), mips->begin() + MipChainBytes(f));
    std::copy(f, f + 6, d->fetch);
    t->deferred = d;
    return t;
}

}  // namespace

TEST_CASE("a block-compressed level's blocks are each block's bytes as DecodeLevelBlocks reads them") {
    // tiled and linear, every endianness, a packed tail's level (offset in
    // its image) and odd sizes: block (bx, by) where the decoder finds it,
    // swapped, at row by, column bx
    for (uint32_t format : kBcFormats) {
        FormatInfo info{};
        REQUIRE(GetFormatInfo(format, info));
        for (bool tiled : {true, false}) {
            for (uint32_t endian : {0u, 1u, 2u, 3u}) {
                for (const Size s : {Size{1, 1}, Size{13, 9}, Size{37, 21}, Size{256, 64},
                                     Size{100, 200}}) {
                    uint32_t f[6];
                    Fetch(f, format, s.w, s.h, AlignUp(s.w, 32), tiled, true, 15);
                    f[1] |= endian << 6;
                    const FetchLayout l = ReadFetchLayout(f);
                    std::vector<uint8_t> mem(MipChainBytes(f) + BaseLevelBytes(f) + 4096);
                    Fill(mem, s.w + s.h * 3 + endian);
                    for (uint32_t level = 0; level <= l.mip_max; level++) {
                        const uint32_t w = std::max(s.w >> level, 1u);
                        const uint32_t h = std::max(s.h >> level, 1u);
                        const LevelPlace p = PlaceLevel(l, info, level);
                        std::vector<uint8_t> out(LevelBlockBytes(format, w, h));
                        UntileLevelBlocks(mem.data(), l, info, p, w, h, out.data());
                        const uint32_t blocks_x = (w + 3) / 4, blocks_y = (h + 3) / 4;
                        REQUIRE(out.size() == size_t(blocks_x) * blocks_y * info.bpb);
                        bool same = true;
                        for (uint32_t by = 0; by < blocks_y; by++) {
                            for (uint32_t bx = 0; bx < blocks_x; bx++) {
                                const uint32_t x = p.x_blocks + bx, y = p.y_blocks + by;
                                const uint32_t at =
                                    p.offset +
                                    (tiled ? uint32_t(TiledOffset2D(int32_t(x), int32_t(y),
                                                                    p.pitch_blocks, info.bpb_log2))
                                           : y * p.row_bytes + x * info.bpb);
                                uint8_t block[16];
                                std::memcpy(block, mem.data() + at, info.bpb);
                                SwapEndian(block, info.bpb, endian);
                                same = same && std::memcmp(block,
                                                           out.data() + (size_t(by) * blocks_x + bx) *
                                                                            info.bpb,
                                                           info.bpb) == 0;
                            }
                        }
                        CHECK_MESSAGE(same, "format ", format, " tiled ", tiled, " endian ", endian,
                                      " ", s.w, "x", s.h, " level ", level);
                    }
                }
            }
        }
    }
}

TEST_CASE("a block-compressed texture's blocks decode to DecodeTextureLevels' texels") {
    // every BC format, tiled and linear, packed mips and not, endianness,
    // odd sizes and pitches past the width, 16 or less on a side (the base
    // in a tail of its own): the blocks (DecodeTextureBlocks), decoded on the
    // CPU (DecodeRgbaFromBlocks), are the texels DecodeTextureLevels decodes
    // in one go
    for (uint32_t format : kBcFormats) {
        for (bool tiled : {true, false}) {
            for (bool packed : {true, false}) {
                for (uint32_t endian : {1u, 2u}) {
                    for (const Size s : {Size{1, 1}, Size{3, 5}, Size{8, 8}, Size{13, 9},
                                         Size{16, 16}, Size{37, 21}, Size{64, 64}, Size{128, 32},
                                         Size{32, 128}, Size{200, 33}, Size{512, 256},
                                         Size{2048, 16}}) {
                        for (uint32_t extra : {0u, 64u}) {
                            uint32_t f[6];
                            Fetch(f, format, s.w, s.h, AlignUp(s.w, 32) + extra, tiled, packed, 15);
                            f[1] |= endian << 6;
                            std::vector<uint8_t> base(BaseLevelBytes(f) + 4096),
                                mips(MipChainBytes(f) + 4096);
                            Fill(base, s.w * 7 + s.h + format + endian);
                            Fill(mips, s.w * 13 + s.h + format + extra);
                            Texture whole;
                            REQUIRE(DecodeTextureLevels(base.data(), mips.data(), f, whole));
                            Texture blocks;
                            REQUIRE(DecodeTextureBlocks(base.data(), mips.data(), f, blocks));
                            REQUIRE(blocks.blocks);
                            CHECK(blocks.rgba.empty());
                            CHECK(blocks.width == whole.width);
                            CHECK(blocks.height == whole.height);
                            CHECK(blocks.format == format);
                            CHECK(blocks.blocks->format == format);
                            CHECK(blocks.blocks->level0.size() ==
                                  LevelBlockBytes(format, s.w, s.h));
                            REQUIRE(blocks.blocks->mips.size() == whole.mips.size());
                            std::vector<uint32_t> rgba;
                            std::vector<std::vector<uint32_t>> levels;
                            DecodeRgbaFromBlocks(*blocks.blocks, s.w, s.h, rgba, levels);
                            CHECK_MESSAGE(rgba == whole.rgba, "format ", format, " tiled ", tiled,
                                          " packed ", packed, " endian ", endian, " ", s.w, "x",
                                          s.h, " pitch +", extra);
                            CHECK_MESSAGE(levels == whole.mips, "format ", format, " tiled ", tiled,
                                          " packed ", packed, " endian ", endian, " ", s.w, "x",
                                          s.h, " pitch +", extra);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("kept as blocks, a texture's rgba decodes from them the first time it's asked for") {
    KeepBlocks keep;
    for (uint32_t format : kBcFormats) {
        for (bool tiled : {true, false}) {
            for (const Size s : {Size{37, 21}, Size{64, 64}, Size{200, 33}}) {
                uint32_t f[6];
                Fetch(f, format, s.w, s.h, AlignUp(s.w, 32), tiled, true, 15);
                f[1] |= 1u << 6;
                std::vector<uint8_t> base(BaseLevelBytes(f) + 65536), mips(MipChainBytes(f) + 65536);
                Fill(base, s.w + format);
                Fill(mips, s.h + format);
                Texture whole;
                REQUIRE(DecodeTextureLevels(base.data(), mips.data(), f, whole));

                // the worker's decode: blocks alone, from the copies cut at
                // BaseLevelBytes and MipChainBytes, which it lets go of
                auto t = Deferred(f, base, &mips);
                const uint64_t kept = g_deferred_decode.bc_blocks.load();
                const uint64_t rgba_decodes = g_deferred_decode.bc_rgba.load();
                DecodeDeferred(*t);
                CHECK(g_deferred_decode.bc_blocks.load() == kept + 1);
                REQUIRE(t->blocks);
                CHECK(t->rgba.empty());
                CHECK(t->mips.empty());
                CHECK(t->deferred->bytes.empty());
                CHECK(t->deferred->mips.empty());
                // asked for: the same texels as decoding guest memory, once
                EnsureRgba(*t);
                CHECK_MESSAGE(t->rgba == whole.rgba, "format ", format, " tiled ", tiled, " ",
                              s.w, "x", s.h);
                CHECK_MESSAGE(t->mips == whole.mips, "format ", format, " tiled ", tiled, " ",
                              s.w, "x", s.h);
                EnsureRgba(*t);
                CHECK(g_deferred_decode.bc_rgba.load() == rgba_decodes + 1);
                CHECK(t->rgba == whole.rgba);

                // and without mips
                Texture base_only;
                REQUIRE(DecodeTextureLevels(base.data(), nullptr, f, base_only));
                auto b = Deferred(f, base, nullptr);
                EnsureRgba(*b);
                REQUIRE(b->blocks);
                CHECK(b->blocks->mips.empty());
                CHECK(b->rgba == base_only.rgba);
                CHECK(b->mips.empty());
            }
        }
    }
}

TEST_CASE("what isn't kept as blocks decodes to RGBA as before") {
    std::vector<uint8_t> base(65536), mips(65536);
    Fill(base, 11);
    Fill(mips, 12);
    auto decoded_as_before = [&](const uint32_t f[6]) {
        Texture whole;
        REQUIRE(DecodeTextureLevels(base.data(), mips.data(), f, whole));
        auto t = Deferred(f, base, &mips);
        DecodeDeferred(*t);
        CHECK(!t->blocks);
        CHECK(t->rgba == whole.rgba);
        CHECK(t->mips == whole.mips);
        EnsureRgba(*t);  // nothing more to do
        CHECK(t->rgba == whole.rgba);
    };
    uint32_t f[6];
    // with the setting off, a DXT5 texture
    Fetch(f, 20, 64, 64, 64, true, true, 6);
    decoded_as_before(f);
    Texture no_blocks;
    CHECK(DecodeTextureBlocks(base.data(), mips.data(), f, no_blocks));  // (it could be)
    no_blocks = Texture{};
    {
        KeepBlocks keep;
        // a format that isn't block-compressed
        Fetch(f, 6, 64, 64, 64, true, true, 6);
        decoded_as_before(f);
        CHECK(!DecodeTextureBlocks(base.data(), mips.data(), f, no_blocks));
        // a DXT1 texture its fetch swizzles (xyz1): counted
        Fetch(f, 18, 64, 64, 64, true, true, 6);
        f[3] = (0 | 1 << 3 | 2 << 6 | 5u << 9) << 1;
        const uint64_t swizzled = g_deferred_decode.bc_swizzled.load();
        decoded_as_before(f);
        CHECK(g_deferred_decode.bc_swizzled.load() == swizzled + 1);
        CHECK(!DecodeTextureBlocks(base.data(), mips.data(), f, no_blocks));
        CHECK(!no_blocks.blocks);
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

TEST_CASE("a frame's textures kept as blocks all have their rgba for a capture file") {
    KeepBlocks keep;
    uint32_t f[6];
    Fetch(f, 18, 16, 16, 32, false, false, 0);
    std::vector<uint8_t> base(BaseLevelBytes(f));
    Fill(base, 7);
    FrameCapture fc;
    DrawItem item{};
    auto draw_tex = Deferred(f, base, nullptr);
    item.tex = draw_tex;
    fc.draws.push_back(item);
    ShadeState shade{};
    auto map = Deferred(f, base, nullptr);
    shade.maps[kMapNormal] = map;
    fc.shades.push_back(shade);
    DecodeDeferred(fc);
    CHECK(draw_tex->blocks);
    CHECK(draw_tex->rgba.empty());
    CHECK(map->rgba.empty());
    EnsureRgba(fc);
    CHECK(draw_tex->rgba.size() == 256);
    CHECK(map->rgba.size() == 256);
}

namespace {

// a mesh's buffers as CaptureGeometry copies them, `verts` vertices and a
// triangle per three
std::shared_ptr<Geometry> DeferredMesh(uint32_t verts, uint32_t seed) {
    auto g = std::make_shared<Geometry>();
    g->tangents = true;
    auto d = std::make_shared<DeferredGeometry>();
    d->vb.resize(size_t(verts) * kPackedVertSize);
    Fill(d->vb, seed);
    d->ib.resize(size_t(verts) * 2);
    for (uint32_t i = 0; i < verts; i++) {
        d->ib[i * 2] = uint8_t(i >> 8);
        d->ib[i * 2 + 1] = uint8_t(i);
    }
    d->num_verts = verts;
    d->num_indices = verts;
    d->faces = verts >= 3;
    g->deferred = d;
    return g;
}

// a linear k_8_8_8_8 texture of `size` x `size`, its bytes from `seed`
std::shared_ptr<Texture> DeferredRgba(uint32_t size, uint32_t seed) {
    uint32_t f[6];
    Fetch(f, 6, size, size, std::max(size, 32u), false, false, 0);
    std::vector<uint8_t> base(BaseLevelBytes(f));
    Fill(base, seed);
    return Deferred(f, base, nullptr);
}

// threads helping DecodeDeferred(FrameCapture), for a test's length
struct DecodeThreads {
    explicit DecodeThreads(unsigned n) { SetDecodeThreads(n); }
    ~DecodeThreads() { SetDecodeThreads(0); }
};

}  // namespace

TEST_CASE("a frame's pending textures and meshes are gathered once each, biggest first") {
    auto small = DeferredRgba(16, 1);  // 4 KB (its rows 256 bytes)
    auto big = DeferredRgba(64, 2);    // 16 KB
    auto mesh = DeferredMesh(200, 3);  // 200 vertices and 200 indices
    auto done = DeferredRgba(32, 4);
    DecodeDeferred(*done);
    FrameCapture fc;
    DrawItem item{};
    item.tex = small;
    item.geom = mesh;
    fc.draws.push_back(item);
    item.tex = big;
    fc.draws.push_back(item);
    item.tex = done;
    item.geom = nullptr;
    fc.draws.push_back(item);
    ShadeState shade{};
    shade.maps[kMapSpecular] = small;
    shade.maps[kMapNormal] = big;
    fc.shades.push_back(shade);
    fc.noise_map = small;
    VelocityObject o;
    o.geom = mesh;
    fc.velocity_objects.push_back(o);

    const std::vector<PendingDecode> pending = GatherPending(fc);
    REQUIRE(pending.size() == 3);
    CHECK(pending[0].tex == big.get());
    CHECK(pending[0].bytes == BaseLevelBytes(big->deferred->fetch));
    CHECK(pending[1].geom == mesh.get());
    CHECK(pending[1].bytes == 200 * kPackedVertSize + 200 * 2);
    CHECK(pending[2].tex == small.get());
    CHECK(pending[2].bytes == BaseLevelBytes(small->deferred->fetch));
    DecodeDeferred(fc);
    CHECK(GatherPending(fc).empty());
}

TEST_CASE("how many helpers decode a frame's pending textures and meshes") {
    const auto of = [](std::vector<uint64_t> sizes) {
        std::vector<PendingDecode> pending;
        for (uint64_t bytes : sizes) pending.push_back({nullptr, nullptr, bytes});
        return pending;
    };
    const uint64_t enough = kParallelDecodeBytes;
    // none asked for; one alone; too little in all
    CHECK(DecodeHelpers(of({enough, enough, enough}), 0) == 0);
    CHECK(DecodeHelpers(of({8 * enough}), 3) == 0);
    CHECK(DecodeHelpers(of({enough / 4, enough / 4, enough / 4}), 3) == 0);
    CHECK(DecodeHelpers({}, 3) == 0);
    // as many as asked, at most one for each besides the asking thread's
    CHECK(DecodeHelpers(of({enough / 2, enough / 2}), 3) == 1);
    CHECK(DecodeHelpers(of({enough, 1, 1, 1, 1}), 2) == 2);
    CHECK(DecodeHelpers(of({enough, 1, 1, 1, 1}), 3) == 3);
    CHECK(DecodeHelpers(of({enough, 1, 1}), 3) == 2);
}

TEST_CASE("a frame decoded on helper threads decodes each once, as on one") {
    // over kParallelDecodeBytes of textures and meshes, some shared between
    // draws, decoded inline and on three helpers; and on two threads asking
    // at once (the worker and the harness's capture), each still once
    std::vector<std::shared_ptr<Texture>> texs, texs_inline;
    std::vector<std::shared_ptr<Geometry>> geoms, geoms_inline;
    for (uint32_t i = 0; i < 24; i++) {
        texs.push_back(DeferredRgba(16u << (i % 6), i));
        texs_inline.push_back(DeferredRgba(16u << (i % 6), i));
        geoms.push_back(DeferredMesh(30 + 700 * (i % 7), i));
        geoms_inline.push_back(DeferredMesh(30 + 700 * (i % 7), i));
    }
    const auto frame = [](const auto& t, const auto& g) {
        FrameCapture fc;
        for (size_t i = 0; i < t.size() * 2; i++) {
            DrawItem item{};
            item.tex = t[i % t.size()];
            item.geom = g[(i * 5) % g.size()];
            fc.draws.push_back(item);
        }
        return fc;
    };
    const FrameCapture fc = frame(texs, geoms), fc_inline = frame(texs_inline, geoms_inline);
    REQUIRE(DecodeHelpers(GatherPending(fc), 3) == 3);

    DecodeDeferred(fc_inline);
    const uint64_t decodes = g_deferred_decode.decodes.load();
    {
        DecodeThreads threads(3);
        std::thread other([&] { DecodeDeferred(fc); });
        DecodeDeferred(fc);
        other.join();
    }
    CHECK(g_deferred_decode.decodes.load() == decodes + texs.size() + geoms.size());
    CHECK(GatherPending(fc).empty());
    for (size_t i = 0; i < texs.size(); i++) {
        CHECK(texs[i]->rgba == texs_inline[i]->rgba);
        CHECK(texs[i]->deferred->bytes.empty());
    }
    for (size_t i = 0; i < geoms.size(); i++) {
        CHECK(SameVerts(geoms[i]->verts, geoms_inline[i]->verts));
        CHECK(geoms[i]->indices == geoms_inline[i]->indices);
    }

    // and again, the helpers asleep since: nothing more
    DecodeThreads threads(3);
    DecodeDeferred(fc);
    CHECK(g_deferred_decode.decodes.load() == decodes + texs.size() + geoms.size());
}
