#pragma once

#include <cstdint>

#include "src/Render/scene_capture.h"
#include "src/Render/shade_model.h"

// Experimental: NgSpotlightDrawer's volumetric spotlights for the native view
// (out/research/spotlight_survey.md, spotlight_design.md). Each frame the game
// draws after post-processing starts:
//   the density map (320x180): its fog proxy's particles, captured as any;
//   the depth volume (640x360, cleared opaque black): each spotlight's cone
//     proxy (scene_capture.h's IsSpotCone), which adds the light along the
//     view ray inside the cone and in front of the scene, ONE ONE into 8 bits
//     (shaders/spot_model.hlsli);
//   two blurs of the depth volume in place, across then down, 5 taps each.
// The composite then adds the blurred depth volume to the picture (not drawn
// yet). PackSpot turns a cone's shade state into the numbers the cone shades
// with (SpotParams); both backends shade from them with the same code,
// spot_model.hlsli, which spot_model.cpp compiles as C++ for the CPU.

namespace band3::render::spot {

using shade::float3;
using shade::float4;
using shade::uint;

#include "src/Render/shaders/spot_params.hlsli"

static_assert(sizeof(SpotParams) == 13 * 16, "SpotParams is float4s only, as HLSL packs it");

// A cone draw's SpotParams from its shade state's spotlight registers, for a
// depth volume width x height; false if the state isn't a cone's
bool PackSpot(const ShadeInputs& state, uint32_t width, uint32_t height, SpotParams& out);

// spot_model.hlsli's SpotRay, on the CPU: the stretch [tn, tf] of the view ray
// through p inside the cone and its case (kSpot*, 0 a miss .. 4 the mirror
// cone only)
struct SpotRayCpu {
    float dir[3];
    float tn, tf;
    uint32_t kind;
};
SpotRayCpu SpotRayOnCpu(const SpotParams& sp, const float p[3], float scene_depth);
inline constexpr uint32_t kSpotRayMiss = 0, kSpotRayFromEye = 1, kSpotRayThrough = 2,
                          kSpotRayToScene = 3, kSpotRayMirror = 4;

// and its other functions
float SpotSceneDepthCpu(const SpotParams& sp, float inv_w);
float SpotFalloffCpu(float un, float uf);
float SpotGoboCoordCpu(const SpotParams& sp, const float p[3]);
void SpotConeCpu(const SpotParams& sp, const float p[3], float w, float scene_depth,
                 float xsec_texel, float density, float out[3]);

}  // namespace band3::render::spot
