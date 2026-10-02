#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

// Experimental: the display gamma ramp, the last thing between RB3's front
// buffer and the screen. The 360's display controller maps each channel of
// every pixel it scans out through a ramp the game sets (D3DDevice_SetGammaRamp
// at 0x82739D28, RB3's own, from a pow curve of a gamma in its system config;
// D3D's presentation setup at 0x8286D98C sets one first). ReXGlue's presenter
// applies it to the guest output at each swap (the command processor's
// gamma_ramp_256_entry_table() or gamma_ramp_pwl_rgb()), and the test
// harness's screenshot is that picture (Presenter::CaptureGuestOutput). So
// the native view applies it last too, after the overlay, as both the screen
// and the screenshot have the HUD under it.
//
// Which ramp applies follows the front buffer's format, as D3D's setup picks
// (0x8286D964) and the presenter does after it: the PWL one for a 10-bit
// front buffer (2_10_10_10, 2_10_10_10_AS_16_16_16_16), the 256-entry table
// otherwise. The capture can't see the front buffer's format, but the guest
// writes the registers of the ramp it uses only, and DC_LUT_RW_MODE says
// which (0 table, 1 PWL; see register_table.inc), which scene_capture.cpp
// reads with them.
//
// The 256-entry table maps each 8-bit value to 10 bits; the presenter's
// output is 10-bit (R10G10B10A2), which CaptureGuestOutput makes 8-bit as
// trunc(v * (255/1023.0f) + 0.5f) (rexruntime.dll's
// D3D12Presenter::CaptureGuestOutput). The native view's picture is 8-bit, as
// the game's front buffer is, so the ramp is a lookup per channel and value
// (GammaLut) both backends apply the same way: the CPU in Rasterize, the GPU
// by shaders/gamma.hlsl's pass before its readback.

namespace band3::render {

struct GammaRamp {
    // which ramp the presenter applies: none (a capture from before it, or
    // the ramp couldn't be read: identity), the table, or PWL
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

// a table entry's channel c (0 red, 1 green, 2 blue), 10-bit
inline uint32_t TableChannel(uint32_t entry, int c) { return entry >> (20 - 10 * c) & 0x3ff; }

// 10 bits to 8 as CaptureGuestOutput does: in float, multiply and add apart
// (one expression could be fused into an FMA and round otherwise)
inline uint8_t Unorm10To8(uint32_t v) {
    const float scaled = float(v) * (255.0f / 1023.0f);
    const float rounded = scaled + 0.5f;
    return uint8_t(rounded);
}

// The PWL ramp's output for a 10-bit input on channel c, 10-bit: the step's
// base plus its delta times the input's low 3 bits over 8 (the increment of
// DC_LUTA_CONTROL 3 that D3D sets), as Xenia's apply_gamma_pwl shader does;
// base and delta are 10.6 fixed point. Unchecked against the game: RB3's
// front buffer is 8-bit, which the table serves.
inline uint32_t PwlChannel(const GammaRamp& ramp, uint32_t v10, int c) {
    const uint32_t entry = ramp.pwl[v10 >> 3 & 127][c];
    const uint32_t base = entry & 0xffff, delta = entry >> 16;
    const uint32_t out = (base + ((delta * (v10 & 7)) >> 3)) >> 6;
    return out > 1023 ? 1023 : out;
}

// What the screen (and the screenshot) shows for each 8-bit value the front
// buffer holds, per channel: lut[c][v]. Identity for kNone.
inline void GammaLut(const GammaRamp& ramp, uint8_t lut[3][256]) {
    for (int c = 0; c < 3; c++) {
        for (uint32_t v = 0; v < 256; v++) {
            switch (ramp.mode) {
                case GammaRamp::kTable:
                    lut[c][v] = Unorm10To8(TableChannel(ramp.table[v], c));
                    break;
                case GammaRamp::kPwl:
                    // the 10-bit front buffer's value for it, as a shader
                    // writing v / 255 stores it
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

// the ramp applied to rgba as the presenter applies it; nothing for kNone
inline void ApplyGamma(const GammaRamp& ramp, std::vector<uint32_t>& rgba) {
    if (ramp.mode == GammaRamp::kNone) return;
    uint8_t lut[3][256];
    GammaLut(ramp, lut);
    if (!IsIdentity(lut)) ApplyGammaLut(lut, rgba);
}

}  // namespace band3::render
