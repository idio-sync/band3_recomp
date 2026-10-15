// Checks native_vorbis's inverse MDCT (src/Audio/vorbis_mdct.h): that it
// computes, bit for bit, what the game's recompiled mdct_backward computes,
// against hashes of the recompiled code's output on tables and coefficients
// any platform reproduces exactly; and that it is an inverse MDCT at all,
// against the transform's definition evaluated in double, on tables made as
// the game's mdct_init makes them.

#include <doctest/doctest.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>
#include "src/Audio/vorbis_mdct.h"

using namespace band3::audio;

namespace {

// big-endian, as the guest keeps its tables
std::vector<uint8_t> Guest(const std::vector<float>& values) {
    std::vector<uint8_t> out;
    for (float v : values) {
        const uint32_t b = std::bit_cast<uint32_t>(v);
        out.insert(out.end(), {uint8_t(b >> 24), uint8_t(b >> 16), uint8_t(b >> 8), uint8_t(b)});
    }
    return out;
}

std::vector<uint8_t> Guest(const std::vector<int32_t>& values) {
    std::vector<float> bits;
    for (int32_t v : values) bits.push_back(std::bit_cast<float>(v));
    return Guest(bits);
}

// libvorbis's bit-reverse table (mdct_init's, integers only)
std::vector<int32_t> BitReverseTable(int log2n) {
    const int n = 1 << log2n;
    std::vector<int32_t> bitrev(n / 4);
    const int mask = (1 << (log2n - 1)) - 1;
    const int msb = 1 << (log2n - 2);
    for (int i = 0; i < n / 8; i++) {
        int acc = 0;
        for (int j = 0; msb >> j; j++) {
            if ((msb >> j) & i) acc |= 1 << j;
        }
        bitrev[i * 2] = ((~acc) & mask) - 1;
        bitrev[i * 2 + 1] = acc;
    }
    return bitrev;
}

struct Lookup {
    int log2n;
    std::vector<uint8_t> trig, trig2, bitrev;

    VorbisMdctTables Tables() const {
        return {1 << log2n, log2n, trig.data(), trig2.data(), bitrev.data()};
    }
};

// the second table from the first, as the game's mdct_init makes it
std::vector<float> Turned(const std::vector<float>& trig) {
    std::vector<float> trig2(trig.size());
    for (size_t i = 0; i < trig.size(); i += 2) {
        trig2[i] = trig[i + 1];
        trig2[i + 1] = -trig[i];
    }
    return trig2;
}

// a generator every platform runs the same: no libm
struct Lcg {
    uint32_t state;
    uint32_t Next() { return state = state * 1664525u + 1013904223u; }
    // [-1, 1), exactly
    float Uniform() { return float(Next() >> 8) * (1.0f / 16777216.0f) * 2.0f - 1.0f; }
};

uint64_t Fnv1a(const std::vector<float>& values) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (float v : values) {
        const uint32_t w = std::bit_cast<uint32_t>(v);
        for (int b = 0; b < 4; b++) {
            h ^= (w >> (8 * b)) & 0xFF;
            h *= 0x100000001b3ull;
        }
    }
    return h;
}

}

TEST_CASE("native_vorbis's inverse MDCT computes what the game's recompiled one does, bit for bit") {
    // FNV-1a of the n outputs of the recompiled mdct_backward (the generated
    // code run on a stand-in guest memory) for the tables and coefficients
    // below. To make them again: compile the generated DEFINE_REX_FUNC bodies of
    // mdct_butterfly_16, _32, _first and _generic, mdct_butterflies,
    // mdct_bitreverse and mdct_backward against a stand-in SDK (a PPCContext of
    // plain registers, a no-op fpscr, REX_LOAD/STORE_U32/U64 byte-swapping at
    // base + address, the save/restore millicode as a host-side stack of r14-r31
    // and f14-f31) with 4 GB reserved as guest memory. Store big-endian
    // kVorbisPi3_8, kVorbisPi2_8, kVorbisPi1_8 and 0.5f at 0x821651F0,
    // 0x821651F4, 0x821CEA40 and 0x82075090; the lookup as the game lays it out,
    // words {n, log2n, trig, trig2, bitrev} at 0x1000, with the three tables at
    // 0x10000, 0x30000 and 0x50000; the n/2 coefficients at 0x100000. Call
    // mdct_backward with r1 = 0x01F00000, r3 = 0x1000, r4 = r5 = 0x100000 and
    // hash the n floats it leaves at 0x100000. Each size's Lcg is seeded
    // 0x9E3779B9 ^ log2n and draws trig's n + n/4 values, then each
    // coefficient's pick and value, as below.
    const uint64_t kRecompiled[] = {
        0x2BA666B6CF3E3051ull,  // n = 64: no first or generic butterfly stage
        0xBD62A7FD4120E9D5ull,  // 128: the first stage
        0x2AF3CEEC995D63B1ull,  // 256: first and generic
        0x1AD1A6121DB7A379ull,  // 512
        0xF6680A964B1E9081ull,  // 1024
        0x66638E1E2FF6B829ull,  // 2048: a long block in RB3's songs
        0x4E11EDACD22C81A9ull,  // 4096
        0x0C38A03BB6E23235ull,  // 8192: the largest Vorbis block
    };
    for (int log2n = 6; log2n <= 13; log2n++) {
        CAPTURE(log2n);
        const int n = 1 << log2n;
        // tables of arbitrary values: exactness doesn't depend on them being
        // sines and cosines, and these need no libm to reproduce
        Lcg rng{0x9E3779B9u ^ uint32_t(log2n)};
        std::vector<float> trig(n + n / 4);
        for (float& v : trig) v = rng.Uniform();
        const Lookup lookup{log2n, Guest(trig), Guest(Turned(trig)), Guest(BitReverseTable(log2n))};
        std::vector<float> x(n, 0.0f);
        for (int k = 0; k < n / 2; k++) {
            // some zeros, some quiet, most like a decoded spectrum's
            const uint32_t pick = rng.Next() >> 29;
            const float v = rng.Uniform();
            x[k] = pick == 0 ? 0.0f : pick == 1 ? v * 1e-3f : v * 0.25f;
        }
        VorbisMdctBackward(lookup.Tables(), x.data());
        CHECK(Fnv1a(x) == kRecompiled[log2n - 6]);
    }
}

TEST_CASE("native_vorbis's inverse MDCT is libvorbis's inverse MDCT") {
    for (int log2n = 6; log2n <= 13; log2n++) {
        CAPTURE(log2n);
        const int n = 1 << log2n;
        const int n2 = n / 2;
        // the game's mdct_init: pairs (-sin, cos), angles in float
        std::vector<float> trig(n + n / 4);
        const float pi = std::numbers::pi_v<float>;
        const float a = pi / float(n);
        const float b = pi / float(2 * n);
        for (int i = 0; i < n / 4; i++) {
            const float x = float(4 * i) * a;
            trig[2 * i] = float(-std::sin(double(x)));
            trig[2 * i + 1] = float(std::cos(double(x)));
            const float y = float(2 * i + 1) * b;
            trig[n2 + 2 * i] = float(std::sin(double(y)));
            trig[n2 + 2 * i + 1] = float(std::cos(double(y)));
        }
        for (int i = 0; i < n / 8; i++) {
            const float x = float(4 * i + 2) * a;
            trig[n + 2 * i] = float(std::sin(double(x)) * -0.5);
            trig[n + 2 * i + 1] = float(std::cos(double(x)) * 0.5);
        }
        const Lookup lookup{log2n, Guest(trig), Guest(Turned(trig)), Guest(BitReverseTable(log2n))};

        Lcg rng{12345u + uint32_t(log2n)};
        std::vector<float> coefficients(n2);
        for (float& v : coefficients) v = rng.Uniform() * 0.5f;
        std::vector<float> x(n, 0.0f);
        std::copy(coefficients.begin(), coefficients.end(), x.begin());
        VorbisMdctBackward(lookup.Tables(), x.data());

        // y[i] = sum over k of X[k] cos(2 pi / n (i + 1/2 + n/4)(k + 1/2))
        double largest = 0.0;
        double worst = 0.0;
        for (int i = 0; i < n; i++) {
            double y = 0.0;
            for (int k = 0; k < n2; k++) {
                y += double(coefficients[k]) *
                     std::cos(2.0 * std::numbers::pi / n * (i + 0.5 + n / 4.0) * (k + 0.5));
            }
            largest = std::max(largest, std::fabs(y));
            worst = std::max(worst, std::fabs(double(x[i]) - y));
        }
        // float rounding: a few 1e-7 of the largest sample; a wrong index or
        // sign would be of the order of the samples themselves
        CHECK(worst <= 1e-5 * largest);
    }
}

TEST_CASE("native_vorbis takes only lookups the game's mdct_init could have made") {
    const std::vector<uint8_t> table(64);
    const uint8_t* p = table.data();
    CHECK(VorbisMdctTablesValid({64, 6, p, p, p}));
    CHECK(VorbisMdctTablesValid({8192, 13, p, p, p}));
    CHECK_FALSE(VorbisMdctTablesValid({32, 5, p, p, p}));        // smaller than Vorbis's blocks
    CHECK_FALSE(VorbisMdctTablesValid({16384, 14, p, p, p}));    // larger
    CHECK_FALSE(VorbisMdctTablesValid({2048, 10, p, p, p}));     // log2n not n's
    CHECK_FALSE(VorbisMdctTablesValid({2000, 11, p, p, p}));     // not a power of two
    CHECK_FALSE(VorbisMdctTablesValid({2048, 11, nullptr, p, p}));
    CHECK_FALSE(VorbisMdctTablesValid({2048, 11, p, nullptr, p}));
    CHECK_FALSE(VorbisMdctTablesValid({2048, 11, p, p, nullptr}));
}
