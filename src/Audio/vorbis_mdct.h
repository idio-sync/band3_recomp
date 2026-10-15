#pragma once
#include <cstdint>

// native_vorbis (src/Hooks/native_vorbis.cpp): the inverse MDCT of the game's
// Vorbis decoder, run natively. The game decodes its songs' Ogg Vorbis with a
// statically linked libvorbis 1.2-era decoder whose mdct_backward (0x82C36480)
// turns each channel's block of n/2 coefficients into n samples, about a third
// of the decode thread's time recompiled. This is that function operation for
// operation, so it computes exactly what the recompiled one does: every float
// add, subtract and multiply the guest rounds to single precision is rounded
// here too, and each fused multiply-add (fmadds, fmsubs and their negations)
// is computed in double and rounded once to float, as the recompiled code
// computes it. Which product of each pair the guest rounds on its own, and
// which it fuses, is read off its disassembly, not libvorbis's C.
//
// The game's mdct_init (0x82C357D0) differs from libvorbis's: each trig pair is
// stored (-sin, cos) instead of (cos, -sin), and a second table holds the same
// pairs turned a quarter (cos, sin), so the butterflies take one product from
// each table and never subtract. The tables are read where the guest keeps
// them, big-endian.
//
// Adapted from libvorbis's lib/mdct.c (Copyright (c) 2002-2020 Xiph.org
// Foundation, BSD-3-Clause: src/ThirdParty/libvorbis/LICENSE.txt).

namespace band3::audio {

// libvorbis's mdct_lookup as the game's mdct_init fills it
struct VorbisMdctTables {
    int n;                  // samples out: a power of two, 64 to 8192
    int log2n;
    const uint8_t* trig;    // n + n/4 big-endian floats
    const uint8_t* trig2;   // n + n/4 big-endian floats
    const uint8_t* bitrev;  // n/4 big-endian int32s
};

// the constants the game's butterflies and bit-reverse load (from 0x821CEA40,
// 0x821651F4, 0x821651F0 and 0x82075090): libvorbis's, as floats
inline constexpr float kVorbisPi1_8 = .92387953251128675613F;
inline constexpr float kVorbisPi2_8 = .70710678118654752441F;
inline constexpr float kVorbisPi3_8 = .38268343236508977175F;

// Whether t is a lookup the game's mdct_init could have made: n a power of
// two from 64 (the smallest Vorbis block) to 8192 (the largest) and log2n its
// log, and the tables present.
bool VorbisMdctTablesValid(const VorbisMdctTables& t);

// The game's mdct_backward in place: x holds n/2 coefficients in, n samples
// out (mapping0_inverse_poll passes the guest's the same array for both).
void VorbisMdctBackward(const VorbisMdctTables& t, float* x);

}
