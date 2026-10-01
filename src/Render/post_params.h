#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// Experimental: what RB3's post-processing is set to do on a frame, as the
// native view captures it (scene_capture.cpp), and the constants RB3's own
// composite was given, to check the one against the other
// (out/research/m4_design.md 2, m4_postproc.md 2 and 6). Plain data, so a
// .cap keeps them as they are; both structs only ever grow at the end.

namespace band3::render {

// Read at DxRnd::DoPostProcess's start, every frame: the RndPostProc that runs
// (TheRnd's override, else RndPostProc::sCurrent) and TheDOFProc, as
// BandDirector and the camera shot left them. Offsets are rb3-xenon's
// (rndobj/PostProc.h, ColorXfm.h, DOFProc_NG.h), checked against the
// recompiled RndPostProc::ColorXfmEnabled and NgDOFProc::Set.
struct PostParams {
    uint32_t valid = 0;     // read this frame (TheRnd was there)
    uint32_t disabled = 0;  // TheRnd's mDisablePostProc (+0x105): no post-processing at all
    uint32_t proc = 0;      // the RndPostProc read, 0 none (menus can have none)
    // it's TheRnd's mPostProcOverride (+0x124), which runs alone: no DOF then
    uint32_t overridden = 0;
    // the colour matrix AdjustColorXfm built (+0xB8, rows +0xB8 +0xC8 +0xD8):
    // c' = c * M + v, Milo's row vectors
    float xfm[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float xfm_offset[3] = {};  // v (+0xE8)
    // mColorModulation (+0x12C), flicker: scales M only. Read again at
    // FinishPostProcess on frames that post-process: DoPost moves it on.
    float color_mod = 1;
    // what the matrix is built from (RndColorXfm at +0x64)
    float hue = 0, saturation = 0, lightness = 0, contrast = 0, brightness = 0;
    float level_in_lo[4] = {}, level_in_hi[4] = {1, 1, 1, 1};
    float level_out_lo[4] = {}, level_out_hi[4] = {1, 1, 1, 1};
    float bloom_color[4] = {1, 1, 1, 0};  // +0x30; alpha above 0 turns bloom on too
    float bloom_threshold = 4;            // +0x40
    float bloom_intensity = 0;            // +0x44
    uint8_t bloom_glare = 0;              // +0x48
    uint8_t bloom_streak = 0;             // +0x49
    uint8_t pad[2] = {};
    float emulate_fps = 0;                // +0x168: even/odd rendering's rate
    // TheDOFProc (0x82CC6368), an NgDOFProc, as NgDOFProc::Set left it
    uint32_t dof = 0;
    uint32_t dof_enabled = 0;  // +0x2C: Set's max blur above 0
    // +0x30, +0x34: the z-buffer values of the focal plane and of where the
    // blur starts in front of it, focal * (1 - blur depth)
    float dof_scale = 0, dof_bias = 0;
    float dof_focal = 0;       // +0x38, world units
    float dof_blur_depth = 0;  // +0x3C
    float dof_min_blur = 0;    // +0x40
    float dof_max_blur = 0;    // +0x44
    float blur_width_scale = 1;  // RndPostProc::sDOFOverride's last (0x82C70440 + 0x18)
    // TheRnd's copy of the world camera (+0xA4): near, far and z range
    uint32_t cam = 0;
    float cam_near = 0, cam_far = 0;
    float cam_zrange[2] = {};
};

// What RB3's composite drew with, read at DxRnd::FinishPostProcess's start on
// frames that post-process (the pixel shader constants from the device's
// shadow, and TheShaderMgr's post flags), and what the DOF and bloom blurs
// were given, to pin their taps.
struct PostConsts {
    uint32_t valid = 0;  // FinishPostProcess ran this frame
    // c6 bloom colour, c15 half pixel, c24 DOF, c91 spotlights (x the
    // gain), c92..c94 the colour matrix, c112/c113 noise, c122 velocity
    // blur, c127 spotlights (x + y * density)
    float c6[4] = {}, c15[4] = {}, c24[4] = {}, c91[4] = {};
    float c92[3][4] = {};
    float c112[4] = {}, c113[4] = {}, c122[4] = {}, c127[4] = {};
    // TheShaderMgr (0x82C76CE0) + 0x26..0x3F: 0x26 DOF, 0x27 bloom, 0x28 glare,
    // 0x29 luminance map, 0x2A colour xfm, 0x2B posterize, 0x2D noise...
    // (m4_postproc.md 8)
    uint8_t flags[26] = {};
    uint8_t pad[2] = {};
    // c31..c38 and c47..c54 after NgDOFProc::DoPost: its last blur's taps
    // (UV offsets) and weights
    uint32_t dof_survey = 0;
    float dof_offsets[8][4] = {}, dof_weights[8][4] = {};
    // c31..c45 and c47..c61 after the frame's first Bloom_Blur (level 0, the
    // first direction)
    uint32_t bloom_survey = 0;
    float bloom_offsets[15][4] = {}, bloom_weights[15][4] = {};
    // TheShaderMgr + 0x25, below flags' range: NgSpotlightDrawer::RenderScene
    // sets it each frame it runs, and it turns on the composite's spotlight
    // term (option bit 51), rgb += s12.rgb * (c127.x + c127.y * s5.r) * c91.x
    // with s12 the blurred depth volume and s5 the density map
    // (out/research/spotlight_survey.md 2); 0 in captures from before
    uint32_t spot_flag = 0;
    // RndSoftParticleBuffer's two 320x180 surfaces (its PostProcessor + 4 and
    // + 8, DxTex), read when its DoPost runs: the particles draw into [0],
    // its blur goes [0] -> [1] -> [0], and the composite adds [0] (s4) where
    // TheShaderMgr + 0x3F is set (out/research/softparticle_survey.md 1); 0
    // when DoPost didn't run, and in captures from before
    uint32_t soft_surface[2] = {};
};

// TheShaderMgr's flag bytes, as PostConsts::flags indexes them
inline constexpr int kPostFlagBase = 0x26;
inline constexpr int kPostFlagDof = 0x26 - kPostFlagBase;
inline constexpr int kPostFlagBloom = 0x27 - kPostFlagBase;
inline constexpr int kPostFlagGlare = 0x28 - kPostFlagBase;
inline constexpr int kPostFlagColorXfm = 0x2A - kPostFlagBase;
inline constexpr int kPostFlagSoft = 0x3F - kPostFlagBase;
// and PostConsts::spot_flag's, before them
inline constexpr int kPostFlagSpot = 0x25;

// Hmx::Color::Pack, as the retail code does it: each channel times 255,
// truncated (fctiwz, which saturates), its low byte; red lowest
inline uint32_t PackColor(const float c[3]) {
    auto chan = [](float v) {
        const float s = v * 255.0f;
        int32_t i;
        if (!(s == s)) i = INT32_MIN;  // NaN converts to the most negative
        else if (s >= 2147483647.0f) i = INT32_MAX;
        else if (s <= -2147483648.0f) i = INT32_MIN;
        else i = int32_t(s);
        return uint32_t(i) & 0xff;
    };
    return chan(c[0]) | chan(c[1]) << 8 | chan(c[2]) << 16;
}

// RndPostProc::ColorXfmEnabled (rb3-xenon PostProc.cpp:709-718, retail
// 0x8242F7E8): whether the composite applies the colour matrix
inline bool ColorXfmEnabled(const PostParams& p) {
    return p.color_mod != 1 || p.hue != 0 || p.saturation != 0 || p.lightness != 0 ||
           p.contrast != 0 || p.brightness != 0 || PackColor(p.level_in_lo) != 0 ||
           PackColor(p.level_out_lo) != 0 || PackColor(p.level_in_hi) != 0xffffff ||
           PackColor(p.level_out_hi) != 0xffffff;
}

// The colour matrix as the composite reads it, c92..c94 (NgPostProc::
// ModulateColorXfm): output channel j = dot(rows[j].xyz, rgb) + rows[j].w, so
// row j is M's column j times the modulation, and v[j]
inline void ModulatedXfm(const PostParams& p, float rows[3][4]) {
    for (int j = 0; j < 3; j++) {
        for (int i = 0; i < 3; i++) rows[j][i] = p.xfm[i][j] * p.color_mod;
        rows[j][3] = p.xfm_offset[j];
    }
}

// PS c24 as NgDOFProc::DoPost sets it: (1/(scale-bias), -scale/(scale-bias),
// min(max, min), max < 0 ? 1 : max). The composite blurs by
// sat(min(max(|t|, z), w)), t = (1 - depth) * x + y.
inline void DofConstants(const PostParams& p, float out[4]) {
    const float range = 1.0f / (p.dof_scale - p.dof_bias);
    out[0] = range;
    out[1] = -p.dof_scale * range;
    out[2] = std::min(p.dof_max_blur, p.dof_min_blur);
    out[3] = p.dof_max_blur >= 0 ? p.dof_max_blur : 1.0f;
}

// PS c6 as NgPostProc::DoBloom sets it: the bloom colour times its intensity
inline void BloomConstant(const PostParams& p, float out[4]) {
    for (int i = 0; i < 3; i++) out[i] = p.bloom_color[i] * p.bloom_intensity;
    out[3] = 0;
}

// RndColorXfm::AdjustColorXfm (rb3-xenon ColorXfm.cpp): the colour matrix
// from hue, saturation, lightness, contrast, brightness and levels, in that
// order, each applied after the ones before (Milo's Multiply(a, b): a then
// b). RB3 keeps the result in the proc; this is for checking it.
inline void AdjustColorXfm(const PostParams& p, float m[3][3], float v[3]) {
    struct Xfm {
        float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        float v[3] = {0, 0, 0};
    };
    Xfm acc;
    auto then = [&](const Xfm& b) {
        Xfm r;
        for (int i = 0; i < 3; i++) {
            for (int j = 0; j < 3; j++) {
                r.m[i][j] = acc.m[i][0] * b.m[0][j] + acc.m[i][1] * b.m[1][j] +
                            acc.m[i][2] * b.m[2][j];
            }
        }
        for (int j = 0; j < 3; j++)
            r.v[j] = acc.v[0] * b.m[0][j] + acc.v[1] * b.m[1][j] + acc.v[2] * b.m[2][j] + b.v[j];
        acc = r;
    };
    auto chan = [](int c) { return ((c % 3) + 3) % 3; };
    constexpr float kQuarter = 1.5707964f;

    Xfm hue;
    if (p.hue != 0) {
        const bool past_120 = p.hue >= 120.0f || p.hue <= -120.0f;
        const float a = (past_120 ? (std::fabs(p.hue) - 120.0f) : std::fabs(p.hue)) / 120.0f * kQuarter;
        const float c = std::cos(a), s = std::sin(a);
        for (int i = 0; i < 3; i++) {
            float& self = hue.m[i][i];
            float& next = hue.m[chan(i + 1)][i];
            float& after = hue.m[chan(i + 2)][i];
            if (p.hue >= 120.0f) self = 0, next = c, after = s;
            else if (p.hue > 0) self = c, next = s, after = 0;
            else if (p.hue <= -120.0f) self = 0, next = s, after = c;
            else self = c, next = 0, after = s;
        }
    }
    then(hue);

    Xfm sat;
    float k = p.saturation / 100.0f;
    k = k > 0 ? k + 1.0f : -(k * -0.6666666f - 1.0f);
    const float spill = (1.0f - k) * 0.5f;
    for (int i = 0; i < 3; i++) {
        sat.m[i][i] = k;
        sat.m[i][chan(i + 1)] = spill;
        sat.m[i][chan(i + 2)] = spill;
    }
    then(sat);

    Xfm light;
    const float l = p.lightness / 100.0f;
    const float lk = l >= 0 ? 1.0f - l : l + 1.0f, lv = l >= 0 ? l : 0.0f;
    for (int i = 0; i < 3; i++) {
        light.m[i][i] = lk;
        light.v[i] = lv;
    }
    then(light);

    Xfm contrast;
    float t = p.contrast / 100.0f;
    t = t > 0 ? 1.0f / (t * -0.9921875f + 1.0f) : -(t * -0.992126f - 1.0f);
    for (int i = 0; i < 3; i++) {
        contrast.m[i][i] = t;
        contrast.v[i] = (1.0f - t) * 0.5f;
    }
    then(contrast);

    Xfm bright;
    const float b = (p.brightness + 100.0f) / 200.0f + -0.5f;
    for (int i = 0; i < 3; i++) bright.v[i] = b;
    then(bright);

    Xfm levels;
    for (int i = 0; i < 3; i++) {
        const float in = p.level_in_hi[i] - p.level_in_lo[i];
        const float a = in != 0 ? (p.level_out_hi[i] - p.level_out_lo[i]) / in : 0.0f;
        levels.m[i][i] = a;
        levels.v[i] = -(p.level_in_lo[i] * a - p.level_out_lo[i]);
    }
    then(levels);

    std::memcpy(m, acc.m, sizeof(acc.m));
    std::memcpy(v, acc.v, sizeof(acc.v));
}

}  // namespace band3::render
