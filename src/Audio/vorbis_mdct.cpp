// Adapted from libvorbis's lib/mdct.c (Copyright (c) 2002-2020 Xiph.org
// Foundation, BSD-3-Clause: src/ThirdParty/libvorbis/LICENSE.txt).

#include "src/Audio/vorbis_mdct.h"

#include <bit>
#include <cmath>
#include <cstring>

// Every product the guest rounds to float on its own has to stay rounded here:
// nothing in this file may fuse a multiply into the add after it. band3 builds
// with clang; CI builds the unit tests with GCC and MSVC too, which have their
// own ways of saying so, and any other compiler stops here rather than risk it.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#elif defined(_MSC_VER)
#pragma fp_contract(off)
#else
#error "vorbis_mdct.cpp needs floating-point contraction off: add this compiler's pragma"
#endif

namespace band3::audio {

namespace {

static_assert(std::endian::native == std::endian::little);

// a cursor over one of the guest's big-endian tables, stepped as libvorbis
// steps its T pointers
struct Table {
    const uint8_t* p;

    float operator[](int i) const {
        uint32_t raw;
        std::memcpy(&raw, p + 4 * i, sizeof raw);
        return std::bit_cast<float>(std::byteswap(raw));
    }
    int32_t Int(int i) const {
        uint32_t raw;
        std::memcpy(&raw, p + 4 * i, sizeof raw);
        return int32_t(std::byteswap(raw));
    }
    Table operator+(int i) const { return {p + 4 * i}; }
    Table& operator+=(int i) {
        p += 4 * i;
        return *this;
    }
    Table& operator-=(int i) {
        p -= 4 * i;
        return *this;
    }
};

// PPC's fmadds and fmsubs (a*b + c, a*b - c) as the recompiled code computes
// them: in double, rounded once to float. The product of two floats is exact
// in double, so this is std::fma's result whichever way it's compiled.
float Fmadds(float a, float b, float c) {
    return float(std::fma(double(a), double(b), double(c)));
}
float Fmsubs(float a, float b, float c) {
    return float(std::fma(double(a), double(b), -double(c)));
}

void Butterfly8(float* x) {
    float r0 = x[6] + x[2];
    float r1 = x[6] - x[2];
    float r2 = x[4] + x[0];
    float r3 = x[4] - x[0];
    x[6] = r0 + r2;
    x[4] = r0 - r2;
    r0 = x[5] - x[1];
    r2 = x[7] - x[3];
    x[0] = r1 + r0;
    x[2] = r1 - r0;
    r0 = x[5] + x[1];
    r1 = x[7] + x[3];
    x[3] = r2 + r3;
    x[1] = r2 - r3;
    x[7] = r1 + r0;
    x[5] = r1 - r0;
}

// mdct_butterfly_16 (0x82C35A98), the two 8-point ones inlined in the guest
void Butterfly16(float* x) {
    float r0 = x[1] - x[9];
    float r1 = x[0] - x[8];
    x[8] = x[8] + x[0];
    x[9] = x[9] + x[1];
    x[0] = (r0 + r1) * kVorbisPi2_8;
    x[1] = (r0 - r1) * kVorbisPi2_8;
    r0 = x[3] - x[11];
    r1 = x[10] - x[2];
    x[10] = x[10] + x[2];
    x[11] = x[11] + x[3];
    x[2] = r0;
    x[3] = r1;
    r0 = x[12] - x[4];
    r1 = x[13] - x[5];
    x[12] = x[12] + x[4];
    x[13] = x[13] + x[5];
    x[4] = (r0 - r1) * kVorbisPi2_8;
    x[5] = (r0 + r1) * kVorbisPi2_8;
    r0 = x[14] - x[6];
    r1 = x[15] - x[7];
    x[14] = x[14] + x[6];
    x[15] = x[15] + x[7];
    x[6] = r0;
    x[7] = r1;
    Butterfly8(x);
    Butterfly8(x + 8);
}

// mdct_butterfly_32 (0x82C35C90): its rotations by pi/8 and 3pi/8 round the
// second product on its own and fuse the first
void Butterfly32(float* x) {
    float r0 = x[30] - x[14];
    float r1 = x[31] - x[15];
    x[30] = x[30] + x[14];
    x[31] = x[31] + x[15];
    x[14] = r0;
    x[15] = r1;
    r0 = x[28] - x[12];
    r1 = x[29] - x[13];
    x[28] = x[28] + x[12];
    x[29] = x[29] + x[13];
    x[12] = Fmsubs(r0, kVorbisPi1_8, r1 * kVorbisPi3_8);
    x[13] = Fmadds(r1, kVorbisPi1_8, r0 * kVorbisPi3_8);
    r0 = x[26] - x[10];
    r1 = x[27] - x[11];
    x[26] = x[26] + x[10];
    x[27] = x[27] + x[11];
    x[10] = (r0 - r1) * kVorbisPi2_8;
    x[11] = (r0 + r1) * kVorbisPi2_8;
    r0 = x[24] - x[8];
    r1 = x[25] - x[9];
    x[24] = x[24] + x[8];
    x[25] = x[25] + x[9];
    x[8] = Fmsubs(r0, kVorbisPi3_8, r1 * kVorbisPi1_8);
    x[9] = Fmadds(r0, kVorbisPi1_8, r1 * kVorbisPi3_8);
    r0 = x[22] - x[6];
    r1 = x[7] - x[23];
    x[22] = x[22] + x[6];
    x[23] = x[23] + x[7];
    x[6] = r1;
    x[7] = r0;
    r0 = x[4] - x[20];
    r1 = x[5] - x[21];
    x[20] = x[20] + x[4];
    x[21] = x[21] + x[5];
    x[4] = Fmadds(r1, kVorbisPi1_8, r0 * kVorbisPi3_8);
    x[5] = Fmsubs(r1, kVorbisPi3_8, r0 * kVorbisPi1_8);
    r0 = x[2] - x[18];
    r1 = x[3] - x[19];
    x[18] = x[18] + x[2];
    x[19] = x[19] + x[3];
    x[2] = (r1 + r0) * kVorbisPi2_8;
    x[3] = (r1 - r0) * kVorbisPi2_8;
    r0 = x[0] - x[16];
    r1 = x[1] - x[17];
    x[16] = x[16] + x[0];
    x[17] = x[17] + x[1];
    x[0] = Fmadds(r0, kVorbisPi1_8, r1 * kVorbisPi3_8);
    x[1] = Fmsubs(r1, kVorbisPi1_8, r0 * kVorbisPi3_8);
    Butterfly16(x);
    Butterfly16(x + 16);
}

// mdct_butterfly_first (0x82C35EC8). In its first pair the guest rounds T*r1
// on its own and fuses T2*r0; everywhere else, here and in the generic stage,
// it's the other way round.
void ButterflyFirst(Table T, Table T2, float* x, int points) {
    float* x1 = x + points - 8;
    float* x2 = x + (points >> 1) - 8;
    do {
        float r0 = x1[6] - x2[6];
        float r1 = x1[7] - x2[7];
        x1[6] = x1[6] + x2[6];
        x1[7] = x1[7] + x2[7];
        x2[6] = Fmadds(T2[0], r0, T[0] * r1);
        x2[7] = Fmadds(T[1], r1, T2[1] * r0);
        r0 = x1[4] - x2[4];
        r1 = x1[5] - x2[5];
        x1[4] = x1[4] + x2[4];
        x1[5] = x1[5] + x2[5];
        x2[4] = Fmadds(T[4], r1, T2[4] * r0);
        x2[5] = Fmadds(T[5], r1, T2[5] * r0);
        r0 = x1[2] - x2[2];
        r1 = x1[3] - x2[3];
        x1[2] = x1[2] + x2[2];
        x1[3] = x1[3] + x2[3];
        x2[2] = Fmadds(T[8], r1, T2[8] * r0);
        x2[3] = Fmadds(T[9], r1, T2[9] * r0);
        r0 = x1[0] - x2[0];
        r1 = x1[1] - x2[1];
        x1[0] = x1[0] + x2[0];
        x1[1] = x1[1] + x2[1];
        x2[0] = Fmadds(T[12], r1, T2[12] * r0);
        x2[1] = Fmadds(T[13], r1, T2[13] * r0);
        x1 -= 8;
        x2 -= 8;
        T += 16;
        T2 += 16;
    } while (x2 >= x);
}

// mdct_butterfly_generic (0x82C36060)
void ButterflyGeneric(Table T, Table T2, float* x, int points, int trigint) {
    float* x1 = x + points - 8;
    float* x2 = x + (points >> 1) - 8;
    do {
        for (int k = 6; k >= 0; k -= 2) {
            const float r0 = x1[k] - x2[k];
            const float r1 = x1[k + 1] - x2[k + 1];
            x1[k] = x1[k] + x2[k];
            x1[k + 1] = x1[k + 1] + x2[k + 1];
            x2[k] = Fmadds(T[0], r1, T2[0] * r0);
            x2[k + 1] = Fmadds(T[1], r1, T2[1] * r0);
            T += trigint;
            T2 += trigint;
        }
        x1 -= 8;
        x2 -= 8;
    } while (x2 >= x);
}

// mdct_butterflies (0x82C361F0)
void Butterflies(const VorbisMdctTables& t, float* x, int points) {
    const Table T{t.trig};
    const Table T2{t.trig2};
    int stages = t.log2n - 5;
    if (--stages > 0) ButterflyFirst(T, T2, x, points);
    for (int i = 1; --stages > 0; i++) {
        for (int j = 0; j < (1 << i); j++) {
            ButterflyGeneric(T, T2, x + (points >> i) * j, points >> i, 4 << i);
        }
    }
    for (int j = 0; j < points; j += 32) Butterfly32(x + j);
}

// mdct_bitreverse (0x82C36328)
void BitReverse(const VorbisMdctTables& t, float* x) {
    const int n = t.n;
    Table bit{t.bitrev};
    float* w0 = x;
    float* w1 = x = w0 + (n >> 1);
    Table T = Table{t.trig} + n;
    do {
        const float* x0 = x + bit.Int(0);
        const float* x1 = x + bit.Int(1);
        float r0 = x0[1] - x1[1];
        float r1 = x0[0] + x1[0];
        float r2 = Fmadds(T[0], r0, T[1] * r1);
        float r3 = Fmsubs(T[0], r1, T[1] * r0);
        w1 -= 4;
        r0 = (x0[1] + x1[1]) * 0.5f;
        r1 = (x0[0] - x1[0]) * 0.5f;
        w0[0] = r0 + r2;
        w1[2] = r0 - r2;
        w0[1] = r1 + r3;
        w1[3] = r3 - r1;
        x0 = x + bit.Int(2);
        x1 = x + bit.Int(3);
        r0 = x0[1] - x1[1];
        r1 = x0[0] + x1[0];
        r2 = Fmadds(T[2], r0, T[3] * r1);
        r3 = Fmsubs(T[2], r1, T[3] * r0);
        r0 = (x0[1] + x1[1]) * 0.5f;
        r1 = (x0[0] - x1[0]) * 0.5f;
        w0[2] = r0 + r2;
        w1[0] = r0 - r2;
        w0[3] = r1 + r3;
        w1[1] = r3 - r1;
        T += 4;
        bit += 4;
        w0 += 4;
    } while (w0 < w1);
}

}

bool VorbisMdctTablesValid(const VorbisMdctTables& t) {
    return t.log2n >= 6 && t.log2n <= 13 && t.n == 1 << t.log2n && t.trig && t.trig2 &&
           t.bitrev;
}

void VorbisMdctBackward(const VorbisMdctTables& t, float* x) {
    const int n = t.n;
    const int n2 = n >> 1;
    const int n4 = n >> 2;
    float* const in = x;
    float* const out = x;

    // rotate: the coefficients, from the top down, into out[n2, n2 + n4) and
    // out[n2 + n4, n), which the coefficients in out[0, n2) don't overlap
    {
        const float* iX = in + n2 - 7;
        float* oX = out + n2 + n4;
        Table T = Table{t.trig} + n4;
        do {
            oX -= 4;
            oX[0] = -Fmadds(iX[0], T[3], iX[2] * T[2]);
            oX[1] = Fmsubs(iX[0], T[2], iX[2] * T[3]);
            oX[2] = -Fmadds(iX[4], T[1], iX[6] * T[0]);
            oX[3] = Fmsubs(iX[4], T[0], iX[6] * T[1]);
            iX -= 8;
            T += 4;
        } while (iX >= in);
    }
    {
        const float* iX = in + n2 - 8;
        float* oX = out + n2 + n4;
        Table T = Table{t.trig} + n4;
        do {
            T -= 4;
            oX[0] = Fmadds(iX[4], T[2], iX[6] * T[3]);
            oX[1] = Fmsubs(iX[4], T[3], iX[6] * T[2]);
            oX[2] = Fmadds(iX[2], T[1], iX[0] * T[0]);
            oX[3] = Fmsubs(iX[0], T[1], iX[2] * T[0]);
            iX -= 8;
            oX += 4;
        } while (iX >= in);
    }

    Butterflies(t, out + n2, n2);
    BitReverse(t, out);

    // rotate + window
    {
        float* oX1 = out + n2 + n4;
        float* oX2 = out + n2 + n4;
        const float* iX = out;
        Table T = Table{t.trig} + n2;
        do {
            oX1 -= 4;
            oX1[3] = Fmsubs(iX[0], T[0], iX[1] * T[1]);
            oX2[0] = -Fmadds(iX[1], T[0], iX[0] * T[1]);
            oX1[2] = Fmsubs(iX[2], T[2], iX[3] * T[3]);
            oX2[1] = -Fmadds(iX[3], T[2], iX[2] * T[3]);
            oX1[1] = Fmsubs(iX[4], T[4], iX[5] * T[5]);
            oX2[2] = -Fmadds(iX[5], T[4], iX[4] * T[5]);
            oX1[0] = Fmsubs(iX[6], T[6], iX[7] * T[7]);
            oX2[3] = -Fmadds(iX[6], T[7], iX[7] * T[6]);
            oX2 += 4;
            iX += 8;
            T += 8;
        } while (iX < oX1);

        iX = out + n2 + n4;
        oX1 = out + n4;
        oX2 = oX1;
        do {
            oX1 -= 4;
            iX -= 4;
            oX2[0] = -(oX1[3] = iX[3]);
            oX2[1] = -(oX1[2] = iX[2]);
            oX2[2] = -(oX1[1] = iX[1]);
            oX2[3] = -(oX1[0] = iX[0]);
            oX2 += 4;
        } while (oX2 < iX);

        iX = out + n2 + n4;
        oX1 = out + n2 + n4;
        oX2 = out + n2;
        do {
            oX1 -= 4;
            oX1[0] = iX[3];
            oX1[1] = iX[2];
            oX1[2] = iX[1];
            oX1[3] = iX[0];
            iX += 4;
        } while (oX1 > oX2);
    }
}

}
