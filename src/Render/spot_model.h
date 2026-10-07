#pragma once

#include <cstdint>

#include "src/Render/scene_capture.h"
#include "src/Render/shade_model.h"

// NgSpotlightDrawer's volumetric spotlights (spotlight_survey.md,
// spotlight_design.md). After post-processing starts the game draws:
//   the density map (320x180): its fog proxy's particles;
//   the depth volume (640x360, cleared opaque black): each cone proxy
//     (IsSpotCone), ONE ONE into 8 bits (shaders/spot_model.hlsli);
//   two in-place 5-tap blurs of it, across then down.
// The composite adds the blurred volume to the picture.

namespace band3::render::spot {

using shade::float3;
using shade::float4;
using shade::uint;

#include "src/Render/shaders/spot_params.hlsli"

static_assert(sizeof(SpotParams) == 13 * 16, "SpotParams is float4s only, as HLSL packs it");

// false if the state isn't a cone's
bool PackSpot(const ShadeInputs& state, uint32_t width, uint32_t height, SpotParams& out);

// NgSpotlightDrawer::BlurRT_824D24D0: uv offsets in PS c31.., per-channel
// weights in c47..
inline constexpr int kSpotBlurTaps = 5;

// Whether the draw is an in-place DrawRect blur (shader 1) with its taps kept:
// the depth volume's, or NgLight::BlurShadowRT's. The game reads its last
// resolve; the renderers blur a copy of the target into it.
bool SpotBlur(const DrawItem& d, const ShadeInputs* state, const Pass& p);

// spot_model.hlsli's functions on the CPU
struct SpotRayCpu {
    float dir[3];
    float tn, tf;
    uint32_t kind;
};
SpotRayCpu SpotRayOnCpu(const SpotParams& sp, const float p[3], float scene_depth);
inline constexpr uint32_t kSpotRayMiss = 0, kSpotRayFromEye = 1, kSpotRayThrough = 2,
                          kSpotRayToScene = 3, kSpotRayMirror = 4;

float SpotSceneDepthCpu(const SpotParams& sp, float inv_w);
float SpotFalloffCpu(float un, float uf);
float SpotGoboCoordCpu(const SpotParams& sp, const float p[3]);
void SpotConeCpu(const SpotParams& sp, const float p[3], float w, float scene_depth,
                 float xsec_texel, float density, float out[3]);

}  // namespace band3::render::spot
