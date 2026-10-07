#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "src/Render/scene_capture.h"

// Experimental: the 360 formats the native view's capture (scene_capture.cpp)
// decodes RB3's meshes and textures from, apart from guest memory so the
// unit tests can check them: DxMesh's packed vertex and index buffers, the
// texture blocks, the tiled layout and the mip chain (and the bytes of each
// that capture copies to decode later), and the sampler a texture fetch
// constant describes.

namespace band3::render::guest_format {

// big-endian, as guest memory holds them
inline uint32_t Be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline float BeF32(const uint8_t* p) {
    const uint32_t u = Be32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

inline float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1;
    const uint32_t exp = (h >> 10) & 0x1f;
    const uint32_t mant = h & 0x3ff;
    float v;
    if (exp == 0) {
        v = std::ldexp(float(mant), -24);
    } else if (exp == 31) {
        v = mant ? NAN : INFINITY;
    } else {
        v = std::ldexp(float(mant | 0x400), int(exp) - 25);
    }
    return sign ? -v : v;
}

// the low `bits` bits as a signed normalised number, as a vfetch of
// 2_10_10_10 with Signed reads it: v / (2^(bits-1) - 1), at least -1. A 2-bit
// w is then -1, 0 or 1.
inline float Snorm(uint32_t v, int bits) {
    const int32_t half = 1 << (bits - 1);
    int32_t s = int32_t(v & ((1u << bits) - 1));
    if (s & half) s -= 2 * half;
    return std::max(-1.0f, float(s) / float(half - 1));
}

// CompressedVertex_Xbox (36 bytes), what DxMesh::OnSync fills the vertex
// buffer with (FillCompressedVertex) and its vertex shaders fetch
// (837915E757EEC6DC, 18A3E6C52471D288): the position, ARGB colour, uv as
// two halves, then the normal and the tangent as 2_10_10_10 (x in the low
// bits, the tangent's w its handedness: RndMesh::Vert's +0x50), the weights
// as three 10-bit fractions (the fourth makes them 1) and four bone bytes
inline Vertex DecodePacked(const uint8_t* p) {
    Vertex v{};
    for (int i = 0; i < 3; i++) v.pos[i] = BeF32(p + i * 4);
    const uint32_t argb = Be32(p + 12);
    v.color = ((argb >> 16) & 0xff) | (((argb >> 8) & 0xff) << 8) | ((argb & 0xff) << 16) |
              (argb & 0xff000000u);
    const uint32_t uv = Be32(p + 16);
    v.uv[0] = HalfToFloat(uint16_t(uv >> 16));
    v.uv[1] = HalfToFloat(uint16_t(uv & 0xffff));
    const uint32_t n = Be32(p + 20);
    for (int i = 0; i < 3; i++) v.nrm[i] = Snorm(n >> (10 * i), 10);
    const uint32_t t = Be32(p + 24);
    for (int i = 0; i < 3; i++) v.tan[i] = Snorm(t >> (10 * i), 10);
    v.tan[3] = Snorm(t >> 30, 2);
    const uint32_t w = Be32(p + 28);
    float sum = 0;
    for (int i = 0; i < 3; i++) {
        v.weight[i] = float((w >> (10 * i)) & 0x3ff) / 1023.0f;
        sum += v.weight[i];
    }
    v.weight[3] = std::max(0.0f, 1.0f - sum);
    const uint32_t bi = Be32(p + 32);
    for (int i = 0; i < 4; i++) v.bone[i] = uint8_t(bi >> (8 * i));
    return v;
}

inline constexpr uint32_t kPackedVertSize = 36;

inline uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

// A DxMesh's vertex buffer (num_verts packed vertices, DecodePacked) and its
// index buffer (num_indices big-endian u16s, a triangle list) as Geometry,
// tangents and all: a triangle with a corner past the vertices is left out
// (whatever the buffers hold past their faces), and a last partial one
inline void DecodeGeometryBytes(const uint8_t* vb, uint32_t num_verts, const uint8_t* ib,
                                uint32_t num_indices, Geometry& out) {
    out.verts.resize(num_verts);
    for (uint32_t i = 0; i < num_verts; i++) out.verts[i] = DecodePacked(vb + i * kPackedVertSize);
    out.tangents = true;
    out.indices.clear();
    out.indices.reserve(num_indices);
    for (uint32_t i = 0; i + 2 < num_indices; i += 3) {
        const uint16_t a = Be16(ib + i * 2), b = Be16(ib + i * 2 + 2), c = Be16(ib + i * 2 + 4);
        if (a >= num_verts || b >= num_verts || c >= num_verts) continue;
        out.indices.insert(out.indices.end(), {a, b, c});
    }
}

// whether DecodeGeometryBytes would keep a triangle of these, looking no
// further than the first it keeps (usually the first): what tells a mesh
// with faces before its geometry is decoded
inline bool HasKeptFace(uint32_t num_verts, const uint8_t* ib, uint32_t num_indices) {
    for (uint32_t i = 0; i + 2 < num_indices; i += 3)
        if (Be16(ib + i * 2) < num_verts && Be16(ib + i * 2 + 2) < num_verts &&
            Be16(ib + i * 2 + 4) < num_verts)
            return true;
    return false;
}

// a DXT5 alpha block (8 bytes, after its endian swap): 16 values
inline void DecodeDxt5Alpha(const uint8_t* b, uint8_t out[16]) {
    uint8_t pal[8];
    pal[0] = b[0];
    pal[1] = b[1];
    if (pal[0] > pal[1]) {
        for (int i = 1; i < 7; i++) pal[i + 1] = uint8_t(((7 - i) * pal[0] + i * pal[1]) / 7);
    } else {
        for (int i = 1; i < 5; i++) pal[i + 1] = uint8_t(((5 - i) * pal[0] + i * pal[1]) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = pal[(bits >> (3 * i)) & 7];
}

// A DXN (ATI2, BC5) block, 16 bytes after its endian swap: two DXT5 alpha
// blocks, the first the fetch's x (red), the second its y (green), as Xenia
// reads it (its host BC5 and its DXN to RG8 decompression alike), which is
// what the game's pictures under band3 show. RB3's normal-mapped shaders
// pair x with the bitangent and y with the tangent (shaders/shade.hlsli's
// MappedNormals).
inline void DecodeDxnBlock(const uint8_t* b, uint8_t x[16], uint8_t y[16]) {
    DecodeDxt5Alpha(b, x);
    DecodeDxt5Alpha(b + 8, y);
}

// ---------------------------------------------------------------------------
// textures

// Xenos tiled 2D addressing (x, y and pitch in blocks), as Xenia computes it
inline int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bpb_log2) {
    pitch = (pitch + 31) & ~31u;
    const int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bpb_log2 + 7);
    const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
    const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
    return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

inline void SwapEndian(uint8_t* p, uint32_t n, uint32_t endian) {
    switch (endian) {
        case 1:  // 8in16
            for (uint32_t i = 0; i + 1 < n; i += 2) std::swap(p[i], p[i + 1]);
            break;
        case 2:  // 8in32
            for (uint32_t i = 0; i + 3 < n; i += 4) {
                std::swap(p[i], p[i + 3]);
                std::swap(p[i + 1], p[i + 2]);
            }
            break;
        case 3:  // 16in32
            for (uint32_t i = 0; i + 3 < n; i += 4) {
                std::swap(p[i], p[i + 2]);
                std::swap(p[i + 1], p[i + 3]);
            }
            break;
        default:
            break;
    }
}

struct Rgba {
    uint8_t c[4];
};

inline void Rgb565(uint16_t v, Rgba& out) {
    out.c[0] = uint8_t(((v >> 11) & 31) * 255 / 31);
    out.c[1] = uint8_t(((v >> 5) & 63) * 255 / 63);
    out.c[2] = uint8_t((v & 31) * 255 / 31);
    out.c[3] = 255;
}

// the colour half of a DXT block; four_colour forces DXT3/5 behaviour
inline void DecodeColorBlock(const uint8_t* b, bool four_colour, Rgba out[16]) {
    const uint16_t c0 = uint16_t(b[0] | (b[1] << 8));
    const uint16_t c1 = uint16_t(b[2] | (b[3] << 8));
    Rgba pal[4];
    Rgb565(c0, pal[0]);
    Rgb565(c1, pal[1]);
    if (four_colour || c0 > c1) {
        for (int i = 0; i < 3; i++) {
            pal[2].c[i] = uint8_t((2 * pal[0].c[i] + pal[1].c[i]) / 3);
            pal[3].c[i] = uint8_t((pal[0].c[i] + 2 * pal[1].c[i]) / 3);
        }
        pal[2].c[3] = pal[3].c[3] = 255;
    } else {
        for (int i = 0; i < 3; i++) pal[2].c[i] = uint8_t((pal[0].c[i] + pal[1].c[i]) / 2);
        pal[2].c[3] = 255;
        pal[3] = Rgba{{0, 0, 0, 0}};
    }
    const uint32_t idx = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) |
                         (uint32_t(b[7]) << 24);
    for (int i = 0; i < 16; i++) out[i] = pal[(idx >> (2 * i)) & 3];
}

struct FormatInfo {
    uint32_t block;  // block width and height in texels
    uint32_t bpb;    // bytes per block
    uint32_t bpb_log2;
};

// the Xenos TextureFormats decoded
inline bool GetFormatInfo(uint32_t format, FormatInfo& info) {
    switch (format) {
        case 2: info = {1, 1, 0}; return true;    // k_8
        case 4: info = {1, 2, 1}; return true;    // k_5_6_5
        case 6: info = {1, 4, 2}; return true;    // k_8_8_8_8
        case 10: info = {1, 2, 1}; return true;   // k_8_8
        case 18: info = {4, 8, 3}; return true;   // k_DXT1
        case 19: info = {4, 16, 4}; return true;  // k_DXT2_3
        case 20: info = {4, 16, 4}; return true;  // k_DXT4_5
        case 49: info = {4, 16, 4}; return true;  // k_DXN
        default: return false;
    }
}

// texels of one block as the fetch's x, y, z, w components
inline void DecodeBlock(uint32_t format, const uint8_t* b, Rgba out[16]) {
    switch (format) {
        case 2:
            out[0] = Rgba{{b[0], 0, 0, 255}};
            break;
        case 4: {
            const uint16_t v = uint16_t(b[0] | (b[1] << 8));
            out[0] = Rgba{{uint8_t((v & 31) * 255 / 31), uint8_t(((v >> 5) & 63) * 255 / 63),
                           uint8_t(((v >> 11) & 31) * 255 / 31), 255}};
            break;
        }
        case 6:
            out[0] = Rgba{{b[0], b[1], b[2], b[3]}};
            break;
        case 10:
            out[0] = Rgba{{b[0], b[1], 0, 255}};
            break;
        case 18:
            DecodeColorBlock(b, false, out);
            break;
        case 19:
            DecodeColorBlock(b + 8, true, out);
            for (int i = 0; i < 16; i++) {
                const uint8_t nib = uint8_t((b[i / 2] >> ((i & 1) * 4)) & 0xF);
                out[i].c[3] = uint8_t(nib * 17);
            }
            break;
        case 20: {
            DecodeColorBlock(b + 8, true, out);
            uint8_t a[16];
            DecodeDxt5Alpha(b, a);
            for (int i = 0; i < 16; i++) out[i].c[3] = a[i];
            break;
        }
        case 49: {
            // normal maps' tangent-space x and y (DecodeDxnBlock), y in z
            // and w too: a fetch fills a format's missing components with
            // its last, as Xenia's texture cache does for DXN (RGGG,
            // rex/graphics/pipeline/texture/cache.h's GetHostFormatSwizzle).
            // TexBlender's copy of a head's normal map into norm_output.tex
            // writes them out, which guest memory's copy shows.
            uint8_t x[16], y[16];
            DecodeDxnBlock(b, x, y);
            for (int i = 0; i < 16; i++) out[i] = Rgba{{x[i], y[i], y[i], y[i]}};
            break;
        }
    }
}

inline uint32_t Log2Ceil(uint32_t v) {
    uint32_t l = 0;
    while ((1u << l) < v) l++;
    return l;
}
inline uint32_t Log2Floor(uint32_t v) {
    uint32_t l = 0;
    while (v > 1) {
        v >>= 1;
        l++;
    }
    return l;
}
inline uint32_t NextPow2(uint32_t v) { return 1u << Log2Ceil(v); }
inline uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// The level a texture's packed mip tail starts at: the first whose shorter
// side is 16 texels or less (Xenia's texture_util GetPackedMipLevel). With
// packed mips, that level and every smaller one share one 32x32-texel tile.
inline uint32_t PackedMipLevel(uint32_t width, uint32_t height) {
    const uint32_t log2_size = Log2Ceil(std::min(width, height));
    return log2_size > 4 ? log2_size - 4 : 0;
}

// Where `mip` of a width x height texture is in its packed tail, in blocks
// of `block` texels, or false if it isn't packed (Xenia's texture_util
// GetPackedMipOffset, for 2D): a wide texture's tail goes down then across,
// a tall or square one's across then down
inline bool PackedMipOffset(uint32_t width, uint32_t height, uint32_t block, uint32_t mip,
                            uint32_t& x_blocks, uint32_t& y_blocks) {
    const uint32_t log2_width = Log2Ceil(width), log2_height = Log2Ceil(height);
    const uint32_t log2_size = std::min(log2_width, log2_height);
    x_blocks = y_blocks = 0;
    if (log2_size > 4 + mip) return false;
    const uint32_t packed_mip_base = log2_size > 4 ? log2_size - 4 : 0;
    const uint32_t packed_mip = mip - packed_mip_base;
    if (packed_mip < 3) {
        if (log2_width > log2_height)
            y_blocks = 16 >> packed_mip;
        else
            x_blocks = 16 >> packed_mip;
    } else if (log2_width > log2_height) {
        x_blocks = (1u << (log2_width - packed_mip_base)) >> (packed_mip - 2);
    } else {
        y_blocks = (1u << (log2_height - packed_mip_base)) >> (packed_mip - 2);
    }
    x_blocks /= block;
    y_blocks /= block;
    return true;
}

// What a texture fetch constant's six dwords say about the texture's layout
struct FetchLayout {
    bool tiled = false;
    uint32_t pitch_texels = 0;  // the base level's row pitch
    uint32_t format = 0, endian = 0;
    uint32_t base_address = 0, mip_address = 0;
    uint32_t width = 0, height = 0;
    uint32_t swizzle = 0;
    uint32_t dimension = 0;  // 1 2D, 3 cube
    bool packed_mips = false;
    // the last level stored under mip_address (0: none), Xenia's
    // GetSubresourcesFromFetchConstant's mip_max_level
    uint32_t mip_max = 0;
};

inline FetchLayout ReadFetchLayout(const uint32_t f[6]) {
    FetchLayout l;
    l.tiled = (f[0] >> 31) & 1;
    l.pitch_texels = ((f[0] >> 22) & 0x1ff) << 5;
    l.format = f[1] & 0x3f;
    l.endian = (f[1] >> 6) & 3;
    l.base_address = f[1] & 0xfffff000u;
    l.width = (f[2] & 0x1fff) + 1;
    l.height = ((f[2] >> 13) & 0x1fff) + 1;
    l.swizzle = (f[3] >> 1) & 0xfff;
    l.dimension = (f[5] >> 9) & 3;
    l.packed_mips = (f[5] >> 11) & 1;
    l.mip_address = f[5] & 0xfffff000u;
    const uint32_t size_max = Log2Floor(std::max(l.width, l.height));
    const uint32_t mip_min = std::min((f[4] >> 2) & 15, size_max);
    l.mip_max = l.mip_address ? std::max(std::min((f[4] >> 6) & 15, size_max), mip_min) : 0;
    return l;
}

// Where a level's texels are: the bytes from its memory's start (the base
// address for level 0, the mip address for the rest) to the 32x32-block-
// padded image it's stored in, that image's row pitch in blocks (tiled) or
// bytes (linear), and its own offset in that image in blocks (non-zero in a
// packed mip tail), as Xenia's GetGuestTextureLayout lays them out: the base
// at the fetch's pitch; mips each padded to max(next_pow2(size) >> level, 1)
// in 32-block tiles, 4 KB aligned, one after another from the mip address;
// with packed mips, every level from PackedMipLevel on in that level's image
// (the mips' tail like level 0's, under the mip address, when the texture's
// shorter side is 16 or less, and then the base in a tail of its own too).
struct LevelPlace {
    uint32_t offset = 0;
    uint32_t pitch_blocks = 0;
    uint32_t row_bytes = 0;  // linear only
    uint32_t x_blocks = 0, y_blocks = 0;
};

inline LevelPlace PlaceLevel(const FetchLayout& l, const FormatInfo& info, uint32_t level) {
    LevelPlace p;
    const uint32_t packed = l.packed_mips ? PackedMipLevel(l.width, l.height) : ~0u;
    if (level == 0) {
        const uint32_t blocks_x = (l.width + info.block - 1) / info.block;
        p.pitch_blocks =
            std::max(blocks_x, (std::max(l.pitch_texels, l.width) + info.block - 1) / info.block);
    } else {
        // the image it's stored in, and the bytes of each image before it
        const uint32_t stored = std::min(level, packed);
        auto pitch_of = [&](uint32_t s) {
            const uint32_t texels = std::max(NextPow2(l.width) >> s, 1u);
            return AlignUp((texels + info.block - 1) / info.block, 32);
        };
        auto row_bytes_of = [&](uint32_t s) {
            const uint32_t bytes = pitch_of(s) * info.bpb;
            return l.tiled ? bytes : AlignUp(bytes, 256);
        };
        for (uint32_t s = 1; s < stored; s++) {
            const uint32_t rows = std::max(NextPow2(l.height) >> s, 1u);
            const uint32_t block_rows = AlignUp((rows + info.block - 1) / info.block, 32);
            p.offset += AlignUp(row_bytes_of(s) * block_rows, 4096);
        }
        p.pitch_blocks = pitch_of(stored);
    }
    p.row_bytes = AlignUp(p.pitch_blocks * info.bpb, 256);
    if (level >= packed) PackedMipOffset(l.width, l.height, info.block, level, p.x_blocks, p.y_blocks);
    return p;
}

// The bytes from the start of a level's image (p.offset on from its memory's
// start) that hold a w x h level placed at p, as DecodeLevel reads them
// (whole rows when linear; tiled, whole 32x32-block tiles, which
// TiledOffset2D lays out in 4 KB groups: a tile of 1- or 2-byte blocks, 1 or
// 2 KB, shares its group with the next ones, its rows 16-31 2 KB on, so the
// last tile's texels reach to its group's end)
inline uint32_t LevelBytes(const FetchLayout& l, const FormatInfo& info, const LevelPlace& p,
                           uint32_t w, uint32_t h) {
    const uint32_t blocks_x = p.x_blocks + (w + info.block - 1) / info.block;
    const uint32_t blocks_y = p.y_blocks + (h + info.block - 1) / info.block;
    if (l.tiled) {
        const uint32_t tiles =
            AlignUp(std::max(p.pitch_blocks, blocks_x), 32) / 32 * (AlignUp(blocks_y, 32) / 32);
        return AlignUp(tiles << (info.bpb_log2 + 7), 512) << 3;
    }
    return p.row_bytes * blocks_y;
}

// The bytes from the base address that hold the base level, as DecodeLevel
// reads them (LevelBytes), or 0 for a format not decoded
inline uint32_t BaseLevelBytes(const uint32_t f[6]) {
    const FetchLayout l = ReadFetchLayout(f);
    FormatInfo info;
    if (!GetFormatInfo(l.format, info)) return 0;
    return LevelBytes(l, info, PlaceLevel(l, info, 0), l.width, l.height);
}

// The bytes from the mip address that hold levels 1..mip_max, as
// DecodeTextureLevels reads them: the furthest any level's image reaches
// (its offset, then LevelBytes; a packed tail's levels share one), or 0
// for none or a format not decoded. With BaseLevelBytes, what a texture's
// texels are copied as to decode later (scene_capture.cpp's CopyForLater).
inline uint32_t MipChainBytes(const uint32_t f[6]) {
    const FetchLayout l = ReadFetchLayout(f);
    FormatInfo info;
    if (!l.mip_max || !GetFormatInfo(l.format, info)) return 0;
    uint32_t end = 0;
    for (uint32_t level = 1; level <= l.mip_max; level++) {
        const LevelPlace p = PlaceLevel(l, info, level);
        const uint32_t w = std::max(l.width >> level, 1u), h = std::max(l.height >> level, 1u);
        end = std::max(end, p.offset + LevelBytes(l, info, p, w, h));
    }
    return end;
}

// DecodeLevel, block by block: every format's way, a texel's address, block
// decode and swizzle at a time
inline void DecodeLevelBlocks(const uint8_t* src, const FetchLayout& l, const FormatInfo& info,
                              const LevelPlace& p, uint32_t w, uint32_t h, uint32_t* out) {
    const uint32_t blocks_x = (w + info.block - 1) / info.block;
    const uint32_t blocks_y = (h + info.block - 1) / info.block;
    uint8_t block[16];
    Rgba texels[16];
    for (uint32_t by = 0; by < blocks_y; by++) {
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            const uint32_t x = p.x_blocks + bx, y = p.y_blocks + by;
            const uint32_t offset =
                p.offset + (l.tiled ? uint32_t(TiledOffset2D(int32_t(x), int32_t(y),
                                                             p.pitch_blocks, info.bpb_log2))
                                    : y * p.row_bytes + x * info.bpb);
            std::memcpy(block, src + offset, info.bpb);
            SwapEndian(block, info.bpb, l.endian);
            DecodeBlock(l.format, block, texels);
            const uint32_t n = info.block;
            for (uint32_t ty = 0; ty < n; ty++) {
                for (uint32_t tx = 0; tx < n; tx++) {
                    const uint32_t px = bx * n + tx, py = by * n + ty;
                    if (px >= w || py >= h) continue;
                    const Rgba& s = texels[ty * n + tx];
                    uint32_t rgba = 0;
                    for (int c = 0; c < 4; c++) {
                        const uint32_t sel = (l.swizzle >> (3 * c)) & 7;
                        const uint8_t v = sel < 4 ? s.c[sel] : sel == 4 ? 0 : 255;
                        rgba |= uint32_t(v) << (8 * c);
                    }
                    out[size_t(py) * w + px] = rgba;
                }
            }
        }
    }
}

// DecodeLevel for k_8, the same texels faster: a byte a texel, no endian
// swap, its block (byte, 0, 0, 255) swizzled into a texel byte * mul | konst,
// the channels that take the byte in mul, those that are 255 in konst. Tiled,
// texels x..x+7 are side by side when x is a multiple of 8 (TiledOffset2D
// leaves the low three bits of x as they are), so a run of eight takes one
// address. Bink's movie planes are k_8: a 1280x720 Y plane and its two
// 640x360 chroma planes take the general loop 10-12 ms, this 0.3-0.9 ms.
inline void DecodeLevel8(const uint8_t* src, const FetchLayout& l, const LevelPlace& p,
                         uint32_t w, uint32_t h, uint32_t* out) {
    uint32_t mul = 0, konst = 0;
    for (int c = 0; c < 4; c++) {
        const uint32_t sel = (l.swizzle >> (3 * c)) & 7;
        if (sel == 0) mul |= 1u << (8 * c);
        else if (sel == 3 || sel > 4) konst |= 0xFFu << (8 * c);
    }
    for (uint32_t py = 0; py < h; py++) {
        const uint32_t y = p.y_blocks + py;
        uint32_t* row = out + size_t(py) * w;
        if (!l.tiled) {
            const uint8_t* s = src + p.offset + size_t(y) * p.row_bytes + p.x_blocks;
            for (uint32_t px = 0; px < w; px++) row[px] = s[px] * mul | konst;
            continue;
        }
        for (uint32_t px = 0; px < w;) {
            const uint32_t x = p.x_blocks + px;
            const uint8_t* s = src + p.offset +
                               uint32_t(TiledOffset2D(int32_t(x), int32_t(y), p.pitch_blocks, 0));
            if ((x & 7) == 0 && px + 8 <= w) {
                for (uint32_t i = 0; i < 8; i++) row[px + i] = s[i] * mul | konst;
                px += 8;
            } else {
                row[px++] = s[0] * mul | konst;
            }
        }
    }
}

// Decodes level `level` (w x h texels) from src, where PlaceLevel says it is,
// into out (w * h RGBA8, R in the low byte, swizzled as the fetch says)
inline void DecodeLevel(const uint8_t* src, const FetchLayout& l, const FormatInfo& info,
                        const LevelPlace& p, uint32_t w, uint32_t h, uint32_t* out) {
    if (l.format == 2) DecodeLevel8(src, l, p, w, h, out);
    else DecodeLevelBlocks(src, l, info, p, w, h, out);
}

// A 2D texture's base level and mip chain, from its base level's memory
// (`base`, the fetch's base address) and its mips' (`mips`, its mip address;
// null for none), as the fetch constant `f` lays them out. False, out left
// empty, for a format not decoded or a texture that isn't 2D. Levels that are
// the base itself in a packed tail (a texture 16 or less on its short side)
// are read where the tail puts them.
inline bool DecodeTextureLevels(const uint8_t* base, const uint8_t* mips, const uint32_t f[6],
                                Texture& out, uint32_t max_size = 4096) {
    const FetchLayout l = ReadFetchLayout(f);
    out.format = l.format;
    FormatInfo info;
    if (l.dimension != 1 || !GetFormatInfo(l.format, info) || !base || l.width > max_size ||
        l.height > max_size)
        return false;
    out.width = l.width;
    out.height = l.height;
    out.rgba.assign(size_t(l.width) * l.height, 0);
    DecodeLevel(base, l, info, PlaceLevel(l, info, 0), l.width, l.height, out.rgba.data());
    out.mips.clear();
    if (!mips) return true;
    for (uint32_t level = 1; level <= l.mip_max; level++) {
        const uint32_t w = std::max(l.width >> level, 1u), h = std::max(l.height >> level, 1u);
        std::vector<uint32_t> px(size_t(w) * h, 0);
        DecodeLevel(mips, l, info, PlaceLevel(l, info, level), w, h, px.data());
        out.mips.push_back(std::move(px));
    }
    return true;
}

// The sampler a texture fetch constant describes (scene_capture.h's
// TexSampler): its clamp modes, filters, anisotropy, mip range and LOD bias
// (5 fractional bits) and border colour. Filters 2 and 3 (base map, "use the
// fetch constant") mean linear for mag and min; anisotropy 1 is 1:1 and 7
// "the fetch constant's", both isotropic.
//
// As the game's own picture under band3 has it: the SDK draws it on the host
// GPU through Xenia's texture cache, which takes the sampler from the same
// fetch constant, and then `aniso_override` (the SDK's anisotropic_override
// setting: -1 none, 0 off, 1..5 1:1..16:1) replaces the anisotropy of a
// sampler that's linear both ways, nearest or linear between levels, on a
// texture with more than one level (xenia-canary's D3D12TextureCache::
// GetSamplerParameters); anisotropy of any ratio filters linearly in all
// three ways, as there. RB3's material shaders take every filter from the
// fetch constant (their tfetch instructions override none), so the override
// is the only difference.
inline TexSampler DecodeSampler(const uint32_t f[6], int32_t aniso_override = -1) {
    TexSampler s;
    if (!f[1]) return s;
    s.filtered = 1;
    s.clamp_x = uint8_t((f[0] >> 10) & 7);
    s.clamp_y = uint8_t((f[0] >> 13) & 7);
    const uint32_t mag = (f[3] >> 19) & 3, min = (f[3] >> 21) & 3, mip = (f[3] >> 23) & 3;
    s.mag_linear = mag != 0;
    s.min_linear = min != 0;
    s.mip = uint8_t(mip == 3 ? 1 : mip);
    s.mip_min = uint8_t((f[4] >> 2) & 15);
    s.mip_max = uint8_t((f[4] >> 6) & 15);
    uint32_t aniso = (f[3] >> 25) & 7;
    if (aniso == 7) aniso = 0;
    aniso = std::min(aniso, 5u);
    const FetchLayout l = ReadFetchLayout(f);
    const uint32_t size_max = Log2Floor(std::max(l.width, l.height));
    const bool has_mips = l.mip_max > std::min<uint32_t>(s.mip_min, size_max);
    if (aniso_override >= 0 && aniso_override <= 5 && has_mips && mag == 1 && min == 1 &&
        (mip == 0 || mip == 1))
        aniso = uint32_t(aniso_override);
    s.aniso = uint8_t(aniso >= 2 ? 1u << (aniso - 1) : 1u);
    if (aniso) {
        s.mag_linear = s.min_linear = 1;
        if (s.mip != 2) s.mip = 1;
    }
    int32_t bias = int32_t((f[4] >> 12) & 1023);
    if (bias & 512) bias -= 1024;
    s.lod_bias = float(bias) / 32.0f;
    s.border_white = (f[5] & 3) == 1;
    return s;
}

}  // namespace band3::render::guest_format
