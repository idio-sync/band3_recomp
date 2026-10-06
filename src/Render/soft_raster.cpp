#include "src/Render/soft_raster.h"

#include "src/Render/frame_compose.h"
#include "src/Render/post_model.h"
#include "src/Render/sample_model.h"
#include "src/Render/shade_model.h"
#include "src/Render/spot_model.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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
    // the overlay's, multisampled (OverlaySamples): `samples` colours and
    // depths per pixel, a pixel's one after another, which its draws write
    // instead of color and depth, and the frame's end averages into color
    uint32_t samples = 1;
    uint32_t* ms_color = nullptr;
    float* ms_depth = nullptr;
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

// a texture to sample: its size, pixels (RGBA8, R low; null for none) and
// mips, if it has any
using TexView = TexLevels;

using MipChain = std::vector<std::vector<uint32_t>>;
const MipChain* MipsOf(const MipChain& m) { return m.empty() ? nullptr : &m; }

TexView View(const Texture* t) {
    if (!t || t->rgba.empty()) return {};
    return {t->width, t->height, t->rgba.data(), MipsOf(t->mips)};
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
    TexView normal;  // kShadeNormalMap's s1 (kShadeRefractMap's), and kShadeDetailMap's s14
    TexView detail;
    TexView proj;  // the projected light's s5 and s10, likewise
    TexView gobo;
    // kShadeShadow: the shadow map's depth (clip z/w) as its pass left it
    const float* shadow = nullptr;
    uint32_t shadow_w = 0, shadow_h = 0;
    // into a shadow map (Target::zw): depth alone
    bool depth_only = false;
    // the target's behind, for kShadeRefract, and its viewport (x, y, w, h),
    // from which a pixel's clip position is worked out (RefractUv)
    TexView behind;
    float view[4] = {};
    // the samplers tex, spec_map, glow, normal and detail are read with
    // (sample_model.h's PackSampler), and whether any of them is the game's,
    // which reads the uv's derivatives
    uint32_t samp_tex[4], samp_spec[4], samp_glow[4], samp_normal[4], samp_detail[4];
    bool lod = false;
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
    // the depth it tests and writes, if not 1/w (a back-buffer draw's
    // camera's: LayoutBackBuffer)
    bool depth_mapped = false;
    DepthMap depth_map;
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

// native_fill_window: mesh.hlsl's StretchEdges. A clip position past the
// game's 16:9 (edge, RasterOptions::overlay_edge) moved out to the picture's
// edge; null for a draw it doesn't apply to
void StretchEdges(float p[4], const float* edge) {
    if (!edge || p[3] <= 0) return;
    for (int a = 0; a < 2; a++) {
        if (edge[a] < 1 && std::abs(p[a]) > edge[a] * p[3]) p[a] = p[a] < 0 ? -p[3] : p[3];
    }
}

// Where a multisampled pixel's samples are, from where it samples
// (PixelCentre), in pixels, y down: D3D's standard 2x and 4x patterns
// (D3D11_STANDARD_MULTISAMPLE_PATTERN, in 16ths of a pixel: 2x (4,4)
// (-4,-4); 4x (-2,-6) (6,-2) (-6,2) (2,6)), which the GPU's multisampled
// targets have (Direct3D 12 and Vulkan both) and the emulated GPU's 2x
// (diagonal, top left and bottom right) is too. gpu_view.cpp's half-pixel
// move of a mesh draw moves its samples with it, so they're these about
// the game's pixel centre either way.
constexpr float kSamples2[2][2] = {{0.25f, 0.25f}, {-0.25f, -0.25f}};
constexpr float kSamples4[4][2] = {
    {-0.125f, -0.375f}, {0.375f, -0.125f}, {-0.375f, 0.125f}, {0.125f, 0.375f}};
const float (*SamplePositions(uint32_t samples))[2] {
    return samples == 4 ? kSamples4 : kSamples2;
}

// A multisampled pixel's samples averaged, as the GPU's resolve does
// (SDL_GPU_STOREOP_RESOLVE, Direct3D 12's ResolveSubresource: the mean of
// the samples' UNORM values, back to 8 bits rounding to nearest, half up),
// and RB3's EndTiling (the mean of its two)
uint32_t ResolvePixel(const uint32_t* s, uint32_t samples) {
    uint32_t r = 0;
    for (int c = 0; c < 4; c++) {
        uint32_t sum = 0;
        for (uint32_t k = 0; k < samples; k++) sum += (s[k] >> (8 * c)) & 0xff;
        r |= ((sum + samples / 2) / samples) << (8 * c);
    }
    return r;
}

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

// A texel of t at uv: by sampler s, the game's (sample_model.h), with the
// uv's derivatives d (across, then down), or else nearest at level 0
void Read(const TexView& t, const uint32_t s[4], const float uv[2], const float d[4],
          float out[4]) {
    if (s[0] & kSampleFiltered)
        SampleTextureCpu(t, s, uv, d, d + 2, out);
    else
        Texel(t, uv, out);
}

// pixel x, y's colour; the picture behind it is read where RefractUv puts
// it, across the target (mesh.hlsl reads it likewise); u and b the
// tangent and bitangent of a normal-mapped draw. With ds.lod, quad has the
// uv at the pixels its derivatives are taken between, as the GPU's
// ddx_fine and ddy_fine take them (RasterTri): the two of its 2x2 quad in
// its row, then the two in its column.
void Shade(const DrawState& ds, int x, int y, const float uv[2], const float n[3],
           const float vc[4], const float wp[3], float depth, const float ao[2],
           const float ld[3], const float la[3], const float u[3], const float b[3],
           const float quad[8], float out[4]) {
    float texel[4] = {1, 1, 1, 1}, spec_map[4] = {1, 1, 1, 1}, glow[4] = {0, 0, 0, 0};
    float behind[4] = {1, 1, 1, 1};
    float d[4] = {0, 0, 0, 0};
    if (ds.lod) {
        d[0] = quad[2] - quad[0];
        d[1] = quad[3] - quad[1];
        d[2] = quad[6] - quad[4];
        d[3] = quad[7] - quad[5];
    }
    if (ds.tex.px) Read(ds.tex, ds.samp_tex, uv, d, texel);
    if (ds.spec_map.px) Read(ds.spec_map, ds.samp_spec, uv, d, spec_map);
    if (ds.glow.px) Read(ds.glow, ds.samp_glow, uv, d, glow);
    shade::NormalMapInputs nm{};
    if (ds.normal_map) {
        for (int i = 0; i < 3; i++) nm.u[i] = u[i];
        for (int i = 0; i < 3; i++) nm.b[i] = b[i];
        Read(ds.normal, ds.samp_normal, uv, d, nm.map);
        if (ds.detail.px) {
            float duv[2], dd[4] = {0, 0, 0, 0};
            shade::DetailUvCpu(ds.shade, uv, duv);
            if (ds.lod) {
                // the detail map's uv is the uv scaled: its derivatives are
                // taken between the same pixels' (mesh.hlsl's likewise)
                float q[8];
                for (int k = 0; k < 4; k++) shade::DetailUvCpu(ds.shade, quad + 2 * k, q + 2 * k);
                dd[0] = q[2] - q[0];
                dd[1] = q[3] - q[1];
                dd[2] = q[6] - q[4];
                dd[3] = q[7] - q[5];
            }
            Read(ds.detail, ds.samp_detail, duv, dd, nm.detail);
        }
    }
    if (ds.behind.px) {
        // the clip position the game's pixel shader is given there (its x, y
        // over w are where in the viewport the pixel samples), and where its
        // refract normal map moves that, read bilinear and clamped
        const float sx = (float(x) + ds.centre - ds.view[0]) / ds.view[2];
        const float sy = (float(y) + ds.centre - ds.view[1]) / ds.view[3];
        const float clip[2] = {(sx * 2 - 1) * depth, (1 - sy * 2) * depth};
        float map[4] = {0.5f, 0.5f, 0, 1}, at[2];
        if (ds.shade.flags.x & shade::kShadeRefractMap)
            Read(ds.normal, ds.samp_normal, uv, d, map);
        shade::RefractUvCpu(ds.shade, clip, depth, map, at);
        SampleLinear(ds.behind, at[0], at[1], behind);
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
// The colour and alpha are clamped to 0..1 first, as an 8-bit target clamps
// what reaches its blender, so before SrcAlpha's scaling too: a lit colour of
// 1.5 multiplies by 1 and adds a * 1. The game's pictures agree: the menus'
// blue header, Multiply and SrcAlphaAdd layers lit above 1, matches them only
// clamped this way. gpu_view.cpp's UNORM target and mesh.hlsl's FinishMesh do
// the same.
// RndMat's Screen, Lighten and Darken (8..10) are drawn as Src too: NgMat's
// SetupShader sets no blend state for them (rb3-xenon Mat_NG.cpp's switch
// falls to default, and the second one asserts), so on the 360 they never
// appear.
uint32_t Blend(int mode, const float s[4], uint32_t dst, AlphaRule alpha) {
    float d[4];
    for (int i = 0; i < 4; i++) d[i] = float((dst >> (8 * i)) & 0xff) / 255.0f;
    float o[4];
    const float a = std::clamp(s[3], 0.0f, 1.0f);
    float c[3];
    for (int i = 0; i < 3; i++) c[i] = std::clamp(s[i], 0.0f, 1.0f);
    for (int i = 0; i < 3; i++) {
        switch (mode) {
            case 2: o[i] = d[i] + c[i]; break;                    // Add
            case 3: o[i] = c[i] * a + d[i] * (1.0f - a); break;  // SrcAlpha
            case 4: o[i] = d[i] + c[i] * a; break;               // SrcAlphaAdd
            case 5: o[i] = d[i] - c[i]; break;                   // Subtract
            case 6: o[i] = d[i] * c[i]; break;                   // Multiply
            // PreMultAlpha, ONE INVSRCALPHA: the colour comes scaled by alpha
            // (c0 by SetupShader's PreMultiplyAlpha, the texture as it is)
            case 7: o[i] = c[i] + d[i] * (1.0f - a); break;
            case 0: o[i] = d[i]; break;  // Dest
            default: o[i] = c[i]; break;  // Src
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
    // the depth each corner tests and writes, (p w + q + r z) / w, which
    // runs straight across the screen as 1/w does
    float dv[3] = {iw[0], iw[1], iw[2]};
    if (ds.depth_mapped)
        for (int i = 0; i < 3; i++)
            dv[i] = ds.depth_map.p + (ds.depth_map.q + ds.depth_map.r * v[i]->p[2]) * iw[i];
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

    // the barycentrics at (qx, qy), inside the triangle or not
    auto bary = [&](float qx, float qy, float l[3]) {
        l[0] = ((sx[2] - sx[1]) * (qy - sy[1]) - (sy[2] - sy[1]) * (qx - sx[1])) * inv_area;
        l[1] = ((sx[0] - sx[2]) * (qy - sy[2]) - (sy[0] - sy[2]) * (qx - sx[2])) * inv_area;
        l[2] = ((sx[1] - sx[0]) * (qy - sy[0]) - (sy[1] - sy[0]) * (qx - sx[0])) * inv_area;
    };
    auto inside = [&](const float l[3]) {
        if (l[0] < 0 || l[1] < 0 || l[2] < 0) return false;
        return !((l[0] == 0 && !own0) || (l[1] == 0 && !own1) || (l[2] == 0 && !own2));
    };
    // the depth tested and written where the barycentrics are l
    auto depth_at = [&](const float l[3]) {
        return ds.depth_mapped ? l[0] * dv[0] + l[1] * dv[1] + l[2] * dv[2]
                               : l[0] * iw[0] + l[1] * iw[1] + l[2] * iw[2];
    };
    // pixel x, y's colour, shaded where it samples (px, py; barycentrics l
    // there, z its 1/w); false if the alpha test drops it
    auto shade_at = [&](int x, int y, float px, float py, const float l[3], float z,
                        float col[4]) {
        const float q0 = l[0] * iw[0] / z, q1 = l[1] * iw[1] / z, q2 = l[2] * iw[2] / z;
        float uv[2], n[3], vc[4], wp[3];
        for (int i = 0; i < 2; i++) uv[i] = q0 * a.uv[i] + q1 * b.uv[i] + q2 * c.uv[i];
        // the uv where the GPU takes its derivatives (Shade): on the
        // plane the pixel's uv is on, perspective-correct, at the other
        // pixels of its 2x2 quad, inside the triangle or not
        float quad[8] = {};
        if (ds.lod) {
            auto uv_at = [&](float qx, float qy, float* out) {
                float m[3];
                bary(qx, qy, m);
                const float mz = m[0] * iw[0] + m[1] * iw[1] + m[2] * iw[2];
                const float r0 = m[0] * iw[0] / mz, r1 = m[1] * iw[1] / mz, r2 = m[2] * iw[2] / mz;
                for (int i = 0; i < 2; i++) out[i] = r0 * a.uv[i] + r1 * b.uv[i] + r2 * c.uv[i];
            };
            const float qx = float(x & ~1) + ds.centre, qy = float(y & ~1) + ds.centre;
            uv_at(qx, py, quad);
            uv_at(qx + 1.0f, py, quad + 2);
            uv_at(px, qy, quad + 4);
            uv_at(px, qy + 1.0f, quad + 6);
        }
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
        if (ds.spot) {
            SpotPixel(ds, t, x, y, wp, 1.0f / z, col);
        } else {
            Shade(ds, x, y, uv, n, vc, wp, 1.0f / z, ao, ld, la, tu, tb, quad, col);
            if (ds.soft) col[3] *= SoftPixelFade(ds, t, x, y, 1.0f / z);
        }
        return !shade::AlphaCutCpu(ds.shade, col[3]);
    };
    // whether depth dz passes the test against d, what's there
    auto passes = [&](float dz, float d) {
        return !ds.z_test || (ds.z_equal_passes ? !(dz < d * 0.9999f) : !(dz <= d));
    };

    // Multisampled (the overlay's: Target::samples), as a GPU's MSAA: the
    // samples the triangle covers, by the same rule as a pixel's centre; a
    // pixel with any is shaded once, where it samples (its centre, inside the
    // triangle or not, as the GPU's interpolants are without centroid), and
    // each covered sample whose depth, its own, passes takes the colour
    if (t.samples > 1) {
        const uint32_t ns = t.samples;
        const float(*pos)[2] = SamplePositions(ns);
        for (int y = int(min_y); y <= int(max_y); y++) {
            const float py = float(y) + ds.centre;
            for (int x = int(min_x); x <= int(max_x); x++) {
                const float px = float(x) + ds.centre;
                const size_t idx = size_t(y) * t.w + x;
                uint32_t* scol = t.ms_color + idx * ns;
                float* sdep = t.ms_depth + idx * ns;
                uint32_t pass = 0;
                float dz[4];
                for (uint32_t k = 0; k < ns; k++) {
                    float l[3];
                    bary(px + pos[k][0], py + pos[k][1], l);
                    if (!inside(l)) continue;
                    dz[k] = depth_at(l);
                    if (passes(dz[k], sdep[k])) pass |= 1u << k;
                }
                if (!pass) continue;
                float l[3];
                bary(px, py, l);
                const float z = l[0] * iw[0] + l[1] * iw[1] + l[2] * iw[2];
                float col[4];
                if (!shade_at(x, y, px, py, l, z, col)) continue;
                for (uint32_t k = 0; k < ns; k++) {
                    if (!(pass & (1u << k))) continue;
                    // Dest draws no colour
                    if (ds.blend != 0 || ds.alpha == AlphaRule::kMax) {
                        scol[k] = Blend(ds.blend, col, scol[k], ds.alpha);
                        if (k == 0 && t.ids && ds.blend != 0) (*t.ids)[idx] = ds.index;
                    }
                    if (ds.z_write) sdep[k] = dz[k];
                }
                st.pixels++;
            }
        }
        return;
    }

    for (int y = int(min_y); y <= int(max_y); y++) {
        const float py = float(y) + ds.centre;
        for (int x = int(min_x); x <= int(max_x); x++) {
            const float px = float(x) + ds.centre;
            float l[3];
            bary(px, py, l);
            if (!inside(l)) continue;
            const size_t idx = size_t(y) * t.w + x;
            if (ds.depth_only) {
                // clip z/w, which runs straight across the screen; LESS
                const float zw =
                    l[0] * a.p[2] * iw[0] + l[1] * b.p[2] * iw[1] + l[2] * c.p[2] * iw[2];
                if (zw < t.zw[idx]) t.zw[idx] = zw;
                st.pixels++;
                continue;
            }
            const float z = l[0] * iw[0] + l[1] * iw[1] + l[2] * iw[2];
            const float dz = ds.depth_mapped ? depth_at(l) : z;
            if (!passes(dz, t.depth[idx])) continue;
            float col[4];
            if (!shade_at(x, y, px, py, l, z, col)) continue;
            // Dest draws no colour, but may still write the scene's alpha
            if (ds.blend != 0 || ds.alpha == AlphaRule::kMax) {
                t.color[idx] = Blend(ds.blend, col, t.color[idx], ds.alpha);
                if (t.ids && ds.blend != 0) (*t.ids)[idx] = ds.index;
            }
            if (ds.z_write) t.depth[idx] = dz;
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
    // its pass's size in the game (Pass::width, height), which w x h is
    // unless it's drawn bigger (PassTargetSize)
    uint32_t game_w = 0, game_h = 0;
    std::vector<uint32_t> color;
    // its mips, made after each pass from what it drew (BuildMips), where
    // the texture has them and filtering is on; empty otherwise
    MipChain mips;
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
                return {f->second.w, f->second.h, f->second.color.data(), MipsOf(f->second.mips)};
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
            return {f->second.w, f->second.h, f->second.color.data(), MipsOf(f->second.mips)};
    }
    if (o.rt_guest_pixels && !rt->rgba.empty()) return View(rt);
    if (o.texture_passes) st.rt_missing++;
    return {};
}

void DrawOne(const DrawItem& it, int32_t index, const ShadeState* state, const RasterOptions& o,
             const RtTargets& rts, Target& t, RasterStats& st, std::vector<ClipVert>& cv,
             const TexView& density = {}, const DepthMap& depth = {},
             const float* overlay_edge = nullptr) {
    const Geometry& g = *it.geom;
    const bool skinned = o.skinning && !it.bones.empty();
    DrawState ds;
    ds.index = index;
    ds.depth_mapped = !depth.Identity();
    ds.depth_map = depth;
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
    // REFRACT_WORLD's refract normal map is s1 too, read as a normal map is
    if (ds.shade.flags.x & shade::kShadeRefractMap) {
        if (!ds.normal.px) ds.normal = NormalMap(state, kMapNormal, o, rts, t, st);
        if (!ds.normal.px) ds.shade.flags.x &= ~shade::kShadeRefractMap;
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
            shade::RescaleShadowCoord(ds.shade, f->second.game_w, f->second.game_h, ds.shadow_w,
                                      ds.shadow_h);
        } else {
            ds.shade.flags.x &= ~shade::kShadeShadow;
        }
    }
    // the samplers they're read with: the game's where the capture kept them
    // and filtering is on, else the old nearest (TexSampler's default)
    {
        const TexSampler none;
        auto pack = [&](const TexSampler& s, const TexView& v, uint32_t out[4]) {
            PackSampler(o.filtering && state ? s : none, v.Levels(), out);
            ds.lod |= v.px && (out[0] & kSampleFiltered) != 0;
        };
        pack(state ? state->diffuse_sampler : none, ds.tex, ds.samp_tex);
        pack(state ? state->samplers[kMapSpecular] : none, ds.spec_map, ds.samp_spec);
        pack(state ? state->samplers[kMapGlow] : none, ds.glow, ds.samp_glow);
        pack(state ? state->samplers[kMapNormal] : none, ds.normal, ds.samp_normal);
        pack(state ? state->samplers[kMapDetailNormal] : none, ds.detail, ds.samp_detail);
        // a cone reads its cross-section texture its own way (SpotPixel)
        if (ds.spot) ds.lod = false;
    }
    ds.depth_only = t.zw != nullptr;
    ds.per_vertex = (ds.shade.flags.x & shade::kShadePerVertex) != 0;
    ds.normal_map = (ds.shade.flags.x & shade::kShadeNormalMap) != 0;
    // REFRACT_WORLD reads the picture behind it: in the picture, once
    // resolved; in the world, the pre-process buffer (Run's world_behind)
    if (ds.shade.flags.x & shade::kShadeRefract) {
        if (t.behind) {
            ds.behind = {t.w, t.h, t.behind};
            ds.view[0] = t.vx;
            ds.view[1] = t.vy;
            ds.view[2] = t.vw;
            ds.view[3] = t.vh;
        } else {
            ds.shade.flags.x &= ~(shade::kShadeRefract | shade::kShadeRefractMap);
        }
    }
    const bool ao_sh = (ds.shade.flags.x & shade::kShadeAoSh) != 0;
    const bool billboard = (ds.shade.flags.x & shade::kShadeBillboard) != 0;
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
        } else if (billboard) {
            // turned to the camera at the instance's translation
            const float zero[3] = {0, 0, 0};
            shade::BillboardCpu(ds.shade, v.pos, it.world.m[3], wp);
            shade::BillboardCpu(ds.shade, nrm, zero, wn);
            if (ao_sh) shade::BillboardCpu(ds.shade, dir, zero, wd);
            if (ds.normal_map) shade::BillboardCpu(ds.shade, tangent, zero, wu);
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
        StretchEdges(c.p, overlay_edge);
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
// In pass p's target drawn bigger than the game's (PassTargetSize), the
// rect, in the game's texels, is scaled with it, and each tap reads the
// texels the game's covers (BlurSubTaps); null p (or its size) is the game's.
void TapBlurDraw(const DrawItem& it, const ShadeState& s, const RasterOptions& o,
                 const TexView& src, Target& t, RasterStats& st, const Pass* p = nullptr) {
    float rect[4] = {it.rect[0], it.rect[1], it.rect[2], it.rect[3]};
    BlurSubTaps sub;
    if (p && (t.w != p->width || t.h != p->height)) {
        const float sx = float(t.w) / float(p->width), sy = float(t.h) / float(p->height);
        rect[0] *= sx;
        rect[1] *= sy;
        rect[2] *= sx;
        rect[3] *= sy;
        sub = BlurSubTapsFor(s, spot::kSpotBlurTaps, *p, t.w, t.h);
    }
    const int x0 = std::clamp(int(std::floor(rect[0])), 0, int(t.w));
    const int y0 = std::clamp(int(std::floor(rect[1])), 0, int(t.h));
    const int x1 = std::clamp(int(std::ceil(rect[0] + rect[2])), x0, int(t.w));
    const int y1 = std::clamp(int(std::ceil(rect[1] + rect[3])), y0, int(t.h));
    if (rect[2] <= 0 || rect[3] <= 0) return;
    const int blend = o.blending ? it.blend : 1;
    for (int y = y0; y < y1; y++) {
        const float v = (float(y) + 0.5f - rect[1]) / rect[3];
        for (int x = x0; x < x1; x++) {
            const float u = (float(x) + 0.5f - rect[0]) / rect[2];
            float sum[4] = {0, 0, 0, 0};
            for (int i = 0; i < spot::kSpotBlurTaps; i++) {
                const float* off = s.Ps(31 + i);
                const float* weight = s.Ps(47 + i);
                float tap[4];
                if (sub.count <= 1) {
                    SampleLinear(src, u + off[0], v + off[1], tap);
                } else {
                    // post.hlsl's PSBlur likewise
                    const float n = float(sub.count);
                    const float first[2] = {-0.5f * (n - 1) * sub.step[0],
                                            -0.5f * (n - 1) * sub.step[1]};
                    float mean[4] = {0, 0, 0, 0};
                    for (uint32_t j = 0; j < sub.count; j++) {
                        SampleLinear(src, u + off[0] + first[0] + float(j) * sub.step[0],
                                     v + off[1] + first[1] + float(j) * sub.step[1], tap);
                        for (int c = 0; c < 4; c++) mean[c] += tap[c];
                    }
                    for (int c = 0; c < 4; c++) tap[c] = mean[c] / n;
                }
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
                  RasterStats& st, const Pass& p) {
    const std::vector<uint32_t> before = t.color;
    TapBlurDraw(it, s, o, {t.w, t.h, before.data()}, t, st, &p);
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
    if (o.post && o.view == RasterView::kFinal &&
        post::PlanPost(f, o.post_only, post_plan, o.grain, o.velocity)) {
        if (post_plan.composite.flags.x & post::kPostSpot) needed.insert(post_plan.spot_volume);
        if (post_plan.composite.flags.x & post::kPostSoft) needed.insert(post_plan.soft);
    }
    // A draw that samples a version of a texture no pass here made, where a
    // pass later in the frame makes the next one: a texture drawn every frame
    // (the title's clouds), sampled before this frame's pass draws it again,
    // whose pass of the frame before the capture didn't record (BeginPass in
    // scene_capture.cpp records a regular one only while capturing). That
    // pass is drawn first instead, a frame newer than the one sampled, which
    // the diffuse texture and normal maps read whatever its version is
    // (Diffuse, NormalMap): one frame of the clouds' drift, where without it
    // they'd be black. Not for version 0, which no pass ever made, a shadow
    // map, or a pass from post-processing on, which draws from this frame's
    // picture.
    std::unordered_set<uint64_t> made;
    for (const Pass& p : f.passes)
        if (p.tex_obj) made.insert(uint64_t(p.tex_obj) << 32 | p.version);
    std::vector<const Pass*> early;
    auto stand_in = [&](const Texture* t, uint32_t d) {
        if (!t->version || t->tex_type == kTexTypeShadowMap ||
            made.count(uint64_t(t->tex_obj) << 32 | t->version))
            return;
        for (const Pass& p : f.passes) {
            if (p.tex_obj != t->tex_obj || p.version != t->version + 1 || p.first_draw <= d)
                continue;
            if (p.first_draw >= f.post_boundary) return;
            if (std::find(early.begin(), early.end(), &p) == early.end()) early.push_back(&p);
            return;
        }
    };
    auto samples = [&](uint32_t first, uint32_t end, bool texture) {
        for (uint32_t d = first; d < end; d++) {
            const DrawItem& it = f.draws[d];
            if (texture ? !DrawnInTexturePass(it) : !DrawnToBackBuffer(it)) continue;
            if (o.textures && IsPassTarget(it.tex.get()) && SamplesDiffuse(it)) {
                needed.insert(it.tex->tex_obj);
                stand_in(it.tex.get(), d);
            }
            const ShadeState* state = shade::ShadeOf(f, it);
            // the projected light's s5, NgLight's shadow
            if (o.textures)
                if (const Texture* map = ProjectedTargetOf(state)) needed.insert(map->tex_obj);
            // a SHADOW_BUFFER draw's shadow map (s5)
            if (o.self_shadow)
                if (const Texture* map = ShadowMapOf(state)) needed.insert(map->tex_obj);
            // a head's normal map (s1), or a detail map RB3 draws (s14)
            if (o.textures && o.normal_maps)
                for (int m : {kMapNormal, kMapDetailNormal}) {
                    if (const Texture* map = MapTargetOf(state, m)) {
                        needed.insert(map->tex_obj);
                        stand_in(map, d);
                    }
                }
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
    // the stand-ins first, whether or not something after them wanted them
    // where they are
    if (o.texture_passes) {
        for (const Pass* p : early) {
            const bool kept = std::any_of(runs.begin(), runs.end(),
                                          [&](const PassRun& r) { return r.pass == p; });
            if (kept || !p->width || !p->height) continue;
            const uint32_t first = std::min(p->first_draw, n);
            runs.push_back({p, first, std::min(first + p->draw_count, n)});
        }
    }
    std::reverse(runs.begin(), runs.end());
    if (!early.empty())
        std::stable_partition(runs.begin(), runs.end(), [&](const PassRun& r) {
            return r.pass && std::find(early.begin(), early.end(), r.pass) != early.end();
        });
    return runs;
}

// One of the world passes drawn before a frame whose world refracts, with
// no pre-process buffer kept for it (kPreBufferPasses): its REFRACT_WORLD
// draws read `behind` (width x height), and the scene its world leaves goes
// into `scene`, where it ends
struct PrePass {
    const uint32_t* behind;
    std::vector<uint32_t>* scene;
};

// Rasterize(), and the texture targets it drew: `stop` (a DxTex, 0 none)
// ends the frame after `stop_version` of it (0 its last) is drawn; with
// `pre`, it's that world pass and ends at post_boundary
RasterStats Run(const FrameCapture& frame, const RasterOptions& o, std::vector<uint32_t>& rgba,
                std::vector<int32_t>* ids, RtTargets& rts, uint32_t stop,
                uint32_t stop_version, const PrePass* pre = nullptr) {
    const auto start = std::chrono::steady_clock::now();
    RasterStats st;
    const size_t pixels = size_t(o.width) * o.height;
    const uint32_t clear = ClearRgba(frame);
    rgba.assign(pixels, clear);
    if (ids) ids->assign(pixels, -1);
    std::vector<float> depth(pixels, 0.0f);
    // the world's draws into the scene target, cleared with alpha 0; the
    // overlay's into the picture, over the depth the world left (cleared, in
    // a capture with its cameras)
    std::vector<uint32_t> scene(pixels, clear & 0x00ffffffu);
    Target world{o.width, o.height, scene, depth, ids};
    world.alpha = TargetAlpha::kScene;
    world.SetViewport(0, 0, float(o.width), float(o.height));
    Target overlay{o.width, o.height, rgba, depth, ids};
    overlay.SetViewport(0, 0, float(o.width), float(o.height));
    Target* back = &world;
    // the overlay's samples, if it's multisampled (OverlaySamples)
    const uint32_t samples = OverlaySamples(o);
    std::vector<uint32_t> ms_color;
    std::vector<float> ms_depth;
    // the picture as the resolve leaves it, kept if an overlay draw reads it
    std::vector<uint32_t> behind;
    bool refracts = false;
    for (size_t i = frame.post_boundary; i < frame.draws.size() && !refracts; i++) {
        const DrawItem& it = frame.draws[i];
        refracts = DrawnToBackBuffer(it) && RefractsWorld(shade::ShadeOf(frame, it));
    }
    post::PostPlan post_plan;
    const bool post_on = o.post && o.view == RasterView::kFinal &&
                         post::PlanPost(frame, o.post_only, post_plan, o.grain, o.velocity);
    // the post buffer (RasterOptions::post_buffer): a post frame's picture
    // kept, and shown by the frames after it that post-process nothing, in
    // place of their world, which isn't drawn
    post::PostHistory* const kept =
        o.post_buffer && o.view == RasterView::kFinal ? o.post_history : nullptr;
    const bool shows_kept = kept && ShowsPostBuffer(frame) && kept->picture_w == o.width &&
                            kept->picture_h == o.height &&
                            PostBufferFor(frame, kept->picture_frame);
    const bool keeps = kept && ProcKnown(frame) && (frame.proc_cmds & kProcPost);
    // The world's REFRACT_WORLD draws read the pre-process buffer (RefractsWorld,
    // RasterOptions::pre_buffer): the one kept from the world frames before,
    // or else the world drawn kPreBufferPasses times first; black where
    // neither (a view of the scene target, whose alpha and depth they don't
    // change, or a texture target's dump). This frame's world is kept in turn.
    std::vector<uint32_t> world_behind;
    const bool world_refracts = !shows_kept && WorldRefracts(frame);
    post::PostHistory* const pre_kept =
        world_refracts && !pre && o.pre_buffer && o.view == RasterView::kFinal ? o.post_history
                                                                               : nullptr;
    if (world_refracts) {
        if (pre) {
            world.behind = pre->behind;
        } else if (pre_kept && pre_kept->pre_w == o.width && pre_kept->pre_h == o.height &&
                   PreBufferFor(frame, pre_kept->pre_frame)) {
            world.behind = pre_kept->pre.data();
        } else {
            world_behind.assign(pixels, 0);
            if (o.view == RasterView::kFinal && !stop) {
                // the world alone, on its own targets, with nothing of the
                // live view's: no post-processing, history or samples
                RasterOptions po = o;
                po.post = po.trails = po.post_buffer = po.pre_buffer = false;
                po.post_history = nullptr;
                po.post_bloom0 = nullptr;
                po.msaa = 1;
                std::vector<uint32_t> picture, next;
                for (int k = 0; k < kPreBufferPasses; k++) {
                    RtTargets pre_rts;
                    const PrePass pass{world_behind.data(), &next};
                    Run(frame, po, picture, nullptr, pre_rts, 0, 0, &pass);
                    world_behind.swap(next);
                }
            }
            world.behind = world_behind.data();
        }
    }
    const BackBufferLayout layout = LayoutBackBuffer(frame);
    // the scene into the picture, at post_boundary (or the frame's end):
    // post-processed, or as it is. A view of the scene target ends the frame
    // there. With the capture's cameras, the overlay's depth starts cleared
    // after it, as RB3's does (DxRnd::DoPostProcess clears its offscreen
    // target's to 0 before the overlay draws: BeginTiling). Multisampled,
    // the overlay's samples start as the picture, each of them, as RB3's
    // CopyPostProcess draws it into its 2x target, and their depth cleared,
    // with the cameras, or else the world's (`overlay_follows`: there are
    // overlay draws to come, not the frame's end).
    auto resolve = [&](bool overlay_follows) {
        back = &overlay;
        // the scene as the world left it, before post-processing, as
        // DoWorldEnd's SavePreBuffer keeps it: for the next world frame's
        // REFRACT_WORLD draws, or the next world pass's
        if (pre) {
            *pre->scene = scene;
        } else if (pre_kept) {
            pre_kept->pre = scene;
            pre_kept->pre_w = o.width;
            pre_kept->pre_h = o.height;
            pre_kept->pre_frame = WorldFrameOf(frame);
        }
        if (shows_kept) {
            rgba = kept->picture;
        } else if (post_on) {
            // the spotlights' passes, drawn before it (Plan keeps them)
            auto image = [&](uint32_t tex_obj) {
                const auto f = rts.find(tex_obj);
                if (!tex_obj || f == rts.end()) return post::PostImage{};
                return post::PostImage{f->second.color.data(), f->second.w, f->second.h};
            };
            // the depth buffer has 1/w, as RunPost wants it
            post::RunPost(post_plan, scene, depth, o.width, o.height,
                          image(post_plan.spot_volume), image(post_plan.spot_density),
                          image(post_plan.soft), rgba, o.post_bloom0,
                          o.trails ? o.post_history : nullptr, frame.game_frame);
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
        if (keeps) {
            kept->picture = rgba;
            kept->picture_w = o.width;
            kept->picture_h = o.height;
            kept->picture_frame = frame.game_frame;
        }
        if (refracts) {
            behind = rgba;
            overlay.behind = behind.data();
        }
        if (samples > 1 && overlay_follows) {
            ms_color.resize(pixels * samples);
            ms_depth.resize(pixels * samples);
            for (size_t i = 0; i < pixels; i++) {
                for (uint32_t k = 0; k < samples; k++) {
                    ms_color[i * samples + k] = rgba[i];
                    ms_depth[i * samples + k] = layout.cameras ? 0.0f : depth[i];
                }
            }
            overlay.samples = samples;
            overlay.ms_color = ms_color.data();
            overlay.ms_depth = ms_depth.data();
        } else if (layout.cameras && o.view == RasterView::kFinal) {
            std::fill(depth.begin(), depth.end(), 0.0f);
        }
    };
    std::vector<ClipVert> cv;
    std::unordered_set<uint32_t> cams_seen;
    uint32_t last_cam = 0;
    // the density map the spotlights' cones read: the last drawn
    uint32_t density = 0;
    for (const PassRun& run : Plan(frame, o, stop, stop_version)) {
        // a world pass ends with the world
        if (pre && back == &overlay) break;
        if (!run.pass) {
            for (uint32_t i = run.first; i < run.end; i++) {
                const DrawItem& it = frame.draws[i];
                if (!DrawnToBackBuffer(it) || (shows_kept && i < frame.post_boundary)) continue;
                if (back == &world && i >= frame.post_boundary) resolve(true);
                if (back == &overlay && (o.view != RasterView::kFinal || pre)) break;
                // (a DrawRect quad has no camera of its own)
                if (it.rect_shader < 0) {
                    if (o.clear_depth_per_camera && !layout.cameras && it.cam != last_cam &&
                        cams_seen.insert(it.cam).second) {
                        if (back->samples > 1)
                            std::fill(ms_depth.begin(), ms_depth.end(), 0.0f);
                        else
                            std::fill(depth.begin(), depth.end(), 0.0f);
                    }
                    last_cam = it.cam;
                }
                if (!Drawable(it) || !DrawnByOptions(frame, it, o)) continue;
                float vp[4];
                DepthMap dm;
                PlaceBackBufferDraw(layout, frame, it, o.width, o.height, vp, dm);
                back->SetViewport(vp[0], vp[1], vp[2], vp[3]);
                // the overlay's camera draws over the whole picture reach its
                // edges (RasterOptions::overlay_edge), as gpu_view.cpp's do
                const bool whole = vp[0] == 0.0f && vp[1] == 0.0f &&
                                   vp[2] == float(o.width) && vp[3] == float(o.height);
                const bool overlay_whole = back == &overlay && whole && it.rect_shader < 0;
                // the song list's rows cut at 16:9 instead (InOverlayCut)
                const bool cut = overlay_whole && InOverlayCut(it, o);
                if (cut) {
                    const float e = o.overlay_edge[0];
                    back->x0 = std::max(back->x0, int(std::lround((1 - e) / 2 * float(o.width))));
                    back->x1 = std::min(back->x1, int(std::lround((1 + e) / 2 * float(o.width))));
                }
                const float* edge = overlay_whole && !cut ? o.overlay_edge : nullptr;
                DrawOne(it, int32_t(i), shade::ShadeOf(frame, it), o, rts, *back, st, cv, {}, dm,
                        edge);
            }
            continue;
        }
        // into the texture's own target, made at its size (again if that
        // changed) and cleared as DxCam::Select cleared it; one no camera
        // cleared (or NgLight cleared: PassClearFlags) starts transparent black
        const Pass& p = *run.pass;
        RtTarget& rt = rts[p.tex_obj];
        const bool shadow_map = p.tex_type == kTexTypeShadowMap;
        uint32_t tw, th;
        PassTargetSize(frame, p, o, tw, th);
        rt.game_w = p.width;
        rt.game_h = p.height;
        if (rt.w != tw || rt.h != th) {
            rt.w = tw;
            rt.h = th;
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
            if (!DrawnInTexturePass(it) || !Drawable(it) || !DrawnByOptions(frame, it, o))
                continue;
            const ShadeState* state = shade::ShadeOf(frame, it);
            if (spot::SpotBlur(it, state, p)) {
                SpotBlurDraw(it, *state, o, rtt, st, p);
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
                TapBlurDraw(it, *state, o, src, rtt, st, &p);
                continue;
            }
            // the camera's viewport, scaled with the target; DrawRect's quads
            // are in the target's pixels, over all of it
            if (it.rect_shader < 0 && p.viewport[2] > 0 && p.viewport[3] > 0) {
                float vp[4];
                ScalePassViewport(p, rt.w, rt.h, vp);
                rtt.SetViewport(vp[0], vp[1], vp[2], vp[3]);
            } else {
                rtt.SetViewport(0, 0, float(rt.w), float(rt.h));
            }
            DrawOne(it, int32_t(i), state, o, rts, rtt, st, cv, density_view);
        }
        // its mips, from what it holds now, as the GPU makes them after the
        // pass (gpu_view.cpp's TargetFor and SDL_GenerateMipmapsForGPUTexture)
        rt.mips.clear();
        if (o.filtering && !shadow_map && p.num_mips > 1)
            BuildMips(rt.color.data(), rt.w, rt.h, std::min(p.num_mips, FullMipChain(rt.w, rt.h)),
                      rt.mips);
        st.passes++;
        if (stop && p.tex_obj == stop && p.version == stop_version) break;
    }
    if (back == &world) resolve(false);
    // the overlay's samples averaged into the picture, as the GPU's resolve
    // and RB3's EndTiling do
    if (overlay.samples > 1)
        for (size_t i = 0; i < pixels; i++) rgba[i] = ResolvePixel(&ms_color[i * samples], samples);
    st.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
    return st;
}

}  // namespace

bool InOverlayCut(const DrawItem& it, const RasterOptions& o) {
    if (o.overlay_edge[0] >= 1.0f || o.overlay_cut[0] >= o.overlay_cut[1]) return false;
    if (!it.geom || it.geom->verts.empty() || !it.bones.empty()) return false;
    for (const Vertex& v : it.geom->verts) {
        float p[3], c[4];
        Point(v.pos, it.world, p);
        for (int col = 0; col < 4; col++)
            c[col] = p[0] * it.view_proj.m[0][col] + p[1] * it.view_proj.m[1][col] +
                     p[2] * it.view_proj.m[2][col] + it.view_proj.m[3][col];
        if (c[3] <= 0) return false;
        const float y = c[1] / c[3] / o.overlay_edge[1];
        if (y < o.overlay_cut[0] || y > o.overlay_cut[1]) return false;
    }
    return true;
}

bool SoftBlur(const FrameCapture& f, const DrawItem& d, const ShadeInputs* s, const Pass& p) {
    const uint32_t* surface = f.post_consts.soft_surface;
    if (d.rect_shader != 1 || !s || !p.tex_obj || !d.tex || d.tex->tex_obj == p.tex_obj ||
        (p.tex_obj != surface[0] && p.tex_obj != surface[1]))
        return false;
    float weights = 0;
    for (int i = 0; i < kSoftBlurTaps; i++) weights += s->Ps(47 + i)[0];
    return weights > 0;
}

void PassTargetSize(const FrameCapture& f, const Pass& p, const RasterOptions& o, uint32_t& w,
                    uint32_t& h) {
    w = p.width;
    h = p.height;
    const float scale = p.tex_type == kTexTypeShadowMap         ? o.shadow_scale
                        : SpotTarget(p) || SoftTarget(f, p) ? o.target_scale
                                                            : 1.0f;
    if (scale == 1.0f || !(scale > 0)) return;
    auto even = [&](uint32_t size) {
        return std::max<uint32_t>(2, 2 * uint32_t(std::lround(double(size) * scale / 2)));
    };
    w = even(p.width);
    h = even(p.height);
}

BlurSubTaps BlurSubTapsFor(const ShadeInputs& s, int taps, const Pass& p, uint32_t w,
                           uint32_t h) {
    BlurSubTaps sub;
    if (!p.width || !p.height || (w <= p.width && h <= p.height)) return sub;
    // the line the taps lie along: the axis their offsets spread over most
    float lo[2] = {0, 0}, hi[2] = {0, 0};
    for (int i = 0; i < taps; i++) {
        for (int k = 0; k < 2; k++) {
            lo[k] = i ? std::min(lo[k], s.Ps(31 + i)[k]) : s.Ps(31 + i)[k];
            hi[k] = i ? std::max(hi[k], s.Ps(31 + i)[k]) : s.Ps(31 + i)[k];
        }
    }
    // (in the game's texels)
    const bool across = (hi[0] - lo[0]) * float(p.width) >= (hi[1] - lo[1]) * float(p.height);
    const float scale = across ? float(w) / float(p.width) : float(h) / float(p.height);
    sub.count = std::clamp<uint32_t>(uint32_t(std::ceil(scale - 1e-3f)), 1, 8);
    if (sub.count > 1) {
        if (across) sub.step[0] = 1.0f / (float(p.width) * float(sub.count));
        else sub.step[1] = 1.0f / (float(p.height) * float(sub.count));
    }
    return sub;
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

bool WorldRefracts(const FrameCapture& frame) {
    const size_t end = std::min<size_t>(frame.post_boundary, frame.draws.size());
    for (size_t i = 0; i < end; i++) {
        const DrawItem& it = frame.draws[i];
        if (DrawnToBackBuffer(it) && RefractsWorld(shade::ShadeOf(frame, it))) return true;
    }
    return false;
}

uint32_t ClearRgba(const FrameCapture& frame) {
    if (!frame.has_clear_color) return kClearColor;
    uint32_t c = 0;
    for (int i = 0; i < 4; i++)
        c |= uint32_t(std::clamp(frame.clear_color[i], 0.0f, 1.0f) * 255.0f + 0.5f) << (8 * i);
    return c;
}

namespace {

// the a and b of view_proj's clip z = a w + b, false if its z isn't a
// multiple of its w plus a constant (an oblique or orthographic projection).
// Rows are vectors (v' = v M), so z and w are columns 2 and 3: z's xyz a
// times w's, and its translation a times w's plus b.
bool PerspectiveZ(const Mat4& m, float& a, float& b) {
    double c2[3], c3[3], n3 = 0, n2 = 0, dot = 0;
    for (int i = 0; i < 3; i++) {
        c2[i] = m.m[i][2];
        c3[i] = m.m[i][3];
        n3 += c3[i] * c3[i];
        n2 += c2[i] * c2[i];
        dot += c2[i] * c3[i];
    }
    if (!(n3 > 1e-12)) return false;
    const double ka = dot / n3;
    double off = 0;
    for (int i = 0; i < 3; i++) off += (c2[i] - ka * c3[i]) * (c2[i] - ka * c3[i]);
    if (off > 1e-10 * std::max(n2, 1e-30)) return false;
    a = float(ka);
    b = float(double(m.m[3][2]) - ka * double(m.m[3][3]));
    return true;
}

}  // namespace

BackBufferLayout LayoutBackBuffer(const FrameCapture& frame) {
    BackBufferLayout l;
    l.cameras = !frame.cameras.empty();
    if (!l.cameras) return l;
    // the reference: the camera with the most mesh draws in the world
    const size_t end = std::min<size_t>(frame.post_boundary, frame.draws.size());
    std::unordered_map<uint32_t, uint32_t> counts;
    for (size_t i = 0; i < end; i++) {
        const DrawItem& d = frame.draws[i];
        if (d.target == 0 && d.rect_shader < 0 && CameraOf(frame, d.cam)) counts[d.cam]++;
    }
    uint32_t best = 0, most = 0;
    for (size_t i = 0; i < end; i++) {
        const uint32_t cam = frame.draws[i].cam;
        if (auto it = counts.find(cam); it != counts.end() && it->second > most) {
            best = cam;
            most = it->second;
        }
    }
    if (!most) return l;
    for (size_t i = 0; i < end; i++) {
        const DrawItem& d = frame.draws[i];
        if (d.cam != best || d.target || d.rect_shader >= 0) continue;
        const CameraView& c = *CameraOf(frame, best);
        float a, b;
        if (!PerspectiveZ(d.view_proj, a, b)) return l;
        const float range = c.zrange[1] - c.zrange[0];
        // d = B + A / w
        l.ref_a = -range * b;
        l.ref_b = 1.0f - c.zrange[0] - range * a;
        if (!(l.ref_a > 0)) return l;
        l.mapped = true;
        l.ref_zrange[0] = c.zrange[0];
        l.ref_zrange[1] = c.zrange[1];
        l.ref_proj[0] = a;
        l.ref_proj[1] = b;
        return l;
    }
    return l;
}

void PlaceBackBufferDraw(const BackBufferLayout& l, const FrameCapture& frame, const DrawItem& d,
                         uint32_t width, uint32_t height, float vp[4], DepthMap& depth) {
    vp[0] = vp[1] = 0;
    vp[2] = float(width);
    vp[3] = float(height);
    depth = DepthMap{};
    if (!l.cameras) return;
    if (d.rect_shader >= 0) {
        // the device's depth 1, w 1
        if (l.mapped) depth = {(1.0f - l.ref_b) / l.ref_a, 0, 0};
        return;
    }
    const CameraView* c = CameraOf(frame, d.cam);
    if (!c) return;
    if (c->target_w && c->target_h) {
        const float sx = float(width) / float(c->target_w), sy = float(height) / float(c->target_h);
        vp[0] = c->viewport[0] * sx;
        vp[1] = c->viewport[1] * sy;
        vp[2] = c->viewport[2] * sx;
        vp[3] = c->viewport[3] * sy;
    }
    if (!l.mapped) return;
    const float range = c->zrange[1] - c->zrange[0];
    float a, b;
    if (PerspectiveZ(d.view_proj, a, b)) {
        // the reference's own: 1/w as it is
        if (a == l.ref_proj[0] && b == l.ref_proj[1] && c->zrange[0] == l.ref_zrange[0] &&
            c->zrange[1] == l.ref_zrange[1])
            return;
        const float own_a = -range * b, own_b = 1.0f - c->zrange[0] - range * a;
        depth = {(own_b - l.ref_b) / l.ref_a, own_a / l.ref_a, 0};
        return;
    }
    depth = {(1.0f - c->zrange[0] - l.ref_b) / l.ref_a, 0, -range / l.ref_a};
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

namespace sample_cpu {
namespace {

// HLSL's types and functions, for sample_model.hlsli: only what it uses
using shade::float2;
using shade::float4;
using shade::uint;
using shade::uint4;
struct uint2 {
    uint x, y;
};
float floor(float v) { return std::floor(v); }
float ceil(float v) { return std::ceil(v); }
float sqrt(float v) { return std::sqrt(v); }
float log2(float v) { return std::log2(v); }
float max(float a, float b) { return a > b ? a : b; }
float min(float a, float b) { return a < b ? a : b; }
uint max(uint a, uint b) { return a > b ? a : b; }
uint min(uint a, uint b) { return a < b ? a : b; }
float asfloat(uint v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

float4 LoadTexel(const TexLevels& t, uint level, int x, int y) {
    const uint w = std::max(t.w >> level, 1u);
    const uint32_t c = t.Level(level)[size_t(y) * w + uint(x)];
    return float4(float(c & 0xff) / 255.0f, float((c >> 8) & 0xff) / 255.0f,
                  float((c >> 16) & 0xff) / 255.0f, float(c >> 24) / 255.0f);
}

#define SAMPLE_TEX const TexLevels& t
#define SAMPLE_ARGS t
#define SAMPLE_LOAD(level, x, y) LoadTexel(t, level, x, y)
#define SAMPLE_LOOP
#include "src/Render/shaders/sample_model.hlsli"
#undef SAMPLE_TEX
#undef SAMPLE_ARGS
#undef SAMPLE_LOAD
#undef SAMPLE_LOOP

}  // namespace
}  // namespace sample_cpu

void SampleTextureCpu(const TexLevels& t, const uint32_t s[4], const float uv[2],
                      const float dx[2], const float dy[2], float out[4]) {
    using namespace sample_cpu;
    const float4 c = SampleTexture(t, uint2{t.w, t.h}, uint4{s[0], s[1], s[2], s[3]},
                                   float2{uv[0], uv[1]}, float2{dx[0], dx[1]},
                                   float2{dy[0], dy[1]});
    out[0] = c.x;
    out[1] = c.y;
    out[2] = c.z;
    out[3] = c.w;
}

void BuildMips(const uint32_t* px, uint32_t w, uint32_t h, uint32_t levels,
               std::vector<std::vector<uint32_t>>& out) {
    out.clear();
    if (levels > 1) out.reserve(levels - 1);
    uint32_t sw = w, sh = h;
    for (uint32_t l = 1; l < levels; l++) {
        const uint32_t* src = l == 1 ? px : out.back().data();
        const uint32_t dw = std::max(sw >> 1, 1u), dh = std::max(sh >> 1, 1u);
        std::vector<uint32_t> dst(size_t(dw) * dh);
        for (uint32_t y = 0; y < dh; y++) {
            const float fy = (float(y) + 0.5f) * float(sh) / float(dh) - 0.5f;
            const float y0f = std::floor(fy), ty = fy - y0f;
            const int y0 = std::clamp(int(y0f), 0, int(sh) - 1);
            const int y1 = std::clamp(int(y0f) + 1, 0, int(sh) - 1);
            for (uint32_t x = 0; x < dw; x++) {
                const float fx = (float(x) + 0.5f) * float(sw) / float(dw) - 0.5f;
                const float x0f = std::floor(fx), tx = fx - x0f;
                const int x0 = std::clamp(int(x0f), 0, int(sw) - 1);
                const int x1 = std::clamp(int(x0f) + 1, 0, int(sw) - 1);
                const uint32_t c00 = src[size_t(y0) * sw + x0], c10 = src[size_t(y0) * sw + x1];
                const uint32_t c01 = src[size_t(y1) * sw + x0], c11 = src[size_t(y1) * sw + x1];
                uint32_t r = 0;
                for (int c = 0; c < 4; c++) {
                    auto ch = [&](uint32_t v) { return float((v >> (8 * c)) & 0xff); };
                    const float top = ch(c00) + (ch(c10) - ch(c00)) * tx;
                    const float bottom = ch(c01) + (ch(c11) - ch(c01)) * tx;
                    const float v = top + (bottom - top) * ty;
                    r |= uint32_t(std::clamp(v, 0.0f, 255.0f) + 0.5f) << (8 * c);
                }
                dst[size_t(y) * dw + x] = r;
            }
        }
        out.push_back(std::move(dst));
        sw = dw;
        sh = dh;
    }
}

}  // namespace band3::render
