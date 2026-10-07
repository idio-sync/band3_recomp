#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

// The display gamma ramp the 360's display controller applies on scan-out
// (D3DDevice_SetGammaRamp at 0x82739D28, from RB3's system config gamma;
// D3D's presentation setup at 0x8286D98C sets one first). ReXGlue's presenter
// applies it at each swap and the harness screenshot captures that, so the
// native view applies it last, after the overlay.
//
// A 10-bit front buffer uses the PWL ramp, otherwise the 256-entry table
// (0x8286D964). The capture can't see the format, but DC_LUT_RW_MODE (0
// table, 1 PWL; register_table.inc) says which the guest wrote.
//
// The table maps 8 bits to 10; CaptureGuestOutput (rexruntime.dll) takes the
// 10-bit output back to 8. Both backends apply the resulting per-value lookup
// (GammaLut): the CPU in Rasterize, the GPU in shaders/gamma.hlsl.

namespace band3::render {

struct GammaRamp {
    // kNone (unreadable or an old capture) is identity
    static constexpr uint32_t kNone = 0, kTable = 1, kPwl = 2;
    uint32_t mode = kNone;
    // DC_LUT_30_COLOR: 10-bit blue in bits 0..9, green 10..19, red 20..29
    uint32_t table[256] = {};
    // DC_LUT_PWL_DATA for red, green and blue: base in bits 0..15, delta in
    // 16..31 (the low 6 bits of each 0), for 128 steps of 8 10-bit values
    uint32_t pwl[128][3] = {};

    bool operator==(const GammaRamp& o) const {
        return mode == o.mode && std::memcmp(table, o.table, sizeof(table)) == 0 &&
               std::memcmp(pwl, o.pwl, sizeof(pwl)) == 0;
    }
};

// c: 0 red, 1 green, 2 blue
inline uint32_t TableChannel(uint32_t entry, int c) { return entry >> (20 - 10 * c) & 0x3ff; }

// as CaptureGuestOutput: multiply and add kept apart so they can't fuse into
// an FMA
inline uint8_t Unorm10To8(uint32_t v) {
    const float scaled = float(v) * (255.0f / 1023.0f);
    const float rounded = scaled + 0.5f;
    return uint8_t(rounded);
}

// 10-bit in and out, as Xenia's apply_gamma_pwl (DC_LUTA_CONTROL increment
// 3); base and delta are 10.6 fixed point. Unchecked against the game: RB3's
// front buffer is 8-bit, which uses the table.
inline uint32_t PwlChannel(const GammaRamp& ramp, uint32_t v10, int c) {
    const uint32_t entry = ramp.pwl[v10 >> 3 & 127][c];
    const uint32_t base = entry & 0xffff, delta = entry >> 16;
    const uint32_t out = (base + ((delta * (v10 & 7)) >> 3)) >> 6;
    return out > 1023 ? 1023 : out;
}

// lut[c][v]: the screen's value for front buffer value v. Identity for kNone.
inline void GammaLut(const GammaRamp& ramp, uint8_t lut[3][256]) {
    for (int c = 0; c < 3; c++) {
        for (uint32_t v = 0; v < 256; v++) {
            switch (ramp.mode) {
                case GammaRamp::kTable:
                    lut[c][v] = Unorm10To8(TableChannel(ramp.table[v], c));
                    break;
                case GammaRamp::kPwl:
                    // v / 255 as a 10-bit front buffer stores it
                    lut[c][v] = Unorm10To8(PwlChannel(ramp, (v * 1023 + 127) / 255, c));
                    break;
                default: lut[c][v] = uint8_t(v); break;
            }
        }
    }
}

inline bool IsIdentity(const uint8_t lut[3][256]) {
    for (int c = 0; c < 3; c++)
        for (uint32_t v = 0; v < 256; v++)
            if (lut[c][v] != v) return false;
    return true;
}

// rgba (R in the low byte) through the lut; alpha kept
inline void ApplyGammaLut(const uint8_t lut[3][256], std::vector<uint32_t>& rgba) {
    for (uint32_t& p : rgba) {
        p = uint32_t(lut[0][p & 0xff]) | uint32_t(lut[1][p >> 8 & 0xff]) << 8 |
            uint32_t(lut[2][p >> 16 & 0xff]) << 16 | (p & 0xff000000u);
    }
}

inline void ApplyGamma(const GammaRamp& ramp, std::vector<uint32_t>& rgba) {
    if (ramp.mode == GammaRamp::kNone) return;
    uint8_t lut[3][256];
    GammaLut(ramp, lut);
    if (!IsIdentity(lut)) ApplyGammaLut(lut, rgba);
}

}  // namespace band3::render
