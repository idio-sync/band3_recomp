#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// RB3's post-processing settings for a frame and the constants its composite
// was given (out/research/m4_postproc.md). Plain data stored as-is in a .cap:
// both structs only ever grow at the end.

namespace band3::render {

// Read at DxRnd::DoPostProcess's start: the RndPostProc that runs (TheRnd's
// override, else RndPostProc::sCurrent) and TheDOFProc. Offsets are
// rb3-xenon's (rndobj/PostProc.h, ColorXfm.h, DOFProc_NG.h).
struct PostParams {
    uint32_t valid = 0;     // TheRnd was there
    uint32_t disabled = 0;  // TheRnd's mDisablePostProc (+0x105)
    uint32_t proc = 0;      // 0 none (menus can have none)
    // TheRnd's mPostProcOverride (+0x124), which runs alone: no DOF then
    uint32_t overridden = 0;
    // AdjustColorXfm's matrix (+0xB8, rows 16 bytes apart): c' = c * M + v,
    // Milo's row vectors
    float xfm[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    float xfm_offset[3] = {};  // v (+0xE8)
    // mColorModulation (+0x12C), flicker: scales M only. Re-read at
    // FinishPostProcess, as DoPost moves it on.
    float color_mod = 1;
    // the matrix's inputs (RndColorXfm at +0x64)
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
    // +0x30, +0x34: z-buffer values of the focal plane and of where blur
    // starts, focal * (1 - blur depth)
    float dof_scale = 0, dof_bias = 0;
    float dof_focal = 0;       // +0x38, world units
    float dof_blur_depth = 0;  // +0x3C
    float dof_min_blur = 0;    // +0x40
    float dof_max_blur = 0;    // +0x44
    float blur_width_scale = 1;  // RndPostProc::sDOFOverride's last (0x82C70440 + 0x18)
    // TheRnd's copy of the world camera (+0xA4)
    uint32_t cam = 0;
    float cam_near = 0, cam_far = 0;
    float cam_zrange[2] = {};
    // Film grain, as NgPostProc::CheckNoise reads it
    // (out/research/n1_post_noise.md): base scale (+0x130, +0x134), top
    // scale (+0x138), intensity (+0x13C, 0 off), stationary (+0x140),
    // midtone (+0x141), the map's RndTex (+0x14C, 0 none) and its texture's
    // physical base address, and the stationary seeds (NgPostProc +0x20C,
    // +0x210)
    float noise_base[2] = {};
    float noise_top = 0;
    float noise_intensity = 0;
    uint8_t noise_stationary = 0;
    uint8_t noise_midtone = 0;
    uint8_t pad2[2] = {};
    uint32_t noise_map = 0;
    uint32_t noise_map_base = 0;
    float noise_seeds[2] = {};
    // the trails (blend previous): mTrailThreshold (+0x150) and
    // mTrailDuration (+0x154)
    float trail_threshold = 0;
    float trail_duration = 0;
    // Camera motion blur (NgPostProc::DoVelocity, RndVelocityBuffer::Draw;
    // out/research/n5_hub_soft.md): RndVelocityBuffer::sSingleton
    // (0x82E12BA0) at DoPostProcess's start. vel_read: it has its velocity
    // texture (+0x36C74); vel_on: the proc's mMotionBlurVelocity (+0x1A4);
    // vel_pre_depth: DxRnd's pre-pass depth texture (+0x340), without which
    // Draw draws nothing; vel_same_cam: its mCam (+0xA8) and
    // mLastFrameCamera (+0x36C7C) are TheRnd's world camera; vel_frame: its
    // mFrame (+0x36C70, frames since CamShot::StartAnim); vel_scale: its last
    // c122 (+0x36BE8, min(2, 41.67 / (ms + 1))). Zero in older captures.
    uint8_t vel_read = 0;
    uint8_t vel_on = 0;
    uint8_t vel_pre_depth = 0;
    uint8_t vel_same_cam = 0;
    uint32_t vel_frame = 0;
    float vel_scale = 0;
    // mViewProjXfm (+0x8) and the previous frame's, unk36bec[idx ^ 1]
    // (+0x36BEC + 64 * (idx ^ 1), idx +0x36C6C), which Draw uploads as PS
    // c134..c137 (Hmx::Matrix4, rows)
    float vel_view_proj[4][4] = {};
    float vel_prev_view_proj[4][4] = {};
    // mDepthRangeValues (+0x48), DrawRectDepth's PS c89 (near, far, depth
    // scale and bias back to z), and the frustum: mFrustumNear (+0x58) and
    // the four corner rays mFrustumCorners (+0x68, 16 bytes apart),
    // DrawRectDepth's TEXCOORD1 and 2
    float vel_depth_range[4] = {};
    float vel_near[4] = {};
    float vel_corners[4][4] = {};
};

// What RB3's composite drew with, read at DxRnd::FinishPostProcess's start
// (PS constants from the device's shadow, TheShaderMgr's post flags), and
// what the DOF and bloom blurs were given.
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
    // TheShaderMgr + 0x25, set by NgSpotlightDrawer::RenderScene: the
    // composite's spotlight term (option bit 51), rgb += s12.rgb * (c127.x +
    // c127.y * s5.r) * c91.x, s12 the blurred depth volume and s5 the
    // density map (out/research/spotlight_survey.md); 0 in older captures
    uint32_t spot_flag = 0;
    // RndSoftParticleBuffer's two 320x180 surfaces (PostProcessor + 4, + 8,
    // DxTex) when its DoPost runs: particles draw into [0], the blur goes
    // [0] -> [1] -> [0], the composite adds [0] (s4) where TheShaderMgr +
    // 0x3F is set (out/research/softparticle_survey.md); 0 if DoPost didn't
    // run, and in older captures
    uint32_t soft_surface[2] = {};
    // sampler 13's fetch constant where TheShaderMgr + 0x2D is set: the noise
    // map CheckNoise bound (FrameCapture::noise_map has its pixels); zero
    // otherwise, and in older captures
    uint32_t noise_fetch[6] = {};
    // c125, from NgPostProc::CheckBlendPrevious: (threshold, dt / duration,
    // 1/3, 0), read with the previous post frame (s14) where TheShaderMgr +
    // 0x2F is set; zero in older captures
    float c125[4] = {};
    // The velocity blur's, to check PostParams' against (zero in older
    // captures): PS c89 and c134..c137 as RndVelocityBuffer::Draw left them,
    // and the fetch constants of sampler 6 (the scene) and 10 (the velocity
    // texture)
    float c89[4] = {};
    float c134[4][4] = {};
    uint32_t scene_fetch[6] = {};
    uint32_t velocity_fetch[6] = {};
};

// TheShaderMgr's flag bytes, as PostConsts::flags indexes them
inline constexpr int kPostFlagBase = 0x26;
inline constexpr int kPostFlagDof = 0x26 - kPostFlagBase;
inline constexpr int kPostFlagBloom = 0x27 - kPostFlagBase;
inline constexpr int kPostFlagGlare = 0x28 - kPostFlagBase;
inline constexpr int kPostFlagColorXfm = 0x2A - kPostFlagBase;
inline constexpr int kPostFlagNoise = 0x2D - kPostFlagBase;
inline constexpr int kPostFlagNoiseMidtone = 0x2E - kPostFlagBase;
// the trails: the previous post frame, faded, kept where it's brighter
inline constexpr int kPostFlagBlendPrevious = 0x2F - kPostFlagBase;
// camera motion blur (NgPostProc::DoVelocity)
inline constexpr int kPostFlagVelocity = 0x39 - kPostFlagBase;
inline constexpr int kPostFlagSoft = 0x3F - kPostFlagBase;
// PostConsts::spot_flag's, outside flags
inline constexpr int kPostFlagSpot = 0x25;

// Hmx::Color::Pack as retail does it: channel * 255 truncated (fctiwz,
// saturating), low byte; red lowest
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

// RndPostProc::ColorXfmEnabled (retail 0x8242F7E8)
inline bool ColorXfmEnabled(const PostParams& p) {
    return p.color_mod != 1 || p.hue != 0 || p.saturation != 0 || p.lightness != 0 ||
           p.contrast != 0 || p.brightness != 0 || PackColor(p.level_in_lo) != 0 ||
           PackColor(p.level_out_lo) != 0 || PackColor(p.level_in_hi) != 0xffffff ||
           PackColor(p.level_out_hi) != 0xffffff;
}

// c92..c94 (NgPostProc::ModulateColorXfm): out[j] = dot(rows[j].xyz, rgb) +
// rows[j].w, so row j is M's column j times the modulation
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

// PS c6 as NgPostProc::DoBloom sets it
inline void BloomConstant(const PostParams& p, float out[4]) {
    for (int i = 0; i < 3; i++) out[i] = p.bloom_color[i] * p.bloom_intensity;
    out[3] = 0;
}

// as NgPostProc::CheckNoise decides
inline bool NoiseEnabled(const PostParams& p) {
    return p.noise_intensity != 0 && p.noise_map != 0;
}

// as RndPostProc::BlendPrevious decides (TheShaderMgr + 0x2F)
inline bool BlendPrevious(const PostParams& p) {
    return p.trail_threshold < 1 && p.trail_duration > 0;
}

// whether NgPostProc::DoVelocity would set TheShaderMgr + 0x39, as far as
// DoPostProcess's start can tell. vel_frame >= 1: after AdvanceFrame this is
// the shot's second frame or later.
inline bool VelocityExpected(const PostParams& p) {
    return p.vel_read && p.vel_on && p.vel_pre_depth && p.vel_same_cam && p.vel_frame >= 1;
}

// PS c113 as NgPostProc::CheckNoise sets it
inline void NoiseConstant(const PostParams& p, float out[4]) {
    out[0] = p.noise_base[0];
    out[1] = p.noise_base[1];
    out[2] = p.noise_stationary ? 1.0f : p.noise_top;
    out[3] = p.noise_intensity;
}

// RndColorXfm::AdjustColorXfm (rb3-xenon ColorXfm.cpp), to check the proc's
// matrix: each step applied after the ones before (Milo's Multiply(a, b): a
// then b)
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
