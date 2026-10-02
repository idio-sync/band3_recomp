#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "src/Render/scene_capture.h"

// Experimental: the 360 formats the native view's capture (scene_capture.cpp)
// decodes RB3's meshes and normal maps from, apart from guest memory so the
// unit tests can check them: DxMesh's packed vertex and the DXN block.

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

}  // namespace band3::render::guest_format
