#pragma once

#include <cstdint>

#include "src/Render/scene_capture.h"
#include "src/Render/soft_raster.h"

// RB3's material lighting. PackShade turns a draw's ShadeState into
// ShadeParams; both backends shade with src/Render/shaders/shade.hlsli,
// compiled here as C++ for the CPU.

namespace band3::render::shade {

// HLSL's types, as shade_params.hlsli's ShadeParams lays them out
using uint = uint32_t;
struct float2 {
    float x, y;
};
struct float3 {
    float x, y, z;
};
struct float4 {
    float x, y, z, w;
};
struct uint4 {
    uint x, y, z, w;
};

#include "src/Render/shaders/shade_params.hlsli"

static_assert(sizeof(ShadeParams) == 44 * 16, "ShadeParams is float4s only, as HLSL packs it");

// Null state or options.legacy_light gives the placeholder; options.lighting
// off draws unlit. `textured`: the backend samples a diffuse texture. Maps are
// flagged only when sampled and decoded, normal maps only with tangents. A
// DxRnd DrawRect quad is texture times vertex colour, except a movie
// (kShadeYuv); a draw into a texture keeps its alpha (no PSEUDO_HDR).
void PackShade(const DrawItem& item, const ShadeState* state, const RasterOptions& options,
               bool textured, ShadeParams& out);

// For a shadow map drawn at w x h instead of game_w x game_h
// (RasterOptions::shadow_scale): moves ShadowCoord's half-texel offset to half
// the new texel
void RescaleShadowCoord(ShadeParams& sp, uint32_t game_w, uint32_t game_h, uint32_t w,
                        uint32_t h);

inline const ShadeState* ShadeOf(const FrameCapture& frame, const DrawItem& item) {
    return item.shade >= 0 && size_t(item.shade) < frame.shades.size() ? &frame.shades[item.shade]
                                                                         : nullptr;
}

// shade.hlsli's functions on the CPU. ShadePixelCpu's null proj and gobo read
// as 0, null normal_map as none.
void TexGenUv(const ShadeParams& sp, const float uv[2], float out[2]);
// t: the instance's translation for a position, zero for a direction
void BillboardCpu(const ShadeParams& sp, const float v[3], const float t[3], float out[3]);
void TextureFrameCpu(const ShadeParams& sp, const float n[3], const float t[4], float n_out[3],
                     float u_out[3]);
void BitangentCpu(const float n[3], const float u[3], float w, float out[3]);
void DetailUvCpu(const ShadeParams& sp, const float uv[2], float out[2]);
// interpolated tangent and bitangent, and the two maps' texels
struct NormalMapInputs {
    float u[3], b[3];
    float map[4], detail[4];
};
void ProjUvCpu(const ShadeParams& sp, const float p[3], float out[2]);
void RefractUvCpu(const ShadeParams& sp, const float clip[2], float w, const float map[4],
                  float out[2]);
void ShadowCoordCpu(const ShadeParams& sp, const float p[3], float out[4]);
struct ShadowTapsCpu {
    int x[4], y[4];
    float weight[4];
    float depth;
};
ShadowTapsCpu ShadowTapsOf(const float s[4], uint32_t w, uint32_t h);
// depth: a w x h map of clip z/w, row by row
float ShadowLitCpu(const ShadeParams& sp, const float p[3], const float* depth, uint32_t w,
                   uint32_t h);
void AoShDirectionCpu(const float vc[4], float out[3]);
float AoShRatioCpu(const ShadeParams& sp, uint light, const float p[3], const float n[3],
                   const float dir[3], float r);
void AoShVertexCpu(const ShadeParams& sp, const float p[3], const float n[3], const float dir[3],
                   const float vc[4], float out[2]);
void LightVertexCpu(const ShadeParams& sp, const float p[3], const float n[3], const float vc[4],
                    const float ao_sh[2], float diffuse[3], float added[3]);
void ShadePixelCpu(const ShadeParams& sp, const float p[3], const float n[3], const float vc[4],
                   const float texel[4], const float spec_map[4], const float glow[4],
                   const float behind[4], float depth, const float ao_sh[2],
                   const float vertex_diffuse[3], const float vertex_added[3], float out[4],
                   const float proj[4] = nullptr, const float gobo[4] = nullptr,
                   float lit = 1.0f, const NormalMapInputs* normal_map = nullptr);
bool AlphaCutCpu(const ShadeParams& sp, float alpha);
// SoftFade(SoftSceneDepth(far_plane, inv_w), w)
float SoftFadeCpu(float far_plane, float inv_w, float w);

}  // namespace band3::render::shade
