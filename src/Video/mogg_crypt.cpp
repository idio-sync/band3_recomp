#include "src/Video/mogg_crypt.h"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <utility>

#include "src/Video/mogg.h"

extern "C" {
#include "src/ThirdParty/tinyaes/aes.h"
}

namespace band3::video {

namespace {

using Key = std::array<uint8_t, 16>;
using HiddenKey = std::array<uint8_t, 32>;

// 0x0B's key, the same for every mogg
constexpr Key kCtrKey0B = {0x37, 0xb2, 0xe2, 0xb9, 0x1c, 0x74, 0xfa, 0x9e,
                           0x38, 0x81, 0x08, 0xea, 0x36, 0x23, 0xdb, 0xe4};

// the Xbox 360's keys for each version's key mask (0x0C's also 0x0D's)
constexpr Key kHvKey0C = {0x01, 0x22, 0x00, 0x38, 0xd2, 0x01, 0x78, 0x8b,
                          0xdd, 0xcd, 0xd0, 0xf0, 0xfe, 0x3e, 0x24, 0x7f};
constexpr Key kHvKey0E = {0x51, 0x73, 0xad, 0xe5, 0xb3, 0x99, 0xb8, 0x61,
                          0x58, 0x1a, 0xf9, 0xb8, 0x1e, 0xa7, 0xbe, 0xbf};
constexpr Key kHvKey0F = {0xc6, 0x22, 0x94, 0x30, 0xd8, 0x3c, 0x84, 0x14,
                          0x08, 0x73, 0x7c, 0xf2, 0x23, 0xf6, 0xeb, 0x5a};
constexpr Key kHvKey10 = {0x02, 0x1a, 0x83, 0xf3, 0x97, 0xe9, 0xd4, 0xb8,
                          0x06, 0x74, 0x14, 0x6b, 0x30, 0x4c, 0x00, 0x91};

// the hidden keys a mogg's key index picks from: the PS3's six, then the
// Xbox 360's six, which are the ones used here
constexpr std::array<HiddenKey, 12> kHiddenKeys = {{
    {0x7f, 0x95, 0x5b, 0x9d, 0x94, 0xba, 0x12, 0xf1, 0xd7, 0x5a, 0x67, 0xd9, 0x16, 0x45, 0x28, 0xdd,
     0x61, 0x55, 0x55, 0xaf, 0x23, 0x91, 0xd6, 0x0a, 0x3a, 0x42, 0x81, 0x18, 0xb4, 0xf7, 0xf3, 0x04},
    {0x78, 0x96, 0x5d, 0x92, 0x92, 0xb0, 0x47, 0xac, 0x8f, 0x5b, 0x6d, 0xdc, 0x1c, 0x41, 0x7e, 0xda,
     0x6a, 0x55, 0x53, 0xaf, 0x20, 0xc8, 0xdc, 0x0a, 0x66, 0x43, 0xdd, 0x1c, 0xb2, 0xa5, 0xa4, 0x0c},
    {0x7e, 0x92, 0x5c, 0x93, 0x90, 0xed, 0x4a, 0xad, 0x8b, 0x07, 0x36, 0xd3, 0x10, 0x41, 0x78, 0x8f,
     0x60, 0x08, 0x55, 0xa8, 0x26, 0xcf, 0xd0, 0x0f, 0x65, 0x11, 0x84, 0x45, 0xb1, 0xa0, 0xfa, 0x57},
    {0x79, 0x97, 0x0b, 0x90, 0x92, 0xb0, 0x44, 0xad, 0x8a, 0x0e, 0x60, 0xd9, 0x14, 0x11, 0x7e, 0x8d,
     0x35, 0x5d, 0x5c, 0xfb, 0x21, 0x9c, 0xd3, 0x0e, 0x32, 0x40, 0xd1, 0x48, 0xb8, 0xa7, 0xa1, 0x0d},
    {0x28, 0xc3, 0x5d, 0x97, 0xc1, 0xec, 0x42, 0xf1, 0xdc, 0x5d, 0x37, 0xda, 0x14, 0x47, 0x79, 0x8a,
     0x32, 0x5c, 0x54, 0xf2, 0x72, 0x9d, 0xd3, 0x0d, 0x67, 0x4c, 0xd6, 0x49, 0xb4, 0xa2, 0xf3, 0x50},
    {0x28, 0x96, 0x5e, 0x95, 0xc5, 0xe9, 0x45, 0xad, 0x8a, 0x5d, 0x64, 0x8e, 0x17, 0x40, 0x2e, 0x87,
     0x36, 0x58, 0x06, 0xfd, 0x75, 0x90, 0xd0, 0x5f, 0x3a, 0x40, 0xd4, 0x4c, 0xb0, 0xf7, 0xa7, 0x04},
    {0x2c, 0x96, 0x01, 0x96, 0x9b, 0xbc, 0x15, 0xa6, 0xde, 0x0e, 0x65, 0x8d, 0x17, 0x47, 0x2f, 0xdd,
     0x63, 0x54, 0x55, 0xaf, 0x76, 0xca, 0x84, 0x5f, 0x62, 0x44, 0x80, 0x4a, 0xb3, 0xf4, 0xf4, 0x0c},
    {0x7e, 0xc4, 0x0e, 0xc6, 0x9a, 0xeb, 0x43, 0xa0, 0xdb, 0x0a, 0x64, 0xdf, 0x1c, 0x42, 0x24, 0x89,
     0x63, 0x5c, 0x55, 0xf3, 0x71, 0x90, 0xdc, 0x5d, 0x60, 0x40, 0xd1, 0x4d, 0xb2, 0xa3, 0xa7, 0x0d},
    {0x2c, 0x9a, 0x0b, 0x90, 0x9a, 0xbe, 0x47, 0xa7, 0x88, 0x5a, 0x6d, 0xdf, 0x13, 0x1d, 0x2e, 0x8b,
     0x60, 0x5e, 0x55, 0xf2, 0x74, 0x9c, 0xd7, 0x0e, 0x60, 0x40, 0x80, 0x1c, 0xb7, 0xa1, 0xf4, 0x02},
    {0x28, 0x96, 0x5b, 0x95, 0xc1, 0xe9, 0x40, 0xa3, 0x8f, 0x0c, 0x32, 0xdf, 0x43, 0x1d, 0x24, 0x8d,
     0x61, 0x09, 0x54, 0xab, 0x27, 0x9a, 0xd3, 0x58, 0x60, 0x16, 0x84, 0x4f, 0xb3, 0xa4, 0xf3, 0x0d},
    {0x25, 0x93, 0x08, 0xc0, 0x9a, 0xbd, 0x10, 0xa2, 0xd6, 0x09, 0x60, 0x8f, 0x11, 0x1d, 0x7a, 0x8f,
     0x63, 0x0b, 0x5d, 0xf2, 0x21, 0xec, 0xd7, 0x08, 0x62, 0x40, 0x84, 0x49, 0xb0, 0xad, 0xf2, 0x07},
    {0x29, 0xc3, 0x0c, 0x96, 0x96, 0xeb, 0x10, 0xa0, 0xda, 0x59, 0x32, 0xd3, 0x17, 0x41, 0x25, 0xdc,
     0x63, 0x08, 0x04, 0xae, 0x77, 0xcb, 0x84, 0x5a, 0x60, 0x4d, 0xdd, 0x45, 0xb5, 0xf4, 0xa0, 0x05},
}};

constexpr uint32_t kFirstEncrypted = 0x0B, kLastEncrypted = 0x10;

uint32_t Le32(std::span<const uint8_t> b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 |
           uint32_t(b[at + 3]) << 24;
}

uint64_t Le64(std::span<const uint8_t> b, size_t at) {
    return uint64_t(Le32(b, at)) | uint64_t(Le32(b, at + 4)) << 32;
}

uint32_t Lcg(uint32_t x) { return x * 0x19660Du + 0x3C6EF35Fu; }

// 32 bytes from a generator seeded with 0xEB, XORed over a revealed hidden key
HiddenKey Masher() {
    HiddenKey out{};
    uint32_t word = 0xEB;
    for (size_t i = 0; i < 8; i++) {
        word = word * 0x19660Eu + 0x3C6EF35Fu;
        for (size_t b = 0; b < 4; b++) out[i * 4 + b] = uint8_t(word >> (8 * b));
    }
    return out;
}

size_t Roll(size_t x) { return (x + 0x13) % 0x20; }

void Supershuffle(HiddenKey& k) {
    for (size_t i = 0; i < 8; i++) {
        std::swap(k[Roll(i * 4)], k[i * 4 + 2]);
        std::swap(k[Roll(i * 4 + 3)], k[i * 4 + 1]);
    }
    for (size_t i = 0; i < 8; i++) {
        std::swap(k[(7 - i) * 4 + 1], k[i * 4 + 2]);
        std::swap(k[(7 - i) * 4], k[i * 4 + 3]);
    }
    for (size_t i = 0; i < 8; i++) {
        std::swap(k[Roll((7 - i) * 4 + 1)], k[i * 4 + 2]);
        std::swap(k[(7 - i) * 4], k[i * 4 + 3]);
    }
    for (size_t i = 0; i < 8; i++) {
        std::swap(k[(7 - i) * 4 + 1], k[i * 4 + 2]);
        std::swap(k[Roll((7 - i) * 4)], k[i * 4 + 3]);
    }
    for (size_t i = 0; i < 8; i++) {
        std::swap(k[(7 - i) * 4 + 1], k[Roll(i * 4 + 2)]);
        std::swap(k[(7 - i) * 4], k[i * 4 + 3]);
    }
    for (size_t i = 0; i < 8; i++) {
        std::swap(k[(7 - i) * 4 + 1], k[i * 4 + 2]);
        std::swap(k[(7 - i) * 4], k[Roll(i * 4 + 3)]);
    }
}

// a hidden key shuffled and mashed: 32 hex digits, as the 16 bytes they spell
Key RevealKey(HiddenKey k) {
    for (int i = 0; i < 14; i++) Supershuffle(k);
    const HiddenKey masher = Masher();
    for (size_t i = 0; i < k.size(); i++) k[i] ^= masher[i];
    auto digit = [](uint8_t h) -> uint8_t {
        if (h >= 'a' && h <= 'f') return uint8_t(h - 87);
        if (h >= 'A' && h <= 'F') return uint8_t(h - 0x37);
        return uint8_t(h - 0x30);
    };
    Key out{};
    for (size_t i = 0; i < 16; i++) out[i] = uint8_t(digit(k[i * 2]) * 16 + digit(k[i * 2 + 1]));
    return out;
}

int Rotr(int x, int n) {
    const uint8_t v = uint8_t(x);
    n &= 7;
    return n ? uint8_t(v >> n | v << (8 - n)) : v;
}

int Rotl(int x, int n) {
    const uint8_t v = uint8_t(x);
    n &= 7;
    return n ? uint8_t(v << n | v >> (8 - n)) : v;
}

int Not(int x) { return x == 0 ? 1 : 0; }

// one of the grinder's 64 operations on a byte of the key and the next
uint8_t Op(uint8_t byte1, uint8_t byte2, uint8_t op) {
    const int32_t a1 = byte1, a2 = byte2;
    // a1 rotated right by a2's low three bits, as an int (the high bits kept)
    const int32_t rot = (a1 >> (a2 & 7)) | (a1 << (-a2 & 7));
    int32_t r = 0;
    switch (op) {
        case 0: r = a2 + Rotr(a1, Not(a2)); break;
        case 1: r = a2 + Rotr(a1, 3); break;
        case 2: r = a2 + Rotl(a1, 1); break;
        case 3: r = a2 ^ rot; break;
        case 4: r = a2 ^ Rotl(a1, 4); break;
        case 5: r = a2 + (a2 ^ Rotr(a1, 3)); break;
        case 6: r = a2 + Rotl(a1, 2); break;
        case 7: r = a2 + Not(a1); break;
        case 8: r = a2 ^ Rotr(a1, Not(a2)); break;
        case 9: r = a2 ^ (a2 + Rotl(a1, 3)); break;
        case 10: r = a2 + Rotl(a1, 3); break;
        case 11: r = a2 + Rotl(a1, 4); break;
        case 12: r = a1 ^ a2; break;
        case 13: r = a2 ^ Not(a1); break;
        case 14: r = a2 ^ (a2 + Rotr(a1, 3)); break;
        case 15: r = a2 ^ Rotl(a1, 3); break;
        case 16: r = a2 ^ Rotl(a1, 2); break;
        case 17: r = a2 + (a2 ^ Rotl(a1, 3)); break;
        case 18: r = a2 + (a1 ^ a2); break;
        case 19: r = a1 + a2; break;
        case 20: r = a2 ^ Rotr(a1, 3); break;
        case 21: r = a2 ^ (a1 + a2); break;
        case 22: r = Rotr(a1, Not(a2)); break;
        case 23: r = a2 + Rotr(a1, 1); break;
        case 24: r = rot; break;
        case 25: r = a1 == 0 ? (a2 == 0 ? 128 : 1) : 0; break;
        case 26: r = a2 + Rotr(a1, 2); break;
        case 27: r = a2 ^ Rotr(a1, 1); break;
        case 28: r = Op(uint8_t(~a1), byte2, 24); break;
        case 29: r = a2 ^ Rotr(a1, 2); break;
        case 30: r = a2 + rot; break;
        case 31: r = a2 ^ Rotl(a1, 1); break;
        // from 0x0E
        case 32: r = (((a1 << 8) | 170 | (a1 ^ 255)) >> 4) ^ a2; break;
        case 33: r = (((a1 ^ 255) | (a1 << 8)) >> 3) ^ a2; break;
        case 34: r = ((((a1 << 8) ^ 65280) | a1) >> 2) ^ a2; break;
        case 35: r = (((a1 ^ 92) | (a1 << 8)) >> 5) ^ a2; break;
        case 36: r = (((a1 << 8) | 101 | (a1 ^ 60)) >> 2) ^ a2; break;
        case 37: r = (((a1 ^ 54) | (a1 << 8)) >> 2) ^ a2; break;
        case 38: r = (((a1 ^ 54) | (a1 << 8)) >> 4) ^ a2; break;
        case 39: r = (((a1 ^ 92) | (a1 << 8) | 54) >> 1) ^ a2; break;
        case 40: r = (((a1 ^ 255) | (a1 << 8)) >> 5) ^ a2; break;
        case 41: r = (((~a1) << 8 | a1) >> 6) ^ a2; break;
        case 42: r = (((a1 ^ 92) | (a1 << 8)) >> 3) ^ a2; break;
        case 43: r = (((a1 ^ 60) | 101 | (a1 << 8)) >> 5) ^ a2; break;
        case 44: r = (((a1 ^ 54) | (a1 << 8)) >> 1) ^ a2; break;
        case 45: r = (((a1 ^ 101) | (a1 << 8) | 60) >> 6) ^ a2; break;
        case 46: r = (((a1 ^ 92) | (a1 << 8)) >> 2) ^ a2; break;
        case 47: r = (((a2 ^ 170) | (a2 << 8) | 255) >> 3) ^ a1; break;
        case 48: r = (((a1 ^ 99) | (a1 << 8) | 92) >> 6) ^ a2; break;
        case 49: r = (((a1 ^ 92) | (a1 << 8) | 54) >> 7) ^ a2; break;
        case 50: r = (((a1 ^ 92) | (a1 << 8)) >> 6) ^ a2; break;
        case 51: r = ((((a1 << 8) ^ 65280) | a1) >> 3) ^ a2; break;
        case 52: r = (((a1 ^ 255) | (a1 << 8)) >> 6) ^ a2; break;
        case 53: r = ((((a1 << 8) ^ 65280) | a1) >> 5) ^ a2; break;
        case 54: r = (((a1 ^ 60) | 101 | (a1 << 8)) >> 4) ^ a2; break;
        case 55: r = (((a1 ^ 99) | (a1 << 8) | 92) >> 3) ^ a2; break;
        case 56: r = (((a1 ^ 99) | (a1 << 8) | 92) >> 5) ^ a2; break;
        case 57: r = (((a1 ^ 175) | (a1 << 8) | 250) >> 5) ^ a2; break;
        case 58: r = (((a1 ^ 92) | (a1 << 8) | 54) >> 5) ^ a2; break;
        case 59: r = (((a1 ^ 92) | (a1 << 8) | 54) >> 3) ^ a2; break;
        case 60: r = (((a1 ^ 54) | (a1 << 8)) >> 3) ^ a2; break;
        case 61: r = (((a1 ^ 99) | (a1 << 8) | 92) >> 4) ^ a2; break;
        case 62: r = (((a1 ^ 255) | (a1 << 8) | 175) >> 6) ^ a2; break;
        case 63: r = (((a1 ^ 255) | (a1 << 8)) >> 2) ^ a2; break;
        default: break;
    }
    return uint8_t(r);
}

// The revealed key ground through operations the header's two magic values
// pick: 32 of them up to 0x0D, 64 from 0x0E
Key GrindArray(uint32_t magic_a, uint32_t magic_b, Key key, uint32_t version) {
    std::array<uint8_t, 64> taken{}, ops{};
    std::array<int, 256> pick_a{}, pick_b{};
    uint32_t a = magic_a;
    for (int& p : pick_a) {
        p = uint8_t(a) >> 3;
        a = Lcg(a);
    }
    uint32_t b = magic_b ? magic_b : 0x303F;
    for (size_t i = 0; i < 32; i++) {
        uint32_t n;
        do {
            b = Lcg(b);
            n = (b >> 2) & 0x1F;
        } while (taken[n]);
        ops[i] = uint8_t(n);
        taken[n] = 1;
    }
    a = magic_b;
    for (int& p : pick_b) {
        p = (uint8_t(a) >> 2) & 0x3F;
        a = Lcg(a);
    }
    const std::array<int, 256>* pick = &pick_a;
    if (version > 0x0D) {
        uint32_t c = magic_a;
        for (size_t i = 32; i < 64; i++) {
            uint32_t n;
            do {
                c = Lcg(c);
                n = ((c >> 2) & 0x1F) + 0x20;
            } while (taken[n]);
            ops[i] = uint8_t(n);
            taken[n] = 1;
        }
        pick = &pick_b;
    }
    for (size_t j = 0; j < 16; j++) {
        uint8_t v = key[j];
        for (size_t k = 0; k < 16; k += 2) v = Op(v, key[k + 1], ops[(*pick)[key[k]]]);
        key[j] = v;
    }
    return key;
}

// where a mogg's crypto values are: past its seek map's `n` entries
size_t CryptoBase(std::span<const uint8_t> mogg) { return 20 + size_t(Le32(mogg, 16)) * 8; }

// 0x0C to 0x10: the key from the hidden key the header's index picks, ground
// with its magic values, XORed with its Xbox 360 key mask
Key DeriveKey(std::span<const uint8_t> mogg, uint32_t version) {
    const size_t base = CryptoBase(mogg);
    const uint32_t magic_a = Le32(mogg, base + 16);
    const uint32_t magic_b = Le32(mogg, base + 24);
    const Key* hv = version <= 0x0D ? &kHvKey0C
                    : version == 0x0E ? &kHvKey0E
                    : version == 0x0F ? &kHvKey0F
                                      : &kHvKey10;
    Key mask{};
    std::copy_n(mogg.begin() + base + 48, 16, mask.begin());
    AES_ctx aes;
    AES_init_ctx(&aes, hv->data());
    AES_ECB_decrypt(&aes, mask.data());
    const uint64_t index = Le64(mogg, base + 64) % 6 + 6;
    // 0x0D grinds as 0x0C does
    const Key ground = GrindArray(magic_a, magic_b, RevealKey(kHiddenKeys[index]),
                                  version == 0x0D ? 0x0C : version);
    Key key{};
    for (size_t i = 0; i < 16; i++) key[i] = ground[i] ^ mask[i];
    return key;
}

// the Ogg (from the header's end) XORed with AES-CTR's stream: each 16-byte
// block's counter the nonce (a little-endian 128-bit number) plus its index
void Crypt(const Key& key, const Key& nonce, std::span<uint8_t> ogg) {
    AES_ctx aes;
    AES_init_ctx(&aes, key.data());
    uint64_t lo = 0, hi = 0;
    for (size_t b = 0; b < 8; b++) {
        lo |= uint64_t(nonce[b]) << (8 * b);
        hi |= uint64_t(nonce[b + 8]) << (8 * b);
    }
    std::array<uint8_t, 16> stream;
    for (size_t at = 0; at < ogg.size(); at += 16) {
        for (size_t b = 0; b < 8; b++) {
            stream[b] = uint8_t(lo >> (8 * b));
            stream[b + 8] = uint8_t(hi >> (8 * b));
        }
        AES_ECB_encrypt(&aes, stream.data());
        const size_t n = std::min<size_t>(16, ogg.size() - at);
        for (size_t b = 0; b < n; b++) ogg[at + b] ^= stream[b];
        if (++lo == 0) ++hi;
    }
}

void XorBe32(std::span<uint8_t> b, size_t at, uint32_t v) {
    for (size_t i = 0; i < 4; i++) b[at + i] ^= uint8_t(v >> (8 * (3 - i)));
}

}

bool MoggDecryptable(uint32_t version) {
    return version >= kFirstEncrypted && version <= kLastEncrypted;
}

bool DecryptMogg(std::span<uint8_t> mogg, std::string& error) {
    const std::optional<MoggHeader> header = ParseMoggHeader(mogg);
    if (!header || mogg.size() < 20) {
        error = "its header is cut short";
        return false;
    }
    if (!MoggDecryptable(header->version)) {
        error = std::format("its version 0x{:02X} isn't one band3 can decrypt", header->version);
        return false;
    }
    const size_t base = CryptoBase(mogg);
    // the nonce, and from 0x0C the magic values, key masks and key index
    const size_t needed = base + (header->version == kFirstEncrypted ? 16 : 72);
    if (Le32(mogg, 16) > (1u << 24) || needed > header->ogg_offset || mogg.size() - header->ogg_offset < 4) {
        error = "its header is cut short";
        return false;
    }
    const Key key = header->version == kFirstEncrypted ? kCtrKey0B : DeriveKey(mogg, header->version);
    Key nonce{};
    std::copy_n(mogg.begin() + base, 16, nonce.begin());
    const std::span<uint8_t> ogg = mogg.subspan(header->ogg_offset);
    Crypt(key, nonce, ogg);
    // the game's own mark on the first page, over a few of its header's bytes
    if (header->version > kFirstEncrypted && ogg.size() >= 24 && ogg[0] == 'H' && ogg[1] == 'M' &&
        ogg[2] == 'X' && ogg[3] == 'A') {
        ogg[0] = 'O';
        ogg[1] = 'g';
        ogg[2] = 'g';
        ogg[3] = 'S';
        XorBe32(ogg, 12, Lcg(Lcg(Le32(mogg, base + 16) ^ 0x5C5C5C5Cu)));
        XorBe32(ogg, 20, Lcg(Le32(mogg, base + 24) ^ 0x36363636u));
    }
    if (ogg[0] != 'O' || ogg[1] != 'g' || ogg[2] != 'g' || ogg[3] != 'S') {
        error = "it didn't decrypt to an Ogg stream";
        return false;
    }
    mogg[0] = uint8_t(kMoggUnencrypted);
    mogg[1] = mogg[2] = mogg[3] = 0;
    return true;
}

}
