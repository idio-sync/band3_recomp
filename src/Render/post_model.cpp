#include "src/Render/post_model.h"

#include <algorithm>
#include <cmath>

#include "src/Render/frame_compose.h"

// See post_model.h.

namespace band3::render::post {
namespace {

using shade::float2;
using shade::float3;

// HLSL's operators and functions, for post_model.hlsli: only what it uses
float3 operator+(float3 a, float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
float3 operator*(float3 a, float3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
float3 operator*(float3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float4 operator+(float4 a, float4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
float4 operator*(float4 a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }

float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }
float3 saturate(float3 v) { return {saturate(v.x), saturate(v.y), saturate(v.z)}; }
float abs(float v) { return std::fabs(v); }
float min(float a, float b) { return a < b ? a : b; }
float max(float a, float b) { return a > b ? a : b; }
float3 lerp(float3 a, float3 b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

#define POST_IN(T) const T&
#include "src/Render/shaders/post_model.hlsli"
#undef POST_IN

float4 Lerp4(float4 a, float4 b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t};
}

// the Poisson discs SetVHBlurWeights scales (rb3-xenon DOFProc_NG.cpp)
constexpr float kDofTapsAcross[8][2] = {
    {0.4432332516f, -0.9751155376f}, {0.5374298096f, -0.4737342f},
    {-0.2649691105f, -0.4189302325f}, {0.7919751406f, 0.1909018755f},
    {-0.2418884039f, 0.9970650673f}, {-0.8140995502f, 0.9143759012f},
    {0.1998412609f, 0.7864136696f}, {0.1438316107f, -0.1410079002f}};
constexpr float kDofTapsDown[8][2] = {
    {-0.9420162439f, -0.3990621567f}, {0.9455860853f, -0.768907249f},
    {-0.09418410063f, -0.9293887019f}, {0.3449593782f, 0.2938776016f},
    {-0.9158858061f, 0.4577143192f}, {-0.8154423237f, -0.8791246414f},
    {-0.3827754259f, 0.276768446f}, {0.9748439789f, 0.7564837933f}};

// A post-processing level on the CPU: RGBA as floats, each an 8-bit value as
// the GPU's RGBA8 targets (and the 360's) keep it
struct Level {
    uint32_t w = 0, h = 0;
    std::vector<float4> px;
    void Resize(uint32_t width, uint32_t height) {
        w = width;
        h = height;
        px.resize(size_t(w) * h);
    }
};

// what an RGBA8 target keeps of a value: the nearest of 256 steps
float Unorm8(float v) { return std::floor(saturate(v) * 255.0f + 0.5f) / 255.0f; }
float4 Unorm8(float4 v) { return {Unorm8(v.x), Unorm8(v.y), Unorm8(v.z), Unorm8(v.w)}; }

float4 Unpack(uint32_t c) {
    return {float(c & 0xff) / 255.0f, float(c >> 8 & 0xff) / 255.0f,
            float(c >> 16 & 0xff) / 255.0f, float(c >> 24) / 255.0f};
}

float4 Load(const Level& l, int x, int y) {
    x = std::clamp(x, 0, int(l.w) - 1);
    y = std::clamp(y, 0, int(l.h) - 1);
    return l.px[size_t(y) * l.w + x];
}

// bilinear, clamped to the edge: the GPU's linear clamp sampler, which RB3
// sets for these taps
float4 Sample(const Level& l, float2 uv) {
    const float x = uv.x * float(l.w) - 0.5f, y = uv.y * float(l.h) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const int x0 = int(fx), y0 = int(fy);
    const float tx = x - fx, ty = y - fy;
    const float4 top = Lerp4(Load(l, x0, y0), Load(l, x0 + 1, y0), tx);
    const float4 bottom = Lerp4(Load(l, x0, y0 + 1), Load(l, x0 + 1, y0 + 1), tx);
    return Lerp4(top, bottom, ty);
}

// the pixel's centre in uv
float2 Uv(uint32_t x, uint32_t y, const Level& l) {
    return {(float(x) + 0.5f) / float(l.w), (float(y) + 0.5f) / float(l.h)};
}

// the bright pass or the 4x downsample, from `src` into `dst` (sized)
void Downsample(const Level& src, bool bright, Level& dst) {
    const float4 half_pixel{0.5f / float(src.w), 0.5f / float(src.h), 0, 0};
    for (uint32_t y = 0; y < dst.h; y++) {
        for (uint32_t x = 0; x < dst.w; x++) {
            const float2 uv = Uv(x, y, dst);
            float4 t[4];
            for (int i = 0; i < 4; i++) t[i] = Sample(src, QuadTap(uv, half_pixel, i));
            dst.px[size_t(y) * dst.w + x] = Unorm8(Quad(t[0], t[1], t[2], t[3], bright));
        }
    }
}

// the Gaussian or the DOF's blur: `n` taps (uv offset, weight) at the same size
void Blur(const Level& src, const float4* taps, int n, Level& dst) {
    dst.Resize(src.w, src.h);
    for (uint32_t y = 0; y < dst.h; y++) {
        for (uint32_t x = 0; x < dst.w; x++) {
            const float2 uv = Uv(x, y, dst);
            float4 sum{0, 0, 0, 0};
            for (int i = 0; i < n; i++)
                sum = sum + Sample(src, float2{uv.x + taps[i].x, uv.y + taps[i].y}) * taps[i].z;
            dst.px[size_t(y) * dst.w + x] = Unorm8(sum);
        }
    }
}

float3 Rgb(const float* v) { return {v[0], v[1], v[2]}; }
float4 Rgba(const float* v) { return {v[0], v[1], v[2], v[3]}; }

}  // namespace

void BloomTaps(bool vertical, uint32_t size, float4 taps[15]) {
    for (int i = 0; i < 15; i++) {
        const float offset = (float(i) - 6.5f) / float(size);
        taps[i] = {vertical ? 0.0f : offset, vertical ? offset : 0.0f, kBloomWeights[i], 0};
    }
}

void DofTaps(bool vertical, float width_scale, float4 taps[8]) {
    // SetVHBlurWeights's, for the game's DOF level: 4.8828124e-06 is
    // 1/204800 and 1.5432099e-05 1/64800
    const float f = width_scale * kDofWidthFactor;
    const float sx = float(Quarter(kGameWidth)) * f * 4.8828124e-06f;
    const float sy = float(Quarter(kGameHeight)) * f * 1.5432099e-05f;
    const auto& disc = vertical ? kDofTapsDown : kDofTapsAcross;
    for (int i = 0; i < 8; i++)
        taps[i] = {disc[i][0] * sx * 5.0f, disc[i][1] * sy * 5.0f, 0.125f, 0};
}

bool PlanPost(const FrameCapture& frame, uint32_t only, PostPlan& plan) {
    plan = PostPlan{};
    const PostParams& p = frame.post;
    const PostConsts& c = frame.post_consts;
    if (!p.valid || p.disabled || !p.proc) return false;
    // The game post-processes where FinishPostProcess runs, on frames whose
    // ProcCommands has kProcPost (7, or 2 with even/odd rendering; a composed
    // frame is its post frame), and the capture has the composite's constants
    // there; a post frame without them drew no post, nor does a frame that
    // does neither (0). A world frame (1) draws none either, but the next
    // frame post-processes its world, by then with that frame's numbers: the
    // live view shows world frames too, so they get the post worked out from
    // their own PostParams, or the view would flicker between post and none.
    const bool world_only =
        (frame.proc_cmds & kProcWorld) && !(frame.proc_cmds & kProcPost);
    if (!world_only && (!(frame.proc_cmds & kProcPost) || !c.valid)) return false;
    const bool consts = !world_only;
    // the flags the game's composite was picked by, or as NgDOFProc::DoPost
    // and NgPostProc::DoBloom would set them
    uint32_t flags = 0;
    if (consts) {
        if (c.flags[kPostFlagDof]) flags |= kPostDof;
        if (c.flags[kPostFlagBloom]) flags |= kPostBloom;
        if (c.flags[kPostFlagGlare]) flags |= kPostGlare;
        if (c.flags[kPostFlagColorXfm]) flags |= kPostXfm;
    } else {
        // the override runs alone: no DOF then
        if (p.dof && p.dof_enabled && !p.overridden) flags |= kPostDof;
        if (p.bloom_intensity > 0 || p.bloom_color[3] > 0)
            flags |= p.bloom_glare ? kPostGlare : kPostBloom;
        if (ColorXfmEnabled(p)) flags |= kPostXfm;
    }
    // depth of field needs the world camera's planes to read the depth by
    if (!(p.cam_near > 0 && p.cam_far > p.cam_near)) flags &= ~kPostDof;
    // The spotlights' term reads the depth volume NgSpotlightDrawer drew
    // after DoPostProcess started (its last version: the blurs' in place),
    // and the density map drawn before its cones. On world frames the
    // drawer doesn't run (even/odd: post frames only), so they have none.
    uint32_t spot_volume = 0, spot_density = 0;
    if (consts && c.spot_flag) {
        for (size_t i = frame.passes.size(); i-- > 0 && !spot_volume;) {
            const Pass& v = frame.passes[i];
            if (v.tex_type != kTexTypeDepthVolume || v.first_draw < frame.post_boundary) continue;
            spot_volume = v.tex_obj;
            for (size_t j = i; j-- > 0;) {
                if (frame.passes[j].tex_type != kTexTypeDensityMap) continue;
                spot_density = frame.passes[j].tex_obj;
                break;
            }
        }
        if (spot_volume) flags |= kPostSpot;
    }
    if (only) flags &= only;
    if (!flags) return false;

    PostPass& pass = plan.composite;
    pass.flags = {flags, 0, 0, 0};
    float c6[4], c24[4], rows[3][4];
    if (consts) {
        std::copy(std::begin(c.c6), std::end(c.c6), c6);
        std::copy(std::begin(c.c24), std::end(c.c24), c24);
        for (int j = 0; j < 3; j++) std::copy(std::begin(c.c92[j]), std::end(c.c92[j]), rows[j]);
    } else {
        BloomConstant(p, c6);
        DofConstants(p, c24);
        ModulatedXfm(p, rows);
    }
    pass.c6 = Rgba(c6);
    pass.c24 = Rgba(c24);
    for (int j = 0; j < 3; j++) pass.xfm[j] = Rgba(rows[j]);
    pass.camera = {p.cam_near, p.cam_far, p.cam_zrange[0], p.cam_zrange[1]};
    if (flags & kPostSpot) {
        pass.spot = {c.c127[0], c.c127[1], c.c91[0], 0};
        plan.spot_volume = spot_volume;
        plan.spot_density = spot_density;
    }

    DofTaps(false, p.blur_width_scale, plan.dof_taps[0]);
    DofTaps(true, p.blur_width_scale, plan.dof_taps[1]);
    uint32_t w = kGameWidth, h = kGameHeight;
    for (int level = 0; level < 3; level++) {
        w = Quarter(w);
        h = Quarter(h);
        BloomTaps(false, w, plan.bloom_taps[level][0]);
        BloomTaps(true, h, plan.bloom_taps[level][1]);
    }
    return true;
}

void RunPost(const PostPlan& plan, const std::vector<uint32_t>& scene,
             const std::vector<float>& depth, uint32_t width, uint32_t height,
             const PostImage& volume, const PostImage& density, std::vector<uint32_t>& out) {
    const PostPass& pass = plan.composite;
    const uint32_t flags = pass.flags.x;
    Level src;
    src.Resize(width, height);
    for (size_t i = 0; i < src.px.size(); i++) src.px[i] = Unpack(scene[i]);

    Level dof, tmp, bloom[3];
    if (flags & kPostDof) {
        // the scene 4x smaller, blurred across then down
        dof.Resize(Quarter(width), Quarter(height));
        Downsample(src, false, dof);
        Blur(dof, plan.dof_taps[0], 8, tmp);
        Blur(tmp, plan.dof_taps[1], 8, dof);
    }
    const bool bloom_on = (flags & (kPostBloom | kPostGlare)) != 0;
    if (bloom_on) {
        // the bright pass into level 0; with bloom (not glare) two more, each
        // 4x smaller than the one before; each blurred across then down
        const int levels = (flags & kPostBloom) ? 3 : 1;
        for (int k = 0; k < levels; k++) {
            const Level& from = k ? bloom[k - 1] : src;
            bloom[k].Resize(Quarter(from.w), Quarter(from.h));
            Downsample(from, k == 0, bloom[k]);
            Blur(bloom[k], plan.bloom_taps[k][0], 15, tmp);
            Blur(tmp, plan.bloom_taps[k][1], 15, bloom[k]);
        }
    }

    // the spotlights' depth volume and density map, as they were drawn
    Level spot[2];
    if (flags & kPostSpot) {
        const PostImage* images[2] = {&volume, &density};
        for (int k = 0; k < 2; k++) {
            const PostImage& image = *images[k];
            if (!image.px || !image.w || !image.h) continue;
            spot[k].Resize(image.w, image.h);
            for (size_t i = 0; i < spot[k].px.size(); i++) spot[k].px[i] = Unpack(image.px[i]);
        }
    }

    out.resize(size_t(width) * height);
    const float4 none{0, 0, 0, 0};
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const size_t i = size_t(y) * width + x;
            const float2 uv = Uv(x, y, src);
            const float4 d = (flags & kPostDof) ? Sample(dof, uv) : none;
            float3 l[3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
            if (bloom_on) {
                for (int k = 0; k < 3; k++) {
                    if (bloom[k].px.empty()) continue;
                    const float4 b = Sample(bloom[k], uv);
                    l[k] = {b.x, b.y, b.z};
                }
            }
            float3 vol{0, 0, 0};
            float dens = 0;
            if (!spot[0].px.empty()) {
                const float4 v = Sample(spot[0], uv);
                vol = {v.x, v.y, v.z};
            }
            if (!spot[1].px.empty()) dens = Sample(spot[1], uv).x;
            const float3 rgb = Composite(pass, src.px[i], d, GameDepth(pass, depth[i]), l[0], l[1],
                                         l[2], vol, dens);
            out[i] = uint32_t(rgb.x * 255.0f + 0.5f) | uint32_t(rgb.y * 255.0f + 0.5f) << 8 |
                     uint32_t(rgb.z * 255.0f + 0.5f) << 16 | 0xff000000u;
        }
    }
}

float GameDepthCpu(const PostPass& pass, float inv_w) { return GameDepth(pass, inv_w); }

float DofAmountCpu(const float c24[4], float depth) { return DofAmount(Rgba(c24), depth); }

void CompositeCpu(const PostPass& pass, const float scene[4], const float dof[4], float depth,
                  const float l0[3], const float l1[3], const float l2[3], const float volume[3],
                  float density, float out[3]) {
    const float3 r = Composite(pass, Rgba(scene), Rgba(dof), depth, Rgb(l0), Rgb(l1), Rgb(l2),
                               Rgb(volume), density);
    out[0] = r.x;
    out[1] = r.y;
    out[2] = r.z;
}

}  // namespace band3::render::post
