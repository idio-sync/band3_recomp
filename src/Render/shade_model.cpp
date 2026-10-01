#include "src/Render/shade_model.h"

#include <algorithm>
#include <cmath>

// See shade_model.h.

namespace band3::render::shade {
namespace {

// HLSL's operators and functions, for shade.hlsli: only what it uses
float3 operator+(float3 a, float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
float3 operator-(float3 a, float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float3 operator*(float3 a, float3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
float3 operator*(float3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float3 operator/(float3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }
float3 operator-(float3 a) { return {-a.x, -a.y, -a.z}; }
float4 operator*(float4 a, float4 b) { return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}; }

float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float dot(float4 a, float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
float saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }
float3 saturate(float3 v) { return {saturate(v.x), saturate(v.y), saturate(v.z)}; }
float max(float a, float b) { return a > b ? a : b; }
float min(float a, float b) { return a < b ? a : b; }
float floor(float v) { return std::floor(v); }
float sqrt(float v) { return std::sqrt(v); }
float rsqrt(float v) { return 1.0f / std::sqrt(v); }
float pow(float x, float p) { return std::pow(x, p); }
float lerp(float a, float b, float t) { return a + (b - a) * t; }
float3 lerp(float3 a, float3 b, float t) { return a + (b - a) * t; }

#define SHADE_IN(T) const T&
#include "src/Render/shaders/shade.hlsli"
#undef SHADE_IN

}  // namespace

void TexGenUv(const ShadeParams& sp, const float uv[2], float out[2]) {
    const float2 r = TexGen(sp, float2{uv[0], uv[1]});
    out[0] = r.x;
    out[1] = r.y;
}

void AoShDirectionCpu(const float vc[4], float out[3]) {
    const float3 d = AoShDirection(float4{vc[0], vc[1], vc[2], vc[3]});
    out[0] = d.x;
    out[1] = d.y;
    out[2] = d.z;
}

float AoShRatioCpu(const ShadeParams& sp, uint light, const float p[3], const float n[3],
                   const float dir[3], float r) {
    return AoShRatio(sp, light, float3{p[0], p[1], p[2]}, float3{n[0], n[1], n[2]},
                     float3{dir[0], dir[1], dir[2]}, r);
}

void AoShVertexCpu(const ShadeParams& sp, const float p[3], const float n[3], const float dir[3],
                   const float vc[4], float out[2]) {
    const float2 ao = AoShVertex(sp, float3{p[0], p[1], p[2]}, float3{n[0], n[1], n[2]},
                                 float3{dir[0], dir[1], dir[2]},
                                 float4{vc[0], vc[1], vc[2], vc[3]});
    out[0] = ao.x;
    out[1] = ao.y;
}

void LightVertexCpu(const ShadeParams& sp, const float p[3], const float n[3], const float vc[4],
                    const float ao_sh[2], float diffuse[3], float added[3]) {
    const Lighting l = Light(sp, float3{p[0], p[1], p[2]}, float3{n[0], n[1], n[2]},
                             float4{vc[0], vc[1], vc[2], vc[3]}, float4{1, 1, 1, 1},
                             float2{ao_sh[0], ao_sh[1]}, float4{0, 0, 0, 0}, float4{0, 0, 0, 0},
                             1.0f);
    diffuse[0] = l.diffuse.x;
    diffuse[1] = l.diffuse.y;
    diffuse[2] = l.diffuse.z;
    added[0] = l.added.x;
    added[1] = l.added.y;
    added[2] = l.added.z;
}

void ProjUvCpu(const ShadeParams& sp, const float p[3], float out[2]) {
    const float2 uv = ProjUv(sp, float3{p[0], p[1], p[2]});
    out[0] = uv.x;
    out[1] = uv.y;
}

void ShadowCoordCpu(const ShadeParams& sp, const float p[3], float out[4]) {
    const float4 s = ShadowCoord(sp, float3{p[0], p[1], p[2]});
    out[0] = s.x;
    out[1] = s.y;
    out[2] = s.z;
    out[3] = s.w;
}

ShadowTapsCpu ShadowTapsOf(const float s[4], uint32_t w, uint32_t h) {
    const ShadowTapSet t = ShadowTaps(float4{s[0], s[1], s[2], s[3]}, float2{float(w), float(h)});
    ShadowTapsCpu out;
    const float xs[4] = {t.x.x, t.x.y, t.x.z, t.x.w}, ys[4] = {t.y.x, t.y.y, t.y.z, t.y.w};
    const float ws[4] = {t.weight.x, t.weight.y, t.weight.z, t.weight.w};
    for (int k = 0; k < 4; k++) {
        out.x[k] = int(xs[k]);
        out.y[k] = int(ys[k]);
        out.weight[k] = ws[k];
    }
    out.depth = t.depth;
    return out;
}

float ShadowLitCpu(const ShadeParams& sp, const float p[3], const float* depth, uint32_t w,
                   uint32_t h) {
    const ShadowTapSet t = ShadowTaps(ShadowCoord(sp, float3{p[0], p[1], p[2]}),
                                      float2{float(w), float(h)});
    auto at = [&](float x, float y) { return depth[size_t(y) * w + size_t(x)]; };
    return ShadowLit(t, float4{at(t.x.x, t.y.x), at(t.x.y, t.y.y), at(t.x.z, t.y.z),
                               at(t.x.w, t.y.w)});
}

void ShadePixelCpu(const ShadeParams& sp, const float p[3], const float n[3], const float vc[4],
                   const float texel[4], const float spec_map[4], const float glow[4],
                   const float behind[4], float depth, const float ao_sh[2],
                   const float vertex_diffuse[3], const float vertex_added[3], float out[4],
                   const float proj[4], const float gobo[4], float lit) {
    const Lighting vertex{float3{vertex_diffuse[0], vertex_diffuse[1], vertex_diffuse[2]},
                          float3{vertex_added[0], vertex_added[1], vertex_added[2]}};
    const float none[4] = {0, 0, 0, 0};
    if (!proj) proj = none;
    if (!gobo) gobo = none;
    const float4 r = ShadePixel(sp, float3{p[0], p[1], p[2]}, float3{n[0], n[1], n[2]},
                                float4{vc[0], vc[1], vc[2], vc[3]},
                                float4{texel[0], texel[1], texel[2], texel[3]},
                                float4{spec_map[0], spec_map[1], spec_map[2], spec_map[3]},
                                float4{glow[0], glow[1], glow[2], glow[3]},
                                float4{behind[0], behind[1], behind[2], behind[3]}, depth,
                                float2{ao_sh[0], ao_sh[1]},
                                float4{proj[0], proj[1], proj[2], proj[3]},
                                float4{gobo[0], gobo[1], gobo[2], gobo[3]}, lit, vertex);
    out[0] = r.x;
    out[1] = r.y;
    out[2] = r.z;
    out[3] = r.w;
}

bool AlphaCutCpu(const ShadeParams& sp, float alpha) { return AlphaCut(sp, alpha); }

float SoftFadeCpu(float far_plane, float inv_w, float w) {
    return SoftFade(SoftSceneDepth(far_plane, inv_w), w);
}

namespace {

void Copy(const float* from, float4& to) { to = {from[0], from[1], from[2], from[3]}; }

// RndShader's ShaderType for the material shader (ShadeInputs::shader_type)
constexpr int32_t kStandardShader = 18;

}  // namespace

void PackShade(const DrawItem& it, const ShadeState* s, const RasterOptions& o, bool textured,
               ShadeParams& sp) {
    using namespace shader_opt;
    sp = ShadeParams{};
    uint& f = sp.flags.x;
    if (textured) f |= kShadeTextured;
    if (it.alpha_cut) f |= kShadeAlphaCut;
    sp.alpha_cut.x = float(it.alpha_threshold);
    sp.texgen[0] = {1, 0, 0, 0};
    sp.texgen[1] = {0, 1, 0, 0};
    if (it.rect_shader >= 0 && (!s || s->shader_type != kStandardShader)) {
        // a DrawRect quad drawn with one of DxRnd's own shaders (blurs,
        // downsamples, the movie's) or no material: the texture times the
        // colour DrawRect gave its vertices. Those with a material's shader
        // (the outfit layers, TexBlender) are shaded as it says, below.
        sp.color = {1, 1, 1, 1};
        f |= kShadePrelit;
        return;
    }
    if (!s || o.legacy_light) {
        Copy(it.color, sp.color);
        if (it.prelit) f |= kShadePrelit;
        if (o.lighting) f |= kShadeLegacyLight;
        return;
    }

    f |= kShadeModel;
    // the pixel shader's registers (c0 is premultiplied for PreMultAlpha,
    // unlike the material's colour), but the texture transform and the
    // occlusion's strength, which only the vertex shader has
    Copy(s->Ps(0), sp.color);
    Copy(s->Ps(1), sp.ambient);
    Copy(s->Ps(2), sp.specular);
    Copy(s->Ps(5), sp.emissive);
    Copy(s->Ps(7), sp.bloom);
    sp.eye = {s->eye[0], s->eye[1], s->eye[2], 1};
    sp.ao.x = s->Vs(24)[0];
    Copy(s->Vs(20), sp.texgen[0]);
    Copy(s->Vs(21), sp.texgen[1]);
    for (int i = 0; i < 2; i++) {
        Copy(s->Ps(64 + i), sp.point_pos[i]);
        Copy(s->Ps(67 + i), sp.point_color[i]);
    }
    for (int i = 0; i < 6; i++) Copy(s->Ps(80 + i), sp.box[i]);
    Copy(s->Ps(63), sp.rim);
    for (int i = 0; i < 3; i++) Copy(s->Ps(53 + i), sp.fade[i]);
    Copy(s->Ps(104), sp.fade_color);
    for (int i = 0; i < 3; i++) Copy(s->Ps(95 + i), sp.proj[i]);
    Copy(s->Ps(66), sp.proj_dir);
    Copy(s->Ps(69), sp.proj_color);
    for (int i = 0; i < 4; i++) Copy(s->Vs(40 + i), sp.shadow[i]);
    Copy(s->Ps(107), sp.shadow_color);
    Copy(s->Ps(108), sp.shadow_dir);

    // Registers the option word doesn't use may hold anything from an earlier
    // draw, so every term is the option word's
    const bool particles = s->shader_type == 14;
    if (s->Option(kPrelit)) f |= kShadePrelit;
    if (s->Option(kIntensify)) f |= kShadeIntensify;
    // the backend takes it off where it has no picture to read (before the
    // resolve, or into a texture)
    if (RefractsWorld(s)) f |= kShadeRefract;
    // the luminance in alpha is for the back buffer's bloom: RB3's shaders
    // keep alpha into a texture ("not the main target", out/research/
    // m3_design.md 3), where it's the impostor's cut-out and a layer's blend
    if (s->Option(kPseudoHdr) && !it.target) f |= kShadePseudoHdr;
    const bool maps = o.textures;
    if (s->Option(kGlowMap) && maps && s->maps[kMapGlow]) f |= kShadeGlow;
    switch (s->OptionBits(kFadeOut, 2)) {
        case 1: f |= kShadeFadeAlpha; break;
        case 2: f |= kShadeFadeColor; break;
        default: break;
    }
    if (particles) {
        // colour = vertex colour times VS c1 times c0 (the particle VS,
        // 2E5F05321D973646 instrs 82 and 84; its PS is texture times that):
        // c1 is the ambient colour the draw was given, which tints fog and
        // smoke (the spotlight drawer's: green at the intro, blue in the
        // arena, out/research/spotlight_survey.md 3). The VS's own registers,
        // as the PS doesn't read them.
        Copy(s->Vs(0), sp.color);
        Copy(s->Vs(1), sp.ambient);
    }
    const bool lit = !particles && (s->Option(kRealLights) || s->Option(kApproxLights));
    if (!o.lighting || !lit) {
        // unlit; lighting off draws lit materials so too, with no ambient
        if (!o.lighting) sp.ambient = {1, 1, 1, 1};
        return;
    }
    f |= kShadeLit;
    if (!s->Option(kPerPixel)) f |= kShadePerVertex;
    // a box map of black (band3's disable_approximate_lights, the default)
    // adds nothing, and its specular is the costly part on the CPU
    bool box_lit = false;
    for (const float4& face : sp.box) box_lit |= face.x != 0 || face.y != 0 || face.z != 0;
    if (s->Option(kApproxLights) && box_lit) f |= kShadeBox;
    if (s->Option(kRealLights)) sp.flags.y = std::min<uint>(s->OptionBits(kNumPoint, 2), 2);
    if (s->Option(kSpecular)) f |= kShadeSpecular;
    if (s->Option(kSpecularMap) && maps && s->maps[kMapSpecular]) f |= kShadeSpecMap;
    if (s->Option(kEnableAO)) f |= kShadeAO;
    // every AO shader the dumps have with a point light occludes it by the
    // vertex colour's SH, none of those without one (out/research/
    // parity_diag_ao_refract.md)
    if (s->Option(kEnableAO) && sp.flags.y >= 1) f |= kShadeAoSh;
    // the projected light, where its maps were decoded: the multiply form
    // reads s5 alone, the gobo s10 too. The 18 pixel shaders the dumps have
    // that read it (c95) all light per pixel (fam3.py matches each); a
    // vertex-lit material's is left out. The multiply form's s5 is a texture
    // RB3 draws (the shadows' silhouettes, blurred), whose drawing the
    // capture leaves out: the capture's copy is guest memory's, right only
    // with --readback_resolve=full (stale otherwise, nearly empty).
    if (s->OptionBits(kNumProj, 2) != 0 && s->Option(kPerPixel) && maps &&
        s->maps[kMapProjected]) {
        if (s->Option(kProjLightMultiply))
            f |= kShadeProjMultiply;
        else if (s->maps[kMapGobo])
            f |= kShadeProjGobo;
    }
    // the shadow buffer, where s5 is the shadow map the capture has the pass
    // of: the backend drops the flag where it hasn't drawn it. Every pixel
    // shader the dumps have that reads it (c107) lights per pixel; a
    // vertex-lit material's is left out.
    if (s->Option(kPerPixel) && o.self_shadow && o.texture_passes && ShadowMapOf(s))
        f |= kShadeShadow;
    if (s->Option(kRimLight)) f |= kShadeRim;
    if (s->Option(kRimLightUnder)) f |= kShadeRimUnder;
    switch (s->OptionBits(kCustomVariation, 2)) {
        case 1: f |= kShadeSkin; break;
        case 2: f |= kShadeHair; break;
        default: break;
    }
}

}  // namespace band3::render::shade
