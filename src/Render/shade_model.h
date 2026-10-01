#pragma once

#include <cstdint>

#include "src/Render/scene_capture.h"
#include "src/Render/soft_raster.h"

// Experimental: RB3's material lighting for the native view, from a draw's
// ShadeState. PackShade turns what the game's shaders were given into the
// numbers the shading reads (ShadeParams); both backends shade from them with
// the same code, src/Render/shaders/shade.hlsli, which mesh.hlsl compiles for
// the GPU and shade_model.cpp compiles as C++ for the CPU.

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

static_assert(sizeof(ShadeParams) == 31 * 16, "ShadeParams is float4s only, as HLSL packs it");

// What a draw shades with. Without a ShadeState (a capture from before them),
// with options.legacy_light, it's the placeholder from before; options.lighting
// off draws the game's shading unlit. `textured`: the draw has a diffuse
// texture the backend samples. The maps (specular, glow) are flagged when the
// option word samples them and the capture decoded them. A DrawRect quad that
// DxRnd drew with a shader of its own is its texture times its vertex colour;
// a draw into a texture keeps its alpha (no PSEUDO_HDR luminance).
void PackShade(const DrawItem& item, const ShadeState* state, const RasterOptions& options,
               bool textured, ShadeParams& out);

// the draw's shade state, or null
inline const ShadeState* ShadeOf(const FrameCapture& frame, const DrawItem& item) {
    return item.shade >= 0 && size_t(item.shade) < frame.shades.size() ? &frame.shades[item.shade]
                                                                         : nullptr;
}

// shade.hlsli's TexGen, AoSh*, ProjUv, Light and ShadePixel, on the CPU; the
// AoSh ones are per vertex (AoShVertexCpu's two are the ao_sh the pixels take
// interpolated), as is LightVertexCpu, for kShadePerVertex, whose pixels take
// its two colours interpolated. ShadePixelCpu's proj and gobo are the
// projected light's texels at ProjUvCpu (null: 0).
void TexGenUv(const ShadeParams& sp, const float uv[2], float out[2]);
void ProjUvCpu(const ShadeParams& sp, const float p[3], float out[2]);
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
                   const float proj[4] = nullptr, const float gobo[4] = nullptr);
bool AlphaCutCpu(const ShadeParams& sp, float alpha);

}  // namespace band3::render::shade
