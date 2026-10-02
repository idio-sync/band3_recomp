#include "src/Render/soft_raster.h"

#include "src/Render/post_model.h"
#include "src/Render/shade_model.h"
#include "src/Render/spot_model.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

// See soft_raster.h.

namespace band3::render {
namespace {

// the picture's clear; the scene target's is the same with alpha 0
constexpr uint32_t kClearColor = 0xff202020u;
// what a render target nothing has drawn samples as
constexpr uint32_t kTransparentBlack = 0;

// a vertex after the view-projection, with what the pixels interpolate
struct ClipVert {
    float p[4];  // clip x, y, z, w
    float uv[2];
    float n[3];
    float c[4];
    float wp[3];  // world position
    // a vertex-lit material's Lighting (shade.hlsli), diffuse and added
    float ld[3];
    float la[3];
    float ao[2];  // the point lights' AoShVertex
    // a normal-mapped draw's tangent and bitangent (shade.hlsli's
    // TextureFrame and Bitangent, in the world)
    float u[3];
    float b[3];
};

ClipVert Lerp(const ClipVert& a, const ClipVert& b, float t) {
    ClipVert r;
    for (int i = 0; i < 4; i++) r.p[i] = a.p[i] + (b.p[i] - a.p[i]) * t;
    for (int i = 0; i < 2; i++) r.uv[i] = a.uv[i] + (b.uv[i] - a.uv[i]) * t;
    for (int i = 0; i < 3; i++) r.n[i] = a.n[i] + (b.n[i] - a.n[i]) * t;
    for (int i = 0; i < 4; i++) r.c[i] = a.c[i] + (b.c[i] - a.c[i]) * t;
    for (int i = 0; i < 3; i++) r.wp[i] = a.wp[i] + (b.wp[i] - a.wp[i]) * t;
    for (int i = 0; i < 3; i++) r.ld[i] = a.ld[i] + (b.ld[i] - a.ld[i]) * t;
    for (int i = 0; i < 3; i++) r.la[i] = a.la[i] + (b.la[i] - a.la[i]) * t;
    for (int i = 0; i < 2; i++) r.ao[i] = a.ao[i] + (b.ao[i] - a.ao[i]) * t;
    for (int i = 0; i < 3; i++) r.u[i] = a.u[i] + (b.u[i] - a.u[i]) * t;
    for (int i = 0; i < 3; i++) r.b[i] = a.b[i] + (b.b[i] - a.b[i]) * t;
    return r;
}

void Point(const float p[3], const Mat4& m, float out[3]) {
    for (int c = 0; c < 3; c++)
        out[c] = p[0] * m.m[0][c] + p[1] * m.m[1][c] + p[2] * m.m[2][c] + m.m[3][c];
}

void Dir(const float d[3], const Mat4& m, float out[3]) {
    for (int c = 0; c < 3; c++) out[c] = d[0] * m.m[0][c] + d[1] * m.m[1][c] + d[2] * m.m[2][c];
}

// what a target does with alpha: the picture keeps it 1, a texture blends
// it by the colour's factors, the scene target as RB3's back buffer does
// (WritesSceneAlpha)
enum class TargetAlpha { kOpaque, kTexture, kScene };

// what a draw does with its target's alpha: Blend()
enum class AlphaRule { kOpaque, kColorFactors, kKeep, kMax };

struct Target {
    uint32_t w, h;
    std::vector<uint32_t>& color;
    std::vector<float>& depth;  // 1/w, larger is nearer, 0 is cleared
    std::vector<int32_t>* ids;  // the draw that last wrote each pixel, if wanted
    // a shadow map's pass: its depth, clip z/w (0 near, cleared to 1), which
    // its draws write where they're nearer, and nothing else
    float* zw = nullptr;
    TargetAlpha alpha = TargetAlpha::kOpaque;
    // a texture pass's: the texture it draws into, which its draws can't sample
    uint32_t tex_obj = 0;
    bool no_z = false;  // no depth buffer: nothing tests or writes depth
    // the picture's, once the scene is resolved into it: a copy of it as the
    // resolve left it, w x h, which REFRACT_WORLD draws read (RefractsWorld)
    const uint32_t* behind = nullptr;
    // a texture pass's: the scene's depth (1/w), scene_w x scene_h, which a
    // spotlight's cone reads where its pixel is on the screen (the game's s9,
    // the world's depth: the cones draw after it, before the overlay's)
    const float* scene_depth = nullptr;
    uint32_t scene_w = 0, scene_h = 0;
    // the viewport (clip space -1..1 maps to x..x+w, y..y+h), and the pixels
    // it covers, which is all a triangle can reach
    float vx = 0, vy = 0, vw = 0, vh = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    void SetViewport(float x, float y, float vw_, float vh_) {
        vx = x;
        vy = y;
        vw = vw_;
        vh = vh_;
        x0 = std::clamp(int(std::floor(x)), 0, int(w));
        y0 = std::clamp(int(std::floor(y)), 0, int(h));
        x1 = std::clamp(int(std::ceil(x + vw_)), x0, int(w));
        y1 = std::clamp(int(std::ceil(y + vh_)), y0, int(h));
    }
};

// a texture to sample: its size and pixels (RGBA8, R low), null for none
struct TexView {
    uint32_t w = 0, h = 0;
    const uint32_t* px = nullptr;
};

TexView View(const Texture* t) {
    if (!t || t->rgba.empty()) return {};
    return {t->width, t->height, t->rgba.data()};
}

struct DrawState {
    int32_t index;  // in the frame's draws
    // a spotlight's cone (IsSpotCone) shades with these instead of `shade`:
    // its tex is the cross-section texture, density the density map drawn
    // before it (none: 0)
    bool spot = false;
    spot::SpotParams spot_params;
    TexView density;
    // a soft particle (IsSoftParticle) shades as `shade` says, its alpha
    // faded by the scene's depth behind it (SoftPixelFade); soft_far is the
    // camera's far plane (PS c89.y), the depth where nothing drew
    bool soft = false;
    float soft_far = 0;
    TexView tex;
    TexView spec_map;  // none unless shade samples it
    TexView glow;
    TexView normal;  // kShadeNormalMap's s1, and kShadeDetailMap's s14
    TexView detail;
    TexView proj;  // the projected light's s5 and s10, likewise
    TexView gobo;
    // kShadeShadow: the shadow map's depth (clip z/w) as its pass left it
    const float* shadow = nullptr;
    uint32_t shadow_w = 0, shadow_h = 0;
    // into a shadow map (Target::zw): depth alone
    bool depth_only = false;
    TexView behind;  // the target's behind, for kShadeRefract
    shade::ShadeParams shade;
    bool per_vertex;  // kShadePerVertex: ClipVert's ld and la are set
    bool normal_map;  // kShadeNormalMap: ClipVert's u and b are set
    int blend;
    AlphaRule alpha;
    bool z_test;
    bool z_equal_passes;
    bool z_write;
    uint8_t cull;  // DrawItem::cull, 0 with RasterOptions::culling off
    // where in a pixel it samples (PixelCentre)
    float centre;
};

// Where a draw samples pixel x: at x + PixelCentre in the target's pixels
// (the viewport maps clip -1..1 to its edges). RB3 draws with D3D9's pixel
// centres, on the integers (the 360's HalfPixelOffset render state off,
// PA_SU_VTX_CNTL's pix_center kD3DZero), but its DrawRect quads with the
// state on, at .5 as D3D10's are (rb3-xenon rnddx9/Rnd.cpp's DrawRect): their
// rects are in pixels, edge to edge. gpu_view.cpp moves a mesh draw's clip
// position half a pixel right and down instead (mesh.hlsl's VSMain), which
// lands it on the same pixels.
float PixelCentre(const DrawItem& it) { return it.rect_shader >= 0 ? 0.5f : 0.0f; }

// nearest texel, wrapping; mesh.hlsl's Texel does the same arithmetic
void Texel(const TexView& t, const float uv[2], float out[4]) {
    const float u = uv[0] - std::floor(uv[0]), v = uv[1] - std::floor(uv[1]);
    const uint32_t x = std::min(uint32_t(u * float(t.w)), t.w - 1);
    const uint32_t y = std::min(uint32_t(v * float(t.h)), t.h - 1);
    const uint32_t c = t.px[size_t(y) * t.w + x];
    for (int i = 0; i < 4; i++) out[i] = float((c >> (8 * i)) & 0xff) / 255.0f;
}

// the texel at u, v (0..1), nearest, clamped to the edge
void TexelClamped(const TexView& t, float u, float v, float out[4]) {
    const uint32_t x = std::min(uint32_t(std::clamp(u, 0.0f, 1.0f) * float(t.w)), t.w - 1);
    const uint32_t y = std::min(uint32_t(std::clamp(v, 0.0f, 1.0f) * float(t.h)), t.h - 1);
    const uint32_t c = t.px[size_t(y) * t.w + x];
    for (int i = 0; i < 4; i++) out[i] = float((c >> (8 * i)) & 0xff) / 255.0f;
}

// bilinear at u, v (0..1), clamped to the edge: the linear clamp sampler the
// spotlight drawer sets for its density map and its blurs' taps
void SampleLinear(const TexView& t, float u, float v, float out[4]) {
    const float x = u * float(t.w) - 0.5f, y = v * float(t.h) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float tx = x - fx, ty = y - fy;
    auto at = [&](float px, float py, int c) {
        const int xi = std::clamp(int(px), 0, int(t.w) - 1);
        const int yi = std::clamp(int(py), 0, int(t.h) - 1);
        return float((t.px[size_t(yi) * t.w + xi] >> (8 * c)) & 0xff) / 255.0f;
    };
    for (int c = 0; c < 4; c++) {
        const float top = at(fx, fy, c) + (at(fx + 1, fy, c) - at(fx, fy, c)) * tx;
        const float bottom = at(fx, fy + 1, c) + (at(fx + 1, fy + 1, c) - at(fx, fy + 1, c)) * tx;
        out[c] = top + (bottom - top) * ty;
    }
}

// bilinear at u, v (0..1), outside the texture a transparent black border:
// the projected light's maps' sampler (their fetch constants clamp to a black
// border, filter linear); mesh.hlsl's ProjTexel does the same arithmetic
void SampleBorder(const TexView& t, float u, float v, float out[4]) {
    for (int c = 0; c < 4; c++) out[c] = 0;
    // beyond 2 every tap is the border; NaN (on the light's plane) is too
    if (!(std::fabs(u) < 2.0f && std::fabs(v) < 2.0f)) return;
    const float x = u * float(t.w) - 0.5f, y = v * float(t.h) - 0.5f;
    const float fx = std::floor(x), fy = std::floor(y);
    const float tx = x - fx, ty = y - fy;
    auto at = [&](int xi, int yi, int c) {
        if (xi < 0 || yi < 0 || xi >= int(t.w) || yi >= int(t.h)) return 0.0f;
        return float((t.px[size_t(yi) * t.w + xi] >> (8 * c)) & 0xff) / 255.0f;
    };
    const int x0 = int(fx), y0 = int(fy);
    for (int c = 0; c < 4; c++) {
        const float top = at(x0, y0, c) + (at(x0 + 1, y0, c) - at(x0, y0, c)) * tx;
        const float bottom = at(x0, y0 + 1, c) + (at(x0 + 1, y0 + 1, c) - at(x0, y0 + 1, c)) * tx;
        out[c] = top + (bottom - top) * ty;
    }
}

// The scene's depth (1/w, 0 where nothing drew) where a texture pass's pixel
// u, v (0..1 across its viewport) is on the screen, point-sampled: the game
// reads its depth texture there (s9) for the spotlights' cones and the soft
// particles, which draw after the world into targets of their own sizes
float SceneInvW(const Target& t, float u, float v) {
    if (!t.scene_depth) return 0;
    const uint32_t sx =
        std::min(uint32_t(std::clamp(u, 0.0f, 1.0f) * float(t.scene_w)), t.scene_w - 1);
    const uint32_t sy =
        std::min(uint32_t(std::clamp(v, 0.0f, 1.0f) * float(t.scene_h)), t.scene_h - 1);
    return t.scene_depth[size_t(sy) * t.scene_w + sx];
}

// A spotlight cone's colour at pixel x, y of the depth volume, wp the
// proxy's world position there and w its clip w (spot_model.hlsli's
// SpotCone). The shader takes where the pixel is on the screen from its clip
// position, where the pixel samples it (DrawState::centre), and reads the
// scene's depth there point-sampled, the density map bilinear.
void SpotPixel(const DrawState& ds, const Target& t, int x, int y, const float wp[3], float w,
               float out[4]) {
    const float u = (float(x) + ds.centre - t.vx) / t.vw;
    const float v = (float(y) + ds.centre - t.vy) / t.vh;
    const float inv_w = SceneInvW(t, u, v);
    const spot::SpotParams& sp = ds.spot_params;
    float texel[4] = {1, 1, 1, 1};
    if (ds.tex.px) TexelClamped(ds.tex, spot::SpotGoboCoordCpu(sp, wp), 0.0f, texel);
    float density[4] = {0, 0, 0, 0};
    if (ds.density.px) SampleLinear(ds.density, u, v, density);
    spot::SpotConeCpu(sp, wp, w, spot::SpotSceneDepthCpu(sp, inv_w), texel[0], density[1], out);
    out[3] = 0;
}

// A soft particle's alpha scale at pixel x, y of the soft-particle buffer, w
// its clip w there (shade.hlsli's SoftFade): the scene's depth read where the
// pixel is on the screen, as SpotPixel reads it. For the 320x180 buffer of a
// 1280x720 picture that's the depth at (4x, 4y), the texel the game's
// shader reads.
float SoftPixelFade(const DrawState& ds, const Target& t, int x, int y, float w) {
    const float u = (float(x) + ds.centre - t.vx) / t.vw;
    const float v = (float(y) + ds.centre - t.vy) / t.vh;
    return shade::SoftFadeCpu(ds.soft_far, SceneInvW(t, u, v), w);
}

// pixel x, y's colour; the picture behind it is the one at x, y, the
// target's size (mesh.hlsl reads it at SV_Position likewise); u and b the
// tangent and bitangent of a normal-mapped draw
void Shade(const DrawState& ds, int x, int y, const float uv[2], const float n[3],
           const float vc[4], const float wp[3], float depth, const float ao[2],
           const float ld[3], const float la[3], const float u[3], const float b[3],
           float out[4]) {
    float texel[4] = {1, 1, 1, 1}, spec_map[4] = {1, 1, 1, 1}, glow[4] = {0, 0, 0, 0};
    float behind[4] = {1, 1, 1, 1};
    if (ds.tex.px) Texel(ds.tex, uv, texel);
    if (ds.spec_map.px) Texel(ds.spec_map, uv, spec_map);
    if (ds.glow.px) Texel(ds.glow, uv, glow);
    shade::NormalMapInputs nm{};
    if (ds.normal_map) {
        for (int i = 0; i < 3; i++) nm.u[i] = u[i];
        for (int i = 0; i < 3; i++) nm.b[i] = b[i];
        Texel(ds.normal, uv, nm.map);
        if (ds.detail.px) {
            float duv[2];
            shade::DetailUvCpu(ds.shade, uv, duv);
            Texel(ds.detail, duv, nm.detail);
        }
    }
    if (ds.behind.px) {
        const uint32_t c = ds.behind.px[size_t(y) * ds.behind.w + x];
        for (int i = 0; i < 4; i++) behind[i] = float((c >> (8 * i)) & 0xff) / 255.0f;
    }
    float proj[4] = {0, 0, 0, 0}, gobo[4] = {0, 0, 0, 0};
    if (ds.proj.px) {
        float puv[2];
        shade::ProjUvCpu(ds.shade, wp, puv);
        SampleBorder(ds.proj, puv[0], puv[1], proj);
        if (ds.gobo.px) SampleBorder(ds.gobo, puv[0], puv[1], gobo);
    }
    const float lit =
        ds.shadow ? shade::ShadowLitCpu(ds.shade, wp, ds.shadow, ds.shadow_w, ds.shadow_h) : 1.0f;
    shade::ShadePixelCpu(ds.shade, wp, n, vc, texel, spec_map, glow, behind, depth, ao, ld, la,
                         out, proj, gobo, lit, ds.normal_map ? &nm : nullptr);
}

// The colour by the material's blend mode (Dest keeps it), and alpha by
// `alpha`: 1, blended by the colour's factors (a texture's, as gpu_view.cpp's
// pipelines do), kept, or RB3's back buffer's ONE ONE MAX, the larger of the
// two where the mode blends (any but Src, which Blend() draws other modes as).
// RndMat's Screen, Lighten and Darken (8..10) are drawn as Src too: NgMat's
// SetupShader sets no blend state for them (rb3-xenon Mat_NG.cpp's switch
// falls to default, and the second one asserts), so on the 360 they never
// appear.
uint32_t Blend(int mode, const float s[4], uint32_t dst, AlphaRule alpha) {
    float d[4];
    for (int i = 0; i < 4; i++) d[i] = float((dst >> (8 * i)) & 0xff) / 255.0f;
    float o[4];
    const float a = std::clamp(s[3], 0.0f, 1.0f);
    for (int i = 0; i < 3; i++) {
        switch (mode) {
            case 2: o[i] = d[i] + s[i]; break;                    // Add
            case 3: o[i] = s[i] * a + d[i] * (1.0f - a); break;  // SrcAlpha
            case 4: o[i] = d[i] + s[i] * a; break;               // SrcAlphaAdd
            case 5: o[i] = d[i] - s[i]; break;                   // Subtract
            case 6: o[i] = d[i] * s[i]; break;                   // Multiply
            // PreMultAlpha, ONE INVSRCALPHA: the colour comes scaled by alpha
            // (c0 by SetupShader's PreMultiplyAlpha, the texture as it is)
            case 7: o[i] = s[i] + d[i] * (1.0f - a); break;
            case 0: o[i] = d[i]; break;  // Dest
            default: o[i] = s[i]; break;  // Src
        }
    }
    switch (alpha) {
        case AlphaRule::kOpaque: o[3] = 1.0f; break;
        case AlphaRule::kKeep: o[3] = d[3]; break;
        case AlphaRule::kMax:
            o[3] = mode < 0 || mode == 1 || mode > 7 ? a : std::max(a, d[3]);
            break;
        case AlphaRule::kColorFactors:
            switch (mode) {
                case 2: o[3] = d[3] + a; break;
                case 3: o[3] = a * a + d[3] * (1.0f - a); break;
                case 4: o[3] = d[3] + a * a; break;
                case 5: o[3] = d[3] - a; break;
                case 6: o[3] = d[3] * a; break;
                case 7: o[3] = a + d[3] * (1.0f - a); break;
                default: o[3] = a; break;
            }
            break;
    }
    uint32_t r = 0;
    for (int i = 0; i < 4; i++)
        r |= uint32_t(std::clamp(o[i], 0.0f, 1.0f) * 255.0f + 0.5f) << (8 * i);
    return r;
}

void RasterTri(const ClipVert& a, const ClipVert& b, const ClipVert& c, const DrawState& ds,
               Target& t, RasterStats& st) {
    const ClipVert* v[3] = {&a, &b, &c};
    float sx[3], sy[3], iw[3];
    for (int i = 0; i < 3; i++) {
        iw[i] = 1.0f / v[i]->p[3];
        sx[i] = t.vx + (v[i]->p[0] * iw[i] * 0.5f + 0.5f) * t.vw;
        sy[i] = t.vy + (0.5f - v[i]->p[1] * iw[i] * 0.5f) * t.vh;
    }
    const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
    if (!(std::fabs(area) > 1e-9f)) return;
    // y runs down the screen, so a positive area goes clockwise
    if (ds.cull && Culls(ds.cull, area > 0)) return;
    const float min_x = std::max(float(t.x0), std::floor(std::min({sx[0], sx[1], sx[2]})));
    const float max_x = std::min(float(t.x1 - 1), std::ceil(std::max({sx[0], sx[1], sx[2]})));
    const float min_y = std::max(float(t.y0), std::floor(std::min({sy[0], sy[1], sy[2]})));
    const float max_y = std::min(float(t.y1 - 1), std::ceil(std::max({sy[0], sy[1], sy[2]})));
    if (min_x > max_x || min_y > max_y) return;
    st.triangles++;
    const float inv_area = 1.0f / area;
    // D3D's top-left rule, as the GPU fills: a pixel centre on an edge is
    // the triangle's only if that's a left edge (the inside to its right) or
    // a top one (level, the inside below), so of two triangles sharing an
    // edge (a quad's diagonal) only one draws it, and a blended quad blends
    // once. Edge k is opposite vertex k; (gx, gy) is where its l grows.
    auto owns = [&](int a, int b) {
        const float gx = -(sy[b] - sy[a]) * inv_area, gy = (sx[b] - sx[a]) * inv_area;
        return gx > 0 || (gx == 0 && gy > 0);
    };
    const bool own0 = owns(1, 2), own1 = owns(2, 0), own2 = owns(0, 1);

    for (int y = int(min_y); y <= int(max_y); y++) {
        const float py = float(y) + ds.centre;
        for (int x = int(min_x); x <= int(max_x); x++) {
            const float px = float(x) + ds.centre;
            const float l0 = ((sx[2] - sx[1]) * (py - sy[1]) - (sy[2] - sy[1]) * (px - sx[1])) * inv_area;
            const float l1 = ((sx[0] - sx[2]) * (py - sy[2]) - (sy[0] - sy[2]) * (px - sx[2])) * inv_area;
            const float l2 = ((sx[1] - sx[0]) * (py - sy[0]) - (sy[1] - sy[0]) * (px - sx[0])) * inv_area;
            if (l0 < 0 || l1 < 0 || l2 < 0) continue;
            if ((l0 == 0 && !own0) || (l1 == 0 && !own1) || (l2 == 0 && !own2)) continue;
            const size_t idx = size_t(y) * t.w + x;
            if (ds.depth_only) {
                // clip z/w, which runs straight across the screen; LESS
                const float zw = l0 * a.p[2] * iw[0] + l1 * b.p[2] * iw[1] + l2 * c.p[2] * iw[2];
                if (zw < t.zw[idx]) t.zw[idx] = zw;
                st.pixels++;
                continue;
            }
            const float z = l0 * iw[0] + l1 * iw[1] + l2 * iw[2];
            if (ds.z_test) {
                const float d = t.depth[idx];
                if (ds.z_equal_passes ? z < d * 0.9999f : z <= d) continue;
            }
            const float q0 = l0 * iw[0] / z, q1 = l1 * iw[1] / z, q2 = l2 * iw[2] / z;
            float uv[2], n[3], vc[4], wp[3];
            for (int i = 0; i < 2; i++) uv[i] = q0 * a.uv[i] + q1 * b.uv[i] + q2 * c.uv[i];
            for (int i = 0; i < 3; i++) n[i] = q0 * a.n[i] + q1 * b.n[i] + q2 * c.n[i];
            for (int i = 0; i < 4; i++) vc[i] = q0 * a.c[i] + q1 * b.c[i] + q2 * c.c[i];
            for (int i = 0; i < 3; i++) wp[i] = q0 * a.wp[i] + q1 * b.wp[i] + q2 * c.wp[i];
            float ao[2];
            for (int i = 0; i < 2; i++) ao[i] = q0 * a.ao[i] + q1 * b.ao[i] + q2 * c.ao[i];
            float ld[3] = {0, 0, 0}, la[3] = {0, 0, 0};
            if (ds.per_vertex) {
                for (int i = 0; i < 3; i++) ld[i] = q0 * a.ld[i] + q1 * b.ld[i] + q2 * c.ld[i];
                for (int i = 0; i < 3; i++) la[i] = q0 * a.la[i] + q1 * b.la[i] + q2 * c.la[i];
            }
            float tu[3] = {0, 0, 0}, tb[3] = {0, 0, 0};
            if (ds.normal_map) {
                for (int i = 0; i < 3; i++) tu[i] = q0 * a.u[i] + q1 * b.u[i] + q2 * c.u[i];
                for (int i = 0; i < 3; i++) tb[i] = q0 * a.b[i] + q1 * b.b[i] + q2 * c.b[i];
            }
            float col[4];
            if (ds.spot) {
                SpotPixel(ds, t, x, y, wp, 1.0f / z, col);
            } else {
                Shade(ds, x, y, uv, n, vc, wp, 1.0f / z, ao, ld, la, tu, tb, col);
                if (ds.soft) col[3] *= SoftPixelFade(ds, t, x, y, 1.0f / z);
            }
            if (shade::AlphaCutCpu(ds.shade, col[3])) continue;
            // Dest draws no colour, but may still write the scene's alpha
            if (ds.blend != 0 || ds.alpha == AlphaRule::kMax) {
                t.color[idx] = Blend(ds.blend, col, t.color[idx], ds.alpha);
                if (t.ids && ds.blend != 0) (*t.ids)[idx] = ds.index;
            }
            if (ds.z_write) t.depth[idx] = z;
            st.pixels++;
        }
    }
}

// The clip planes, as distances that are >= 0 inside: the near plane, then a
// guard band kGuard times the screen's half-size each side. A triangle that
// crosses the near plane projects up to ~1e9 pixels away, where RasterTri's
// float edge functions lose the pixel position altogether (stripes and blocks
// across the screen); clipped to the band, no corner is more than a few
// thousand pixels out. The band's planes pass through the eye, so clipping to
// them moves no pixel: they only cut away what's off screen anyway. A depth
// draw (a shadow map's) is clipped at z 0 too, the light camera's near plane,
// as the game's device clips it: its depth isn't 1/w but clip z/w, and what's
// in front of that plane would be stored nearer than anything.
constexpr float kGuard = 8.0f;
constexpr int kClipPlanes = 5;
constexpr int kDepthClipPlanes = kClipPlanes + 1;

float PlaneDist(const ClipVert& v, int plane) {
    switch (plane) {
        case 0: return v.p[3] - kNearW;
        case 1: return kGuard * v.p[3] - v.p[0];
        case 2: return kGuard * v.p[3] + v.p[0];
        case 3: return kGuard * v.p[3] - v.p[1];
        case 4: return kGuard * v.p[3] + v.p[1];
        default: return v.p[2];
    }
}

// clips against the near plane and the guard band (and z 0, a depth draw),
// then draws the fan
void ClipAndRaster(const ClipVert& a, const ClipVert& b, const ClipVert& c,
                   const DrawState& ds, Target& t, RasterStats& st) {
    const int planes = ds.depth_only ? kDepthClipPlanes : kClipPlanes;
    uint32_t outside = 0;
    for (int p = 0; p < planes; p++)
        if (PlaneDist(a, p) < 0 || PlaneDist(b, p) < 0 || PlaneDist(c, p) < 0) outside |= 1u << p;
    if (!outside) {
        RasterTri(a, b, c, ds, t, st);
        return;
    }
    // each plane adds at most one corner
    ClipVert buf[2][3 + kDepthClipPlanes];
    buf[0][0] = a;
    buf[0][1] = b;
    buf[0][2] = c;
    int n = 3, cur_buf = 0;
    for (int p = 0; p < planes && n >= 3; p++) {
        if (!(outside & (1u << p))) continue;
        const ClipVert* in = buf[cur_buf];
        ClipVert* out = buf[cur_buf ^ 1];
        int m = 0;
        for (int i = 0; i < n; i++) {
            const ClipVert& cur = in[i];
            const ClipVert& nxt = in[(i + 1) % n];
            const float dc = PlaneDist(cur, p), dn = PlaneDist(nxt, p);
            if (dc >= 0) out[m++] = cur;
            if ((dc >= 0) != (dn >= 0)) out[m++] = Lerp(cur, nxt, dc / (dc - dn));
        }
        n = m;
        cur_buf ^= 1;
    }
    const ClipVert* poly = buf[cur_buf];
    for (int i = 1; i + 1 < n; i++) RasterTri(poly[0], poly[i], poly[i + 1], ds, t, st);
}

// a texture pass's target, kept for the frame: what its passes have drawn,
// up to the one that made `version`
struct RtTarget {
    uint32_t w = 0, h = 0;
    std::vector<uint32_t> color;
    std::vector<float> depth;
    std::vector<float> zw;  // a shadow map's depth (Target::zw); empty for the rest
    uint32_t version = 0;
};
using RtTargets = std::unordered_map<uint32_t, RtTarget>;

// What a draw's diffuse texture samples. A render target is what its passes
// have drawn so far this frame, else guest memory's pixels if they're kept
// and wanted, else transparent black (counted); the target being drawn can't
// sample itself, so it's black too. Without texture passes, a render target
// is its guest pixels if wanted. A texture without pixels (a render target
// kept without them, or a format that isn't decoded) draws untextured, as
// does a draw whose shader doesn't sample it (SamplesDiffuse).
TexView Diffuse(const DrawItem& it, const RasterOptions& o, const RtTargets& rts,
                const Target& t, RasterStats& st) {
    static constexpr uint32_t kBlack = kTransparentBlack;
    if (!o.textures || !it.tex || !SamplesDiffuse(it)) return {};
    const Texture& tex = *it.tex;
    if (o.texture_passes && IsPassTarget(&tex)) {
        if (tex.tex_obj != t.tex_obj) {
            if (auto f = rts.find(tex.tex_obj); f != rts.end())
                return {f->second.w, f->second.h, f->second.color.data()};
            if (o.rt_guest_pixels && !tex.rgba.empty()) return View(&tex);
        }
        st.rt_missing++;
        return {1, 1, &kBlack};
    }
    if (tex.tex_obj && !o.rt_guest_pixels) return {};
    return View(&tex);
}

// The projected light's s5. A texture RB3 draws (ProjectedTargetOf: NgLight's
// shadow) is its target as its passes left it, where the last of them made
// the version the draw reads, else guest memory's pixels if they're kept and
// wanted, else none (counted); without texture passes, its guest pixels if
// wanted. Any other is the capture's decoded map. None leaves the projected
// light out, as gpu_view.cpp does.
TexView Projected(const ShadeState* state, const RasterOptions& o, const RtTargets& rts,
                  const Target& t, RasterStats& st) {
    const Texture* map = state->maps[kMapProjected].get();
    const Texture* rt = ProjectedTargetOf(state);
    if (!rt) return View(map);
    if (o.texture_passes) {
        if (auto f = rts.find(rt->tex_obj); f != rts.end() && rt->tex_obj != t.tex_obj &&
                                            f->second.version == rt->version)
            return {f->second.w, f->second.h, f->second.color.data()};
        if (o.rt_guest_pixels && !rt->rgba.empty()) return View(rt);
        st.rt_missing++;
        return {};
    }
    return o.rt_guest_pixels ? View(rt) : TexView{};
}

// A normal or detail map. One RB3 draws (MapTargetOf: a head's normal map)
// is its target as its passes have drawn it so far, as a diffuse render
// target is (Diffuse), else guest memory's pixels if they're kept and wanted,
// else none (counted), which leaves the map out, as gpu_view.cpp does. Any
// other is the capture's decoded map.
TexView NormalMap(const ShadeState* state, int map, const RasterOptions& o, const RtTargets& rts,
                  const Target& t, RasterStats& st) {
    const Texture* tex = state->maps[map].get();
    const Texture* rt = MapTargetOf(state, map);
    if (!rt) return View(tex);
    if (o.texture_passes && rt->tex_obj != t.tex_obj) {
        if (auto f = rts.find(rt->tex_obj); f != rts.end())
            return {f->second.w, f->second.h, f->second.color.data()};
    }
    if (o.rt_guest_pixels && !rt->rgba.empty()) return View(rt);
    if (o.texture_passes) st.rt_missing++;
    return {};
}

void DrawOne(const DrawItem& it, int32_t index, const ShadeState* state, const RasterOptions& o,
             const RtTargets& rts, Target& t, RasterStats& st, std::vector<ClipVert>& cv,
             const TexView& density = {}) {
    const Geometry& g = *it.geom;
    const bool skinned = o.skinning && !it.bones.empty();
    DrawState ds;
    ds.index = index;
    ds.cull = o.culling ? it.cull : 0;
    ds.centre = PixelCentre(it);
    ds.tex = Diffuse(it, o, rts, t, st);
    // a cone shades only into a texture: the depth volume
    ds.spot = t.tex_obj && IsSpotCone(it, state) && spot::PackSpot(*state, t.w, t.h, ds.spot_params);
    if (ds.spot) ds.density = density;
    // a soft particle fades only into a texture: the soft-particle buffer
    ds.soft = t.tex_obj && IsSoftParticle(it, state);
    if (ds.soft) ds.soft_far = state->Ps(89)[1];
    shade::PackShade(it, state, o, ds.tex.px != nullptr, ds.shade);
    if (ds.shade.flags.x & shade::kShadeSpecMap)
        ds.spec_map = View(state->maps[kMapSpecular].get());
    if (ds.shade.flags.x & shade::kShadeGlow) ds.glow = View(state->maps[kMapGlow].get());
    if (ds.shade.flags.x & shade::kShadeNormalMap) {
        ds.normal = NormalMap(state, kMapNormal, o, rts, t, st);
        if (ds.shade.flags.x & shade::kShadeDetailMap)
            ds.detail = NormalMap(state, kMapDetailNormal, o, rts, t, st);
        if (!ds.detail.px) ds.shade.flags.x &= ~shade::kShadeDetailMap;
        if (!ds.normal.px) ds.shade.flags.x &= ~(shade::kShadeNormalMap | shade::kShadeDetailMap);
    }
    if (ds.shade.flags.x & (shade::kShadeProjMultiply | shade::kShadeProjGobo)) {
        ds.proj = Projected(state, o, rts, t, st);
        if (ds.shade.flags.x & shade::kShadeProjGobo)
            ds.gobo = View(state->maps[kMapGobo].get());
        if (!ds.proj.px || ((ds.shade.flags.x & shade::kShadeProjGobo) && !ds.gobo.px)) {
            ds.shade.flags.x &= ~(shade::kShadeProjMultiply | shade::kShadeProjGobo);
            ds.proj = ds.gobo = {};
        }
    }
    // the shadow map, as the pass that made the version it reads left it; one
    // the frame drew no pass of, or another version of since, leaves it lit
    if (ds.shade.flags.x & shade::kShadeShadow) {
        const Texture* map = ShadowMapOf(state);
        const auto f = map ? rts.find(map->tex_obj) : rts.end();
        if (f != rts.end() && !f->second.zw.empty() && f->second.version == map->version &&
            map->tex_obj != t.tex_obj) {
            ds.shadow = f->second.zw.data();
            ds.shadow_w = f->second.w;
            ds.shadow_h = f->second.h;
        } else {
            ds.shade.flags.x &= ~shade::kShadeShadow;
        }
    }
    ds.depth_only = t.zw != nullptr;
    ds.per_vertex = (ds.shade.flags.x & shade::kShadePerVertex) != 0;
    ds.normal_map = (ds.shade.flags.x & shade::kShadeNormalMap) != 0;
    // REFRACT_WORLD reads the picture behind it: in the picture, once resolved
    if (ds.shade.flags.x & shade::kShadeRefract) {
        if (t.behind)
            ds.behind = {t.w, t.h, t.behind};
        else
            ds.shade.flags.x &= ~shade::kShadeRefract;
    }
    const bool ao_sh = (ds.shade.flags.x & shade::kShadeAoSh) != 0;
    cv.resize(g.verts.size());
    for (size_t i = 0; i < g.verts.size(); i++) {
        const Vertex& v = g.verts[i];
        ClipVert& c = cv[i];
        for (int k = 0; k < 4; k++) c.c[k] = float((v.color >> (8 * k)) & 0xff) / 255.0f;
        // the vertex colour's SH direction turns as the normal does
        float dir[3] = {0, 0, 0};
        if (ao_sh) shade::AoShDirectionCpu(c.c, dir);
        // a normal-mapped draw's normal is its frame's, which turns with
        // its tangent
        float nrm[3] = {v.nrm[0], v.nrm[1], v.nrm[2]}, tangent[3] = {0, 0, 0};
        if (ds.normal_map) shade::TextureFrameCpu(ds.shade, v.nrm, v.tan, nrm, tangent);
        float wp[3] = {0, 0, 0}, wn[3] = {0, 0, 0}, wd[3] = {0, 0, 0}, wu[3] = {0, 0, 0};
        if (skinned) {
            float total = 0;
            for (int k = 0; k < 4; k++) {
                const float w = v.weight[k];
                if (w <= 0) continue;
                const Mat4& b = it.bones[v.bone[k] < it.bones.size() ? v.bone[k] : 0];
                float p[3], n[3], d[3] = {0, 0, 0}, u[3] = {0, 0, 0};
                Point(v.pos, b, p);
                Dir(nrm, b, n);
                if (ao_sh) Dir(dir, b, d);
                if (ds.normal_map) Dir(tangent, b, u);
                for (int j = 0; j < 3; j++) {
                    wp[j] += p[j] * w;
                    wn[j] += n[j] * w;
                    wd[j] += d[j] * w;
                    wu[j] += u[j] * w;
                }
                total += w;
            }
            if (total <= 0) {
                Point(v.pos, it.bones[0], wp);
                Dir(nrm, it.bones[0], wn);
                if (ao_sh) Dir(dir, it.bones[0], wd);
                if (ds.normal_map) Dir(tangent, it.bones[0], wu);
            }
        } else {
            Point(v.pos, it.world, wp);
            Dir(nrm, it.world, wn);
            if (ao_sh) Dir(dir, it.world, wd);
            if (ds.normal_map) Dir(tangent, it.world, wu);
        }
        for (int k = 0; k < 3; k++) c.u[k] = wu[k];
        c.b[0] = c.b[1] = c.b[2] = 0;
        if (ds.normal_map) shade::BitangentCpu(wn, wu, v.tan[3], c.b);
        for (int col = 0; col < 4; col++)
            c.p[col] = wp[0] * it.view_proj.m[0][col] + wp[1] * it.view_proj.m[1][col] +
                       wp[2] * it.view_proj.m[2][col] + it.view_proj.m[3][col];
        shade::TexGenUv(ds.shade, v.uv, c.uv);
        for (int k = 0; k < 3; k++) c.n[k] = wn[k];
        for (int k = 0; k < 3; k++) c.wp[k] = wp[k];
        c.ao[0] = c.ao[1] = 1.0f;
        if (ao_sh) shade::AoShVertexCpu(ds.shade, wp, wn, wd, c.c, c.ao);
        if (ds.per_vertex) shade::LightVertexCpu(ds.shade, wp, wn, c.c, c.ao, c.ld, c.la);
    }

    ds.blend = o.blending ? it.blend : 1;
    switch (t.alpha) {
        case TargetAlpha::kOpaque: ds.alpha = AlphaRule::kOpaque; break;
        case TargetAlpha::kTexture: ds.alpha = AlphaRule::kColorFactors; break;
        case TargetAlpha::kScene:
            ds.alpha = WritesSceneAlpha(state) ? AlphaRule::kMax : AlphaRule::kKeep;
            break;
    }
    switch (it.z_mode) {
        case 0: ds.z_test = false; ds.z_equal_passes = false; ds.z_write = false; break;
        case 2: ds.z_test = true; ds.z_equal_passes = true; ds.z_write = false; break;
        case 3: ds.z_test = false; ds.z_equal_passes = false; ds.z_write = true; break;
        case 4: ds.z_test = true; ds.z_equal_passes = true; ds.z_write = true; break;
        default: ds.z_test = true; ds.z_equal_passes = false; ds.z_write = true; break;
    }
    if (!o.blending) {
        ds.z_test = true;
        ds.z_write = true;
    }
    if (t.no_z) ds.z_test = ds.z_write = false;
    for (size_t i = 0; i + 2 < g.indices.size(); i += 3)
        ClipAndRaster(cv[g.indices[i]], cv[g.indices[i + 1]], cv[g.indices[i + 2]], ds, t, st);
    st.draws++;
}

bool Drawable(const DrawItem& it) {
    return it.geom && !it.geom->verts.empty() && it.geom->indices.size() >= 3;
}

// A DrawRect blur's taps (shader 1: c31.. their uv offsets, c47.. their
// weights per channel) from `src` into its rect of `t`: each pixel the taps'
// weighted sum, bilinear and clamped, at the quad's uv plus each tap's
// offset, blended by its material. The spotlights' blur and NgLight's read
// their own target as it was (spot::SpotBlur: whole texels apart, so point), the soft
// particles' the other surface (SoftBlur: half-texel taps, so bilinear).
void TapBlurDraw(const DrawItem& it, const ShadeState& s, const RasterOptions& o,
                 const TexView& src, Target& t, RasterStats& st) {
    const int x0 = std::clamp(int(std::floor(it.rect[0])), 0, int(t.w));
    const int y0 = std::clamp(int(std::floor(it.rect[1])), 0, int(t.h));
    const int x1 = std::clamp(int(std::ceil(it.rect[0] + it.rect[2])), x0, int(t.w));
    const int y1 = std::clamp(int(std::ceil(it.rect[1] + it.rect[3])), y0, int(t.h));
    if (it.rect[2] <= 0 || it.rect[3] <= 0) return;
    const int blend = o.blending ? it.blend : 1;
    for (int y = y0; y < y1; y++) {
        const float v = (float(y) + 0.5f - it.rect[1]) / it.rect[3];
        for (int x = x0; x < x1; x++) {
            const float u = (float(x) + 0.5f - it.rect[0]) / it.rect[2];
            float sum[4] = {0, 0, 0, 0};
            for (int i = 0; i < spot::kSpotBlurTaps; i++) {
                const float* off = s.Ps(31 + i);
                const float* weight = s.Ps(47 + i);
                float tap[4];
                SampleLinear(src, u + off[0], v + off[1], tap);
                for (int c = 0; c < 4; c++) sum[c] += tap[c] * weight[c];
            }
            const size_t idx = size_t(y) * t.w + x;
            t.color[idx] = Blend(blend, sum, t.color[idx], AlphaRule::kColorFactors);
            st.pixels++;
        }
    }
    st.draws++;
}

// The blur into the target it samples (spot::SpotBlur), as the game does it
// in place by a resolve: from a copy of the target as it was before it
void SpotBlurDraw(const DrawItem& it, const ShadeState& s, const RasterOptions& o, Target& t,
                  RasterStats& st) {
    const std::vector<uint32_t> before = t.color;
    TapBlurDraw(it, s, o, {t.w, t.h, before.data()}, t, st);
}


// NgSpotlightDrawer's targets and RndSoftParticleBuffer's surfaces: drawn
// after post-processing starts, for the composite, so kept when something
// wants them though the rest of post-processing's passes aren't
bool SpotTarget(const Pass& p) {
    return p.tex_type == kTexTypeDepthVolume || p.tex_type == kTexTypeDensityMap;
}
bool SoftTarget(const FrameCapture& f, const Pass& p) {
    return p.tex_obj && (p.tex_obj == f.post_consts.soft_surface[0] ||
                         p.tex_obj == f.post_consts.soft_surface[1]);
}

// PlanPasses, with `also` (a DxTex, 0 none) wanted whatever samples it,
// even after post-processing starts: at the frame's end, and its pass of
// `also_version` (0 none) too
std::vector<PassRun> Plan(const FrameCapture& f, const RasterOptions& o, uint32_t also,
                          uint32_t also_version) {
    std::vector<PassRun> runs;
    const uint32_t n = uint32_t(f.draws.size());
    if (f.passes.empty()) {
        runs.push_back({nullptr, 0, n});
        return runs;
    }
    // backwards, so a pass is drawn only if something after it samples its
    // texture
    std::unordered_set<uint32_t> needed;
    if (also) needed.insert(also);
    // the composite's spotlight term reads the depth volume (and through its
    // cones the density map) at the frame's end, its soft particles' term
    // the soft-particle surface
    post::PostPlan post_plan;
    if (o.post && o.view == RasterView::kFinal && post::PlanPost(f, o.post_only, post_plan)) {
        if (post_plan.composite.flags.x & post::kPostSpot) needed.insert(post_plan.spot_volume);
        if (post_plan.composite.flags.x & post::kPostSoft) needed.insert(post_plan.soft);
    }
    auto samples = [&](uint32_t first, uint32_t end, bool texture) {
        for (uint32_t d = first; d < end; d++) {
            const DrawItem& it = f.draws[d];
            if (texture ? !DrawnInTexturePass(it) : !DrawnToBackBuffer(it)) continue;
            if (o.textures && IsPassTarget(it.tex.get()) && SamplesDiffuse(it))
                needed.insert(it.tex->tex_obj);
            const ShadeState* state = shade::ShadeOf(f, it);
            // the projected light's s5, NgLight's shadow
            if (o.textures)
                if (const Texture* map = ProjectedTargetOf(state)) needed.insert(map->tex_obj);
            // a SHADOW_BUFFER draw's shadow map (s5)
            if (o.self_shadow)
                if (const Texture* map = ShadowMapOf(state)) needed.insert(map->tex_obj);
            // a head's normal map (s1), or a detail map RB3 draws (s14)
            if (o.textures && o.normal_maps)
                for (int m : {kMapNormal, kMapDetailNormal})
                    if (const Texture* map = MapTargetOf(state, m)) needed.insert(map->tex_obj);
        }
    };
    for (size_t i = f.passes.size(); i-- > 0;) {
        const Pass& p = f.passes[i];
        const uint32_t first = std::min(p.first_draw, n);
        const uint32_t end = std::min(first + p.draw_count, n);
        if (!p.tex_obj) {
            runs.push_back({nullptr, first, end});
            samples(first, end, false);
            continue;
        }
        const bool wanted = also && p.tex_obj == also;
        const bool shadow_map = p.tex_type == kTexTypeShadowMap;
        if (!o.texture_passes || !p.width || !p.height) continue;
        if (shadow_map && !o.self_shadow && !wanted) continue;
        if (!needed.count(p.tex_obj) && !(wanted && p.version == also_version)) continue;
        // (a shadow map is drawn for the character after it, wherever that is)
        if (first >= f.post_boundary && !wanted && !SpotTarget(p) && !SoftTarget(f, p) &&
            !shadow_map)
            continue;
        // what it drew over isn't seen: a shadow map's clear is its depth's
        if ((PassClearFlags(f, p) & 0x0f) || (shadow_map && (p.clear_flags & 0x30)))
            needed.erase(p.tex_obj);
        runs.push_back({&p, first, end});
        samples(first, end, true);
        // the cones read the density map drawn before them
        if (p.tex_type == kTexTypeDepthVolume) {
            for (size_t j = i; j-- > 0;) {
                if (f.passes[j].tex_type != kTexTypeDensityMap) continue;
                needed.insert(f.passes[j].tex_obj);
                break;
            }
        }
    }
    std::reverse(runs.begin(), runs.end());
    return runs;
}

// Rasterize(), and the texture targets it drew: `stop` (a DxTex, 0 none)
// ends the frame after `stop_version` of it (0 its last) is drawn
RasterStats Run(const FrameCapture& frame, const RasterOptions& o, std::vector<uint32_t>& rgba,
                std::vector<int32_t>* ids, RtTargets& rts, uint32_t stop,
                uint32_t stop_version) {
    const auto start = std::chrono::steady_clock::now();
    RasterStats st;
    const size_t pixels = size_t(o.width) * o.height;
    rgba.assign(pixels, kClearColor);
    if (ids) ids->assign(pixels, -1);
    std::vector<float> depth(pixels, 0.0f);
    // the world's draws into the scene target, cleared with alpha 0; the
    // overlay's into the picture, over the depth the world left
    std::vector<uint32_t> scene(pixels, kClearColor & 0x00ffffffu);
    Target world{o.width, o.height, scene, depth, ids};
    world.alpha = TargetAlpha::kScene;
    world.SetViewport(0, 0, float(o.width), float(o.height));
    Target overlay{o.width, o.height, rgba, depth, ids};
    overlay.SetViewport(0, 0, float(o.width), float(o.height));
    Target* back = &world;
    // the picture as the resolve leaves it, kept if an overlay draw reads it
    std::vector<uint32_t> behind;
    bool refracts = false;
    for (size_t i = frame.post_boundary; i < frame.draws.size() && !refracts; i++) {
        const DrawItem& it = frame.draws[i];
        refracts = DrawnToBackBuffer(it) && RefractsWorld(shade::ShadeOf(frame, it));
    }
    post::PostPlan post_plan;
    const bool post_on =
        o.post && o.view == RasterView::kFinal && post::PlanPost(frame, o.post_only, post_plan);
    // the scene into the picture, at post_boundary (or the frame's end):
    // post-processed, or as it is. A view of the scene target ends the frame
    // there.
    auto resolve = [&] {
        back = &overlay;
        if (post_on) {
            // the spotlights' passes, drawn before it (Plan keeps them)
            auto image = [&](uint32_t tex_obj) {
                const auto f = rts.find(tex_obj);
                if (!tex_obj || f == rts.end()) return post::PostImage{};
                return post::PostImage{f->second.color.data(), f->second.w, f->second.h};
            };
            // the depth buffer has 1/w, as RunPost wants it
            post::RunPost(post_plan, scene, depth, o.width, o.height,
                          image(post_plan.spot_volume), image(post_plan.spot_density),
                          image(post_plan.soft), rgba, o.post_bloom0);
        } else {
            for (size_t i = 0; i < pixels; i++) {
                const uint32_t c = scene[i];
                uint32_t g;
                switch (o.view) {
                    case RasterView::kSceneAlpha: g = c >> 24; break;
                    case RasterView::kSceneDepth:
                        // the depth buffer has 1/w
                        g = uint32_t(DepthViewGrey(depth[i] * kNearW) * 255.0f + 0.5f);
                        break;
                    default: rgba[i] = c | 0xff000000u; continue;
                }
                rgba[i] = g | g << 8 | g << 16 | 0xff000000u;
            }
        }
        if (refracts) {
            behind = rgba;
            overlay.behind = behind.data();
        }
    };
    std::vector<ClipVert> cv;
    std::unordered_set<uint32_t> cams_seen;
    uint32_t last_cam = 0;
    // the density map the spotlights' cones read: the last drawn
    uint32_t density = 0;
    for (const PassRun& run : Plan(frame, o, stop, stop_version)) {
        if (!run.pass) {
            for (uint32_t i = run.first; i < run.end; i++) {
                const DrawItem& it = frame.draws[i];
                if (!DrawnToBackBuffer(it)) continue;
                if (back == &world && i >= frame.post_boundary) resolve();
                if (back == &overlay && o.view != RasterView::kFinal) break;
                if (o.clear_depth_per_camera && it.cam != last_cam &&
                    cams_seen.insert(it.cam).second)
                    std::fill(depth.begin(), depth.end(), 0.0f);
                last_cam = it.cam;
                if (!Drawable(it)) continue;
                DrawOne(it, int32_t(i), shade::ShadeOf(frame, it), o, rts, *back, st, cv);
            }
            continue;
        }
        // into the texture's own target, made at its size (again if that
        // changed) and cleared as DxCam::Select cleared it; one no camera
        // cleared (or NgLight cleared: PassClearFlags) starts transparent black
        const Pass& p = *run.pass;
        RtTarget& rt = rts[p.tex_obj];
        const bool shadow_map = p.tex_type == kTexTypeShadowMap;
        if (rt.w != p.width || rt.h != p.height) {
            rt.w = p.width;
            rt.h = p.height;
            rt.color.assign(size_t(rt.w) * rt.h, kTransparentBlack);
            rt.depth.assign(size_t(rt.w) * rt.h, 0.0f);
            rt.zw.clear();
        }
        // a shadow map's depth: nothing drawn is as far as it goes
        if (shadow_map && rt.zw.empty()) rt.zw.assign(size_t(rt.w) * rt.h, 1.0f);
        if (PassClearFlags(frame, p) & 0x0f)
            std::fill(rt.color.begin(), rt.color.end(), ArgbToRgba(p.clear_color));
        if (p.clear_flags & 0x30) {
            std::fill(rt.depth.begin(), rt.depth.end(), 0.0f);
            std::fill(rt.zw.begin(), rt.zw.end(), p.clear_z);
        }
        rt.version = p.version;
        Target rtt{rt.w, rt.h, rt.color, rt.depth, nullptr};
        if (shadow_map) rtt.zw = rt.zw.data();
        rtt.alpha = TargetAlpha::kTexture;
        rtt.tex_obj = p.tex_obj;
        rtt.no_z = (p.tex_type & kTexTypeNoZ) != 0;
        rtt.scene_depth = depth.data();
        rtt.scene_w = o.width;
        rtt.scene_h = o.height;
        if (p.tex_type == kTexTypeDensityMap) density = p.tex_obj;
        TexView density_view;
        if (auto d = rts.find(density); density && d != rts.end() && p.tex_obj != density)
            density_view = {d->second.w, d->second.h, d->second.color.data()};
        for (uint32_t i = run.first; i < run.end; i++) {
            const DrawItem& it = frame.draws[i];
            if (!DrawnInTexturePass(it) || !Drawable(it)) continue;
            const ShadeState* state = shade::ShadeOf(frame, it);
            if (spot::SpotBlur(it, state, p)) {
                SpotBlurDraw(it, *state, o, rtt, st);
                continue;
            }
            if (SoftBlur(frame, it, state, p)) {
                // the other surface as its pass left it (transparent black,
                // counted, if none did)
                static constexpr uint32_t kBlack = kTransparentBlack;
                TexView src{1, 1, &kBlack};
                if (auto s = rts.find(it.tex->tex_obj); s != rts.end())
                    src = {s->second.w, s->second.h, s->second.color.data()};
                else
                    st.rt_missing++;
                TapBlurDraw(it, *state, o, src, rtt, st);
                continue;
            }
            // the camera's viewport; DrawRect's quads are in the target's
            // pixels, over all of it
            if (it.rect_shader < 0 && p.viewport[2] > 0 && p.viewport[3] > 0)
                rtt.SetViewport(p.viewport[0], p.viewport[1], p.viewport[2], p.viewport[3]);
            else
                rtt.SetViewport(0, 0, float(rt.w), float(rt.h));
            DrawOne(it, int32_t(i), state, o, rts, rtt, st, cv, density_view);
        }
        st.passes++;
        if (stop && p.tex_obj == stop && p.version == stop_version) break;
    }
    if (back == &world) resolve();
    st.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
    return st;
}

}  // namespace

bool SoftBlur(const FrameCapture& f, const DrawItem& d, const ShadeInputs* s, const Pass& p) {
    const uint32_t* surface = f.post_consts.soft_surface;
    if (d.rect_shader != 1 || !s || !p.tex_obj || !d.tex || d.tex->tex_obj == p.tex_obj ||
        (p.tex_obj != surface[0] && p.tex_obj != surface[1]))
        return false;
    float weights = 0;
    for (int i = 0; i < kSoftBlurTaps; i++) weights += s->Ps(47 + i)[0];
    return weights > 0;
}

bool ShadowCasterPass(const FrameCapture& f, const Pass& p) {
    if (!p.tex_obj) return false;
    const size_t end = std::min<size_t>(size_t(p.first_draw) + p.draw_count, f.draws.size());
    for (size_t d = p.first_draw; d < end; d++)
        if (f.draws[d].draw_mode == kDrawModeShadowCasters) return true;
    return false;
}

std::vector<PassRun> PlanPasses(const FrameCapture& frame, const RasterOptions& options) {
    return Plan(frame, options, 0, 0);
}

RasterStats Rasterize(const FrameCapture& frame, const RasterOptions& o,
                      std::vector<uint32_t>& rgba, std::vector<int32_t>* ids) {
    RtTargets rts;
    RasterStats st = Run(frame, o, rgba, ids, rts, 0, 0);
    // the presenter's last step, after the overlay
    if (o.gamma && o.view == RasterView::kFinal) {
        const auto start = std::chrono::steady_clock::now();
        ApplyGamma(frame.gamma, rgba);
        st.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                           start).count();
    }
    return st;
}

bool RasterizeTarget(const FrameCapture& frame, const RasterOptions& options, uint32_t tex_obj,
                     uint32_t version, std::vector<uint32_t>& rgba, uint32_t& width,
                     uint32_t& height, RasterStats* stats) {
    RtTargets rts;
    std::vector<uint32_t> screen;
    const RasterStats st = Run(frame, options, screen, nullptr, rts, tex_obj, version);
    if (stats) *stats = st;
    auto it = rts.find(tex_obj);
    if (it == rts.end()) return false;
    rgba = it->second.color;
    width = it->second.w;
    height = it->second.h;
    // a shadow map's depth, near white to far (or nothing) black, opaque
    if (!it->second.zw.empty()) {
        for (size_t i = 0; i < rgba.size(); i++) {
            const float g = 1.0f - std::clamp(it->second.zw[i], 0.0f, 1.0f);
            const uint32_t v = uint32_t(g * 255.0f + 0.5f);
            rgba[i] = v | v << 8 | v << 16 | 0xff000000u;
        }
    }
    return true;
}

}  // namespace band3::render
