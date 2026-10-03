#include "src/Render/post_model.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "src/Render/frame_compose.h"
#include "src/Render/sample_model.h"

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
float dot(float4 a, float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
float saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }
float3 saturate(float3 v) { return {saturate(v.x), saturate(v.y), saturate(v.z)}; }
float abs(float v) { return std::fabs(v); }
float sqrt(float v) { return std::sqrt(v); }
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

// What an RGBA8 target keeps of a value: the nearest of 256 steps, and of a
// value halfway between two, to float error, the lower, as the GPU backend's
// levels have it. The 4x downsample averages four texels, so a quarter of its
// values are halfway; rounding them up, as the CPU did, left its bloom and
// DOF levels a step brighter there, which the colour matrix of a desaturated
// frame (rows summing to over 2) made a mean of 0.4 over the picture
// (out/parity4's default-25s).
float Unorm8(float v) { return std::floor(saturate(v) * 255.0f + (0.5f - 1.0f / 1024)) / 255.0f; }
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

// the glare pass over level 0 `src`, into `dst`
void Glare(const Level& src, Level& dst) {
    dst.Resize(src.w, src.h);
    for (uint32_t y = 0; y < dst.h; y++) {
        for (uint32_t x = 0; x < dst.w; x++) {
            const float2 uv = Uv(x, y, dst);
            const float2 stride = GlareStep(uv);
            float2 at = uv;
            float3 sum{0, 0, 0};
            for (int k = 0; k < kGlareTaps; k++) {
                const float4 t = Sample(src, at);
                sum = sum + GlareTerm(float3{t.x, t.y, t.z}, GlareWeight(at));
                at = float2{at.x + stride.x, at.y + stride.y};
            }
            const float3 rgb = GlareOut(sum);
            dst.px[size_t(y) * dst.w + x] = Unorm8(float4{rgb.x, rgb.y, rgb.z, 1});
        }
    }
}

float3 Rgb(const float* v) { return {v[0], v[1], v[2]}; }
float4 Rgba(const float* v) { return {v[0], v[1], v[2], v[3]}; }

// a world frame's noise seeds (c112), which the game draws at random each
// frame: four numbers in 0..1 from its frame number, so the grain moves as
// the game's does and both backends draw the same
float4 FrameSeeds(uint64_t frame) {
    uint64_t h = frame * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
    float out[4];
    for (float& v : out) {
        h ^= h >> 31;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 29;
        v = float(h >> 40) / float(1u << 24);
    }
    return {out[0], out[1], out[2], out[3]};
}

// A vertex of object o placed by its palettes (VS 21A0C657 / F922317D): the
// world position by this frame's (rows 0..) and the last frame's (rows
// bones * 3 on), each through its view-projection
void ObjectVertex(const VelocityObject& o, const VelocityObjectPass& pass, const Vertex& v,
                  float4& cur, float4& prev) {
    const float4 at{v.pos[0], v.pos[1], v.pos[2], 1.0f};
    auto row = [&](bool last, uint32_t bone, int r) {
        const size_t i = (size_t(last ? o.bones * 3 : 0) + size_t(bone) * 3 + r) * 4;
        return float4{o.rows[i], o.rows[i + 1], o.rows[i + 2], o.rows[i + 3]};
    };
    auto place = [&](bool last) {
        float3 world{0, 0, 0};
        if (!o.skinned) {
            world = {dot(row(last, 0, 0), at), dot(row(last, 0, 1), at), dot(row(last, 0, 2), at)};
        } else {
            const float4 w = VelocityObjectWeights(
                float4{v.weight[0], v.weight[1], v.weight[2], v.weight[3]});
            const float ws[4] = {w.x, w.y, w.z, w.w};
            for (int k = 0; k < 4; k++) {
                const uint32_t b = v.bone[k] < o.bones ? v.bone[k] : 0;
                world = world + float3{dot(row(last, b, 0), at), dot(row(last, b, 1), at),
                                       dot(row(last, b, 2), at)} *
                                    ws[k];
            }
        }
        return VelocityObjectClip(pass, world, last);
    };
    cur = place(false);
    prev = place(true);
}

// The object pass on the CPU, over `vel` (the camera pass's texels, RGBA8 as
// floats), from the scene's `depth` (1/w, width x height): each object's
// triangles clipped at the camera's near plane (w at least c8.x, the clip z
// both backends give, VelocityObjectDepth), on D3D9's pixel centres as the
// game's mesh draws are (soft_raster.cpp's PixelCentre), culled by its cull
// mode, filled by D3D's top-left rule, their depth tested (smaller or equal
// passes) and written in a buffer of their own, cleared to 1, and where a
// pixel passes, the texel VelocityObjectTexel gives replacing the camera's
// where its alpha is 1 (SrcAlpha, by 1 or 0)
void ObjectPass(const PostPlan& plan, Level& vel, const std::vector<float>& depth,
                uint32_t width, uint32_t height) {
    if (plan.velocity_objects.empty()) return;
    std::vector<float> zbuf(vel.px.size(), 1.0f);
    const float vw = float(vel.w), vh = float(vel.h);
    struct Corner {
        float4 cur, prev;
    };
    std::vector<Corner> corners;
    for (size_t n = 0; n < plan.velocity_objects.size(); n++) {
        const VelocityObject& o = *plan.velocity_objects[n];
        const VelocityObjectPass& pass = plan.velocity_object_passes[n];
        const float near_plane = pass.depth_range.x;
        const Geometry& g = *o.geom;
        corners.resize(g.verts.size());
        for (size_t i = 0; i < g.verts.size(); i++)
            ObjectVertex(o, pass, g.verts[i], corners[i].cur, corners[i].prev);
        auto raster = [&](const Corner& a, const Corner& b, const Corner& c) {
            const Corner* v[3] = {&a, &b, &c};
            float sx[3], sy[3], iw[3];
            for (int i = 0; i < 3; i++) {
                iw[i] = 1.0f / v[i]->cur.w;
                sx[i] = (v[i]->cur.x * iw[i] * 0.5f + 0.5f) * vw;
                sy[i] = (0.5f - v[i]->cur.y * iw[i] * 0.5f) * vh;
            }
            const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
            if (!(std::fabs(area) > 1e-9f)) return;
            // y runs down the target, so a positive area goes clockwise
            if (o.cull && Culls(o.cull, area > 0)) return;
            const int x0 = std::max(0, int(std::floor(std::min({sx[0], sx[1], sx[2]}))));
            const int x1 = std::min(int(vel.w) - 1, int(std::ceil(std::max({sx[0], sx[1], sx[2]}))));
            const int y0 = std::max(0, int(std::floor(std::min({sy[0], sy[1], sy[2]}))));
            const int y1 = std::min(int(vel.h) - 1, int(std::ceil(std::max({sy[0], sy[1], sy[2]}))));
            if (x0 > x1 || y0 > y1) return;
            const float inv_area = 1.0f / area;
            auto owns = [&](int i, int j) {
                const float gx = -(sy[j] - sy[i]) * inv_area, gy = (sx[j] - sx[i]) * inv_area;
                return gx > 0 || (gx == 0 && gy > 0);
            };
            const bool own0 = owns(1, 2), own1 = owns(2, 0), own2 = owns(0, 1);
            for (int y = y0; y <= y1; y++) {
                for (int x = x0; x <= x1; x++) {
                    const float qx = float(x), qy = float(y);
                    float l[3];
                    l[0] = ((sx[2] - sx[1]) * (qy - sy[1]) - (sy[2] - sy[1]) * (qx - sx[1])) * inv_area;
                    l[1] = ((sx[0] - sx[2]) * (qy - sy[2]) - (sy[0] - sy[2]) * (qx - sx[2])) * inv_area;
                    l[2] = ((sx[1] - sx[0]) * (qy - sy[0]) - (sy[1] - sy[0]) * (qx - sx[0])) * inv_area;
                    if (l[0] < 0 || l[1] < 0 || l[2] < 0) continue;
                    if ((l[0] == 0 && !own0) || (l[1] == 0 && !own1) || (l[2] == 0 && !own2))
                        continue;
                    const float z = l[0] * iw[0] + l[1] * iw[1] + l[2] * iw[2];
                    const float d = VelocityObjectDepth(pass, 1.0f / z);
                    const size_t at = size_t(y) * vel.w + x;
                    if (!(d <= zbuf[at])) continue;
                    zbuf[at] = d;
                    const float q0 = l[0] * iw[0] / z, q1 = l[1] * iw[1] / z, q2 = l[2] * iw[2] / z;
                    const float4 cur = a.cur * q0 + b.cur * q1 + c.cur * q2;
                    const float4 prev = a.prev * q0 + b.prev * q1 + c.prev * q2;
                    const float2 uv = VelocityObjectUv(cur);
                    const int dx = int(std::clamp(std::floor(uv.x * float(width)), 0.0f,
                                                  float(width) - 1.0f));
                    const int dy = int(std::clamp(std::floor(uv.y * float(height)), 0.0f,
                                                  float(height) - 1.0f));
                    const float4 t =
                        VelocityObjectTexel(pass, cur, prev, depth[size_t(dy) * width + dx]);
                    if (t.w > 0.5f) vel.px[at] = Unorm8(float4{t.x, t.y, t.z, 1.0f});
                }
            }
        };
        // clipped at w = near: each triangle's corners in front of it, and
        // where its edges cross it
        for (size_t i = 0; i + 2 < g.indices.size(); i += 3) {
            const Corner* tri[3] = {&corners[g.indices[i]], &corners[g.indices[i + 1]],
                                    &corners[g.indices[i + 2]]};
            Corner poly[4];
            int count = 0;
            for (int k = 0; k < 3; k++) {
                const Corner& p = *tri[k];
                const Corner& q = *tri[(k + 1) % 3];
                const float dp = p.cur.w - near_plane, dq = q.cur.w - near_plane;
                if (dp >= 0) poly[count++] = p;
                if ((dp >= 0) != (dq >= 0)) {
                    const float t = dp / (dp - dq);
                    poly[count++] = {Lerp4(p.cur, q.cur, t), Lerp4(p.prev, q.prev, t)};
                }
            }
            for (int k = 1; k + 1 < count; k++) raster(poly[0], poly[k], poly[k + 1]);
        }
    }
}

// The velocity pass's numbers (PostPass::vel_*), from the velocity buffer as
// DoPostProcess found it (PostParams::vel_*), and the blur's step scale
// (c122.x, or the buffer's last)
void PlanVelocity(const PostParams& p, float scale, PostPass& pass) {
    // c134..c137 as Draw uploads the previous matrix: its columns (the
    // clip position's x is the first column's dot with the world position,
    // Milo's row vectors)
    for (int r = 0; r < 4; r++) {
        pass.vel_prev[r] = {p.vel_prev_view_proj[0][r], p.vel_prev_view_proj[1][r],
                            p.vel_prev_view_proj[2][r], p.vel_prev_view_proj[3][r]};
    }
    const float near_plane = p.vel_depth_range[0], far_plane = p.vel_depth_range[1];
    pass.vel_near = {p.vel_near[0], p.vel_near[1], p.vel_near[2], far_plane};
    // DrawRectDepth's four vertices, a strip over the target: (-1, 1),
    // (-1, -1), (1, 1), (1, -1) with uv (0, 0), (0, 1), (1, 0), (1, 1), the
    // corner rays in that order. The far plane's corners are a rectangle, so
    // the ray at uv is the top left's plus u across and v down
    const float* c = p.vel_corners[0];
    const float* down = p.vel_corners[1];
    const float* across = p.vel_corners[2];
    pass.vel_corner[0] = {c[0], c[1], c[2], 0};
    pass.vel_corner[1] = {across[0] - c[0], across[1] - c[1], across[2] - c[2], 0};
    pass.vel_corner[2] = {down[0] - c[0], down[1] - c[1], down[2] - c[2], 0};
    // where nothing drew, the depth texture's 0 (cleared, reverse Z) as
    // the shader takes it through c89: z = c89.z - c89.w, w = near far /
    // (far - z (far - near)); w / far
    const float z = p.vel_depth_range[2] - p.vel_depth_range[3];
    const float w = near_plane * far_plane / (far_plane - z * (far_plane - near_plane));
    pass.vel_depth = {w * (1.0f / far_plane), scale, 0, 0};
}

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

bool PlanPost(const FrameCapture& frame, uint32_t only, PostPlan& plan, bool noise,
              bool velocity) {
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
    // The soft particles' term reads the first surface of
    // RndSoftParticleBuffer, which its DoPost cleared and drew its particles
    // into after DoPostProcess started (and blurred into the second and
    // back). Without that pass the capture has no particles to draw (one
    // from before, or a frame whose queue held none it keeps).
    uint32_t soft = 0;
    if (consts && c.flags[kPostFlagSoft] && c.soft_surface[0]) {
        for (const Pass& s : frame.passes) {
            if (s.tex_obj == c.soft_surface[0] && (s.clear_flags & 0x0f) &&
                s.first_draw >= frame.post_boundary) {
                soft = s.tex_obj;
                flags |= kPostSoft;
                break;
            }
        }
    }
    // The noise reads the map the capture kept (none: no grain), by the
    // composite's seeds and scales, or on a world frame by the proc's
    const Texture* noise_map = frame.noise_map.get();
    const bool noise_kept = noise && noise_map && noise_map->width && noise_map->height &&
                            noise_map->rgba.size() == size_t(noise_map->width) * noise_map->height;
    if (noise_kept) {
        if (consts ? c.flags[kPostFlagNoise] != 0 : NoiseEnabled(p)) flags |= kPostNoise;
        if (consts ? c.flags[kPostFlagNoiseMidtone] != 0 : p.noise_midtone != 0)
            flags |= kPostNoiseMidtone;
    }
    if (consts ? c.flags[kPostFlagBlendPrevious] != 0 : BlendPrevious(p)) flags |= kPostTrails;
    // The camera motion blur, where the game's composite had it (TheShaderMgr
    // + 0x39), or on a world frame where DoVelocity would turn it on
    // (VelocityExpected). It needs the velocity buffer's cameras, which
    // captures from before don't have: none there. With `velocity` false
    // it's left off.
    const bool vel_known = p.vel_read && p.vel_depth_range[1] > 0;
    if (velocity && vel_known &&
        (consts ? c.flags[kPostFlagVelocity] != 0 : VelocityExpected(p)))
        flags |= kPostVelocity;
    if (only) flags &= only | ((only & kPostNoise) ? kPostNoiseMidtone : 0u);
    if (!(flags & kPostNoise)) flags &= ~kPostNoiseMidtone;
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
    if (flags & kPostSoft) plan.soft = soft;
    if (flags & kPostNoise) {
        float c113[4];
        if (consts) {
            pass.noise_seeds = Rgba(c.c112);
            std::copy(std::begin(c.c113), std::end(c.c113), c113);
        } else {
            pass.noise_seeds = p.noise_stationary ? float4{p.noise_seeds[0], p.noise_seeds[1],
                                                           p.noise_seeds[0], p.noise_seeds[1]}
                                                  : FrameSeeds(frame.game_frame);
            NoiseConstant(p, c113);
        }
        pass.noise = Rgba(c113);
        uint32_t s[4];
        PackSampler(frame.noise_sampler, 1 + uint32_t(noise_map->mips.size()), s);
        pass.noise_sampler = {s[0], s[1], s[2], s[3]};
        pass.noise_tex = {noise_map->width, noise_map->height, 0, 0};
        plan.noise = noise_map;
    }
    if (flags & kPostTrails) {
        if (consts) {
            pass.trails = Rgba(c.c125);
        } else {
            // UpdateBlendPrevious's, with a post frame's time at the rate
            // even/odd rendering runs post-processing at (every frame
            // without it)
            const float dt = 1.0f / (p.emulate_fps > 0 ? p.emulate_fps : 60.0f);
            pass.trails = {p.trail_threshold, dt / p.trail_duration, 1.0f / 3.0f, 0};
        }
    }
    if (flags & kPostVelocity) {
        PlanVelocity(p, consts ? c.c122[0] : p.vel_scale, pass);
        for (const VelocityObject& o : frame.velocity_objects) {
            if (!o.geom || o.geom->indices.empty() || !o.bones ||
                o.rows.size() != size_t(o.bones) * 3 * 2 * 4)
                continue;
            VelocityObjectPass v{};
            for (int r = 0; r < 8; r++) v.view_proj[r] = Rgba(o.view_proj[r]);
            v.depth_range = Rgba(o.depth_range);
            v.mesh = {o.skinned ? 1u : 0u, o.bones, 0, 0};
            plan.velocity_objects.push_back(&o);
            plan.velocity_object_passes.push_back(v);
        }
    }
    plan.trails_update = consts;

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
             const PostImage& volume, const PostImage& density, const PostImage& soft,
             std::vector<uint32_t>& out, std::vector<uint32_t>* bloom0,
             PostHistory* history, uint64_t game_frame) {
    const PostPass& pass = plan.composite;
    // the trails read the previous post frame, where there's one of this
    // size from an earlier frame
    const bool have_prev = history && history->w == width && history->h == height &&
                           history->game_frame && history->game_frame < game_frame &&
                           history->rgba.size() == size_t(width) * height;
    const uint32_t flags = have_prev ? pass.flags.x : pass.flags.x & ~kPostTrails;
    Level src;
    src.Resize(width, height);
    for (size_t i = 0; i < src.px.size(); i++) src.px[i] = Unpack(scene[i]);

    // the velocity pass, into a level half the picture's size, each texel
    // from the depth texel its uv lands in (the game's s9, point: depth's
    // width over the level's times x + .5, in integers, as the GPU's)
    Level vel;
    if (flags & kPostVelocity) {
        vel.Resize(std::max(width / 2, 1u), std::max(height / 2, 1u));
        for (uint32_t y = 0; y < vel.h; y++) {
            const uint32_t dy = std::min((2 * y + 1) * height / (2 * vel.h), height - 1);
            for (uint32_t x = 0; x < vel.w; x++) {
                const uint32_t dx = std::min((2 * x + 1) * width / (2 * vel.w), width - 1);
                vel.px[size_t(y) * vel.w + x] =
                    Unorm8(VelocityTexel(pass, Uv(x, y, vel), depth[size_t(dy) * width + dx]));
            }
        }
        // and the objects with their own motion over it
        ObjectPass(plan, vel, depth, width, height);
    }

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
        // with glare, its pass over level 0, which the composite reads
        if (flags & kPostGlare) {
            Glare(bloom[0], tmp);
            std::swap(bloom[0], tmp);
        }
        if (bloom0) {
            bloom0->resize(bloom[0].px.size());
            for (size_t i = 0; i < bloom0->size(); i++) {
                const float4 c = bloom[0].px[i];
                (*bloom0)[i] = uint32_t(c.x * 255.0f + 0.5f) | uint32_t(c.y * 255.0f + 0.5f) << 8 |
                               uint32_t(c.z * 255.0f + 0.5f) << 16 |
                               uint32_t(c.w * 255.0f + 0.5f) << 24;
            }
        }
    } else if (bloom0) {
        bloom0->clear();
    }

    // the spotlights' depth volume and density map, and the soft-particle
    // surface, as they were drawn
    auto load = [](const PostImage& image, Level& l) {
        if (!image.px || !image.w || !image.h) return;
        l.Resize(image.w, image.h);
        for (size_t i = 0; i < l.px.size(); i++) l.px[i] = Unpack(image.px[i]);
    };
    Level spot[2], soft_level;
    if (flags & kPostSpot) {
        load(volume, spot[0]);
        load(density, spot[1]);
    }
    if (flags & kPostSoft) load(soft, soft_level);

    // the noise map, read by its sampler at each tap (sample_model.h); the
    // composite's target size gives the taps' derivatives
    PostPass composite = pass;
    composite.flags.x = flags;
    composite.target = {float(width), float(height), 1.0f / float(width), 1.0f / float(height)};
    TexLevels noise_levels;
    if ((flags & kPostNoise) && plan.noise) {
        noise_levels.w = plan.noise->width;
        noise_levels.h = plan.noise->height;
        noise_levels.px = plan.noise->rgba.data();
        noise_levels.mips = &plan.noise->mips;
    }
    const uint32_t noise_sampler[4] = {pass.noise_sampler.x, pass.noise_sampler.y,
                                       pass.noise_sampler.z, pass.noise_sampler.w};
    auto noise_tap = [&](float2 uv, int tap) {
        const float2 at = NoiseUv(composite, uv, tap);
        const float2 dx = NoiseDx(composite, tap), dy = NoiseDy(composite, tap);
        const float a[2] = {at.x, at.y}, ddx[2] = {dx.x, dx.y}, ddy[2] = {dy.x, dy.y};
        float t[4];
        SampleTextureCpu(noise_levels, noise_sampler, a, ddx, ddy, t);
        return float3{t[0], t[1], t[2]};
    };

    out.resize(size_t(width) * height);
    const float4 none{0, 0, 0, 0};
    // a post frame's composite, which the next frame's trails read (once a
    // frame: drawn again, it's kept as it was)
    const bool keep = history && plan.trails_update && game_frame &&
                      !(history->game_frame == game_frame && history->w == width &&
                        history->h == height);
    std::vector<uint32_t> kept(keep ? out.size() : 0);
    auto pack = [](float4 c) {
        return uint32_t(c.x * 255.0f + 0.5f) | uint32_t(c.y * 255.0f + 0.5f) << 8 |
               uint32_t(c.z * 255.0f + 0.5f) << 16 | uint32_t(c.w * 255.0f + 0.5f) << 24;
    };
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
            float3 particles{0, 0, 0};
            if (!soft_level.px.empty()) {
                const float4 s = Sample(soft_level, uv);
                particles = {s.x, s.y, s.z};
            }
            float3 n0{0, 0, 0}, n1{0, 0, 0};
            if (noise_levels.px) {
                n0 = noise_tap(uv, 0);
                n1 = noise_tap(uv, 1);
            }
            // the scene, blurred along the motion where the velocity says
            float4 scene_px = src.px[i];
            if (!vel.px.empty()) {
                const float4 v = Sample(vel, uv);
                if (VelocityBlurs(composite, v)) {
                    const float2 step = VelocityStep(composite, v);
                    float4 acc = scene_px * kVelocityCentre;
                    for (int k = -5; k < 5; k++)
                        acc = acc + Sample(src, VelocityTap(uv, step, k)) * VelocityWeight(k);
                    scene_px = acc * kVelocityNorm;
                }
            }
            const float game_depth = GameDepth(pass, depth[i]);
            const float3 color = CompositeColor(composite, scene_px, d, game_depth, l[0], l[1],
                                                l[2], vol, dens, particles, n0, n1);
            float4 c;
            if (flags & kPostTrails) {
                c = Trails(composite, color, Unpack(history->rgba[i]));
            } else {
                const float3 rgb = saturate(color);
                c = {rgb.x, rgb.y, rgb.z, CompositeAlpha(composite, scene_px, d, game_depth)};
            }
            if (keep) kept[i] = pack(c);
            out[i] = pack(float4{c.x, c.y, c.z, 1.0f});
        }
    }
    if (keep) {
        history->rgba = std::move(kept);
        history->w = width;
        history->h = height;
        history->game_frame = game_frame;
    }
}

float GameDepthCpu(const PostPass& pass, float inv_w) { return GameDepth(pass, inv_w); }

float DofAmountCpu(const float c24[4], float depth) { return DofAmount(Rgba(c24), depth); }

void CompositeCpu(const PostPass& pass, const float scene[4], const float dof[4], float depth,
                  const float l0[3], const float l1[3], const float l2[3], const float volume[3],
                  float density, const float soft[3], const float noise0[3],
                  const float noise1[3], float out[3]) {
    const float3 r = Composite(pass, Rgba(scene), Rgba(dof), depth, Rgb(l0), Rgb(l1), Rgb(l2),
                               Rgb(volume), density, Rgb(soft), Rgb(noise0), Rgb(noise1));
    out[0] = r.x;
    out[1] = r.y;
    out[2] = r.z;
}

void VelocityTexelCpu(const PostPass& pass, const float uv[2], float inv_w, float out[4]) {
    const float4 v = VelocityTexel(pass, float2{uv[0], uv[1]}, inv_w);
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
    out[3] = v.w;
}

void VelocityObjectTexelCpu(const VelocityObjectPass& pass, const float cur[4],
                            const float prev[4], float inv_w, float out[4]) {
    const float4 t = VelocityObjectTexel(pass, Rgba(cur), Rgba(prev), inv_w);
    out[0] = t.x;
    out[1] = t.y;
    out[2] = t.z;
    out[3] = t.w;
}

void VelocityObjectVertexCpu(const VelocityObject& o, const VelocityObjectPass& pass,
                             const Vertex& v, float cur[4], float prev[4]) {
    float4 c, p;
    ObjectVertex(o, pass, v, c, p);
    cur[0] = c.x, cur[1] = c.y, cur[2] = c.z, cur[3] = c.w;
    prev[0] = p.x, prev[1] = p.y, prev[2] = p.z, prev[3] = p.w;
}

void VelocityBlurCpu(const PostPass& pass, const float uv[2], const float velocity[4],
                     const float centre[4], void (*tap)(const float at[2], float out[4]),
                     float out[4]) {
    float4 acc = Rgba(centre);
    const float4 v = Rgba(velocity);
    if (VelocityBlurs(pass, v)) {
        const float2 step = VelocityStep(pass, v);
        acc = acc * kVelocityCentre;
        for (int k = -5; k < 5; k++) {
            const float2 at = VelocityTap(float2{uv[0], uv[1]}, step, k);
            const float a[2] = {at.x, at.y};
            float t[4];
            tap(a, t);
            acc = acc + Rgba(t) * VelocityWeight(k);
        }
        acc = acc * kVelocityNorm;
    }
    out[0] = acc.x;
    out[1] = acc.y;
    out[2] = acc.z;
    out[3] = acc.w;
}

void TrailsCpu(const PostPass& pass, const float rgb[3], const float prev[4], float out[4]) {
    const float4 c = Trails(pass, Rgb(rgb), Rgba(prev));
    out[0] = c.x;
    out[1] = c.y;
    out[2] = c.z;
    out[3] = c.w;
}

void NoiseTapCpu(const PostPass& pass, const float uv[2], int tap, float at[2], float dx[2],
                 float dy[2]) {
    const float2 a = NoiseUv(pass, float2{uv[0], uv[1]}, tap);
    const float2 x = NoiseDx(pass, tap), y = NoiseDy(pass, tap);
    at[0] = a.x;
    at[1] = a.y;
    dx[0] = x.x;
    dx[1] = x.y;
    dy[0] = y.x;
    dy[1] = y.y;
}

}  // namespace band3::render::post
