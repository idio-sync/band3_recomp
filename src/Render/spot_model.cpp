#include "src/Render/spot_model.h"

#include <algorithm>
#include <cmath>

// See spot_model.h.

namespace band3::render::spot {
namespace {

// HLSL's operators and functions, for spot_model.hlsli: only what it uses
float3 operator+(float3 a, float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
float3 operator-(float3 a, float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float3 operator*(float3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float3 operator/(float3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }

float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float saturate(float v) { return std::clamp(v, 0.0f, 1.0f); }
float abs(float v) { return std::fabs(v); }
float sqrt(float v) { return std::sqrt(v); }
// as HLSL's: a NaN on one side gives the other
float min(float a, float b) { return a < b ? a : b; }
float max(float a, float b) { return a > b ? a : b; }
float lerp(float a, float b, float t) { return a + (b - a) * t; }

#define SPOT_IN(T) const T&
#include "src/Render/shaders/spot_model.hlsli"
#undef SPOT_IN

static_assert(kSpotMiss == kSpotRayMiss && kSpotFromEye == kSpotRayFromEye &&
                  kSpotThrough == kSpotRayThrough && kSpotToScene == kSpotRayToScene &&
                  kSpotMirror == kSpotRayMirror,
              "spot_model.h numbers SpotRay's cases as spot_model.hlsli does");

float3 Vec(const float p[3]) { return {p[0], p[1], p[2]}; }

void Copy(const float* from, float4& to) { to = {from[0], from[1], from[2], from[3]}; }

}  // namespace

bool PackSpot(const ShadeInputs& s, uint32_t width, uint32_t height, SpotParams& sp) {
    sp = SpotParams{};
    if (s.shader_type != kDepthVolumeShader || !width || !height) return false;
    Copy(s.Ps(10), sp.eye);
    Copy(s.Ps(25), sp.apex);
    Copy(s.Ps(26), sp.axis);
    Copy(s.Ps(27), sp.eye_apex);
    Copy(s.Ps(28), sp.cone);
    Copy(s.Ps(30), sp.forward);
    Copy(s.Ps(86), sp.xsec);
    Copy(s.Ps(87), sp.plane_a);
    Copy(s.Ps(88), sp.plane_b);
    Copy(s.Ps(89), sp.depth_range);
    Copy(s.Ps(90), sp.color);
    Copy(s.Ps(127), sp.fog);
    sp.target = {float(width), float(height), 1.0f / float(width), 1.0f / float(height)};
    return true;
}

bool SpotBlur(const DrawItem& d, const ShadeInputs* s, const Pass& p) {
    if (d.rect_shader != 1 || !s || p.tex_type != kTexTypeDepthVolume || !d.tex ||
        d.tex->tex_obj != p.tex_obj)
        return false;
    float weights = 0;
    for (int i = 0; i < kSpotBlurTaps; i++) weights += s->Ps(47 + i)[0];
    return weights > 0;
}

SpotRayCpu SpotRayOnCpu(const SpotParams& sp, const float p[3], float scene_depth) {
    const SpotSegment s = SpotRay(sp, Vec(p), scene_depth);
    return {{s.dir.x, s.dir.y, s.dir.z}, s.tn, s.tf, s.kind};
}

float SpotSceneDepthCpu(const SpotParams& sp, float inv_w) { return SpotSceneDepth(sp, inv_w); }

float SpotFalloffCpu(float un, float uf) { return SpotFalloff(un, uf); }

float SpotGoboCoordCpu(const SpotParams& sp, const float p[3]) {
    return SpotGoboCoord(sp, Vec(p));
}

void SpotConeCpu(const SpotParams& sp, const float p[3], float w, float scene_depth,
                 float xsec_texel, float density, float out[3]) {
    const float3 c = SpotCone(sp, Vec(p), w, scene_depth, xsec_texel, density);
    out[0] = c.x;
    out[1] = c.y;
    out[2] = c.z;
}

}  // namespace band3::render::spot
