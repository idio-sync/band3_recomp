#include "sha1.h"

#include <cstring>
#include <vector>

namespace band3::net {

namespace {

constexpr size_t kBlockSize = 64;

uint32_t Rotate(uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); }

// One 64-byte block into the running state.
void Compress(std::array<uint32_t, 5>& state, const uint8_t* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t{block[i * 4]} << 24) | (uint32_t{block[i * 4 + 1]} << 16) |
               (uint32_t{block[i * 4 + 2]} << 8) | uint32_t{block[i * 4 + 3]};
    }
    for (int i = 16; i < 80; i++) w[i] = Rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const uint32_t next = Rotate(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = Rotate(b, 30);
        b = a;
        a = next;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

}  // namespace

std::array<uint8_t, 20> Sha1(std::span<const uint8_t> data) {
    std::array<uint32_t, 5> state = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const size_t whole =data.size() - data.size() % kBlockSize;
    for (size_t at = 0; at < whole; at += kBlockSize) Compress(state, data.data() + at);

    // the rest, a 1 bit, zeros, then the length in bits: one block or two
    uint8_t tail[kBlockSize * 2] = {};
    const size_t rest = data.size() - whole;
    if (rest) std::memcpy(tail, data.data() + whole, rest);
    tail[rest] = 0x80;
    const size_t tail_size = rest + 1 + 8 <= kBlockSize ? kBlockSize : kBlockSize * 2;
    const uint64_t bits = uint64_t{data.size()} * 8;
    for (int i = 0; i < 8; i++) tail[tail_size - 1 - i] = static_cast<uint8_t>(bits >> (i * 8));
    for (size_t at = 0; at < tail_size; at += kBlockSize) Compress(state, tail + at);

    std::array<uint8_t, 20> digest;
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 4; j++) digest[i * 4 + j] = static_cast<uint8_t>(state[i] >> (24 - j * 8));
    }
    return digest;
}

std::array<uint8_t, 20> HmacSha1(std::span<const uint8_t> key, std::span<const uint8_t> data) {
    // a key longer than a block is hashed down to one first
    uint8_t padded_key[kBlockSize] = {};
    if (key.size() > kBlockSize) {
        const auto hashed = Sha1(key);
        std::memcpy(padded_key, hashed.data(), hashed.size());
    } else if (!key.empty()) {
        std::memcpy(padded_key, key.data(), key.size());
    }

    std::vector<uint8_t> inner(kBlockSize + data.size());
    for (size_t i = 0; i < kBlockSize; i++) inner[i] = padded_key[i] ^ 0x36;
    if (!data.empty()) std::memcpy(inner.data() + kBlockSize, data.data(), data.size());
    const auto inner_hash = Sha1(inner);

    uint8_t outer[kBlockSize + 20];
    for (size_t i = 0; i < kBlockSize; i++) outer[i] = padded_key[i] ^ 0x5C;
    std::memcpy(outer + kBlockSize, inner_hash.data(), inner_hash.size());
    return Sha1(outer);
}

}
