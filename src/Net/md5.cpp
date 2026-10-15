#include "md5.h"

#include <cstring>

namespace band3::net {

namespace {

constexpr size_t kBlockSize = 64;

// each round's shift amounts, and floor(abs(sin(i + 1)) * 2^32)
constexpr int kShifts[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};
constexpr uint32_t kSines[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

uint32_t Rotate(uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }

// One 64-byte block into the running state; MD5's words are little-endian.
void Compress(std::array<uint32_t, 4>& state, const uint8_t* block) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++) {
        m[i] = uint32_t{block[i * 4]} | (uint32_t{block[i * 4 + 1]} << 8) |
               (uint32_t{block[i * 4 + 2]} << 16) | (uint32_t{block[i * 4 + 3]} << 24);
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const uint32_t next = b + Rotate(a + f + kSines[i] + m[g], kShifts[i]);
        a = d;
        d = c;
        c = b;
        b = next;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

}  // namespace

std::array<uint8_t, 16> Md5(std::span<const uint8_t> data) {
    std::array<uint32_t, 4> state = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const size_t whole = data.size() - data.size() % kBlockSize;
    for (size_t at = 0; at < whole; at += kBlockSize) Compress(state, data.data() + at);

    // the rest, a 1 bit, zeros, then the length in bits (little-endian): one
    // block or two
    uint8_t tail[kBlockSize * 2] = {};
    const size_t rest = data.size() - whole;
    if (rest) std::memcpy(tail, data.data() + whole, rest);
    tail[rest] = 0x80;
    const size_t tail_size = rest + 1 + 8 <= kBlockSize ? kBlockSize : kBlockSize * 2;
    const uint64_t bits = uint64_t{data.size()} * 8;
    for (int i = 0; i < 8; i++) tail[tail_size - 8 + i] = static_cast<uint8_t>(bits >> (i * 8));
    for (size_t at = 0; at < tail_size; at += kBlockSize) Compress(state, tail + at);

    std::array<uint8_t, 16> digest;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) digest[i * 4 + j] = static_cast<uint8_t>(state[i] >> (j * 8));
    }
    return digest;
}

std::string Md5Hex(std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    const auto digest =
        Md5({reinterpret_cast<const uint8_t*>(text.data()), text.size()});
    std::string hex;
    hex.reserve(32);
    for (uint8_t byte : digest) {
        hex += kHex[byte >> 4];
        hex += kHex[byte & 0xF];
    }
    return hex;
}

}
