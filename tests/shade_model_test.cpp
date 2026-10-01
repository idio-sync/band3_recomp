// Checks the native view's shading (src/Render/shade_model.cpp, which runs
// src/Render/shaders/shade.hlsli on the CPU, as mesh.hlsl does on the GPU)
// against the M2 research's Python models of the game's own shaders
// (tools/shaders/research: fam3.py, skin2.py and hair3.py, checked there
// against the shaders' microcode), the SH occlusion, REFRACT_WORLD's
// picture behind and the shadow buffer's taps, their weights and its
// darkening against hand-worked numbers, PackShade's reading of the
// option word, the particle quad's corners (scene_capture.h's
// ParticleCorner) against the particle VS's instructions, and a soft
// particle's fade (SoftFade) against the soft particle pixel shader's maths.

#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "src/Render/shade_model.h"

using namespace band3::render;
using namespace band3::render::shade;

namespace {

// one point shaded both ways: the models' inputs and what they give
struct Case {
    const char* name;
    uint32_t flags;
    uint32_t points;
    // c0 c1 c2 c5 c7 c63 c64 c65 c67 c68 c80..c85
    float c[16][4];
    float p[3], eye[3], n[3], vc[4];
    float ao;  // VS c24's strength
    float tex[4], spec_map[4], glow[4];
    float rgb[3];
    float alpha;
    // the projected light's: c66, c69 and its texels (s5, s10); 0 without it
    float c66[4], c69[4], proj[4], gobo[4];
    // the shadow buffer's: c107, c108 and how lit the pixel is (ShadowLit)
    float c107[4], c108[4], lit;
};

// Made from the models (fam3.model, skin2.skin, hair3.hair) by
// tools/shaders/research/gen_shade_cases.py, with no normal or
// environment map. The hair's cases have no
// specular colour, as its strand highlight needs the tangent the capture
// doesn't keep, so only its box highlight is compared.
const Case kCases[] = {
    {"standard: two points, box, specular",
     kShadeLit | kShadeBox | kShadeSpecular | kShadeTextured, 2,
     {{0.307f, 0.878f, 0.811f, 0.404f}, {0.198f, 0.18f, 0.261f, 0.315f}, {0.275f, 0.223f, 0.869f, 23.34f}, {0.0f, 2.0f, 0.0f, 0.0f}, {0.03f, 0.059f, 0.011f, 10.0f}, {0.556f, 0.777f, 0.383f, 3.7f}, {-187.76f, -189.82f, 185.35f, -0.001737f}, {-24.84f, -1.68f, 108.27f, -0.003421f}, {0.948f, 0.668f, 1.018f, 1.222f}, {0.672f, 1.081f, 0.793f, 1.838f}, {0.334f, 0.385f, 0.112f, 0.596f}, {0.516f, 0.073f, 0.2f, 0.433f}, {0.427f, 0.562f, 0.253f, 0.498f}, {0.402f, 0.182f, 0.353f, 0.529f}, {0.508f, 0.303f, 0.353f, 0.021f}, {0.146f, 0.478f, 0.249f, 0.104f}},
     {4.88f, 20.3f, 17.45f}, {-75.18f, -468.31f, 201.69f}, {0.557f, 0.042f, -0.213f}, {0.49f, 0.03f, 0.043f, 0.703f}, 0.0f,
     {0.985f, 0.634f, 0.454f, 0.253f}, {0.552f, 0.984f, 0.793f, 0.586f}, {0.86f, 0.232f, 0.514f, 0.952f},
     {0.191935188f, 0.430698795f, 0.198360442f}, 0.03219678f},
    {"standard: AO, one point, specular map, glow, intensify",
     kShadeLit | kShadeBox | kShadeSpecular | kShadeSpecMap | kShadeAO | kShadeGlow | kShadeIntensify | kShadeTextured, 1,
     {{0.965f, 0.958f, 0.245f, 0.268f}, {0.334f, 0.294f, 0.268f, 0.123f}, {0.685f, 0.685f, 0.665f, 14.06f}, {1.09f, 2.0f, 0.0f, 0.0f}, {0.03f, 0.059f, 0.011f, 10.0f}, {0.778f, 0.996f, 0.96f, 2.33f}, {-92.7f, -185.63f, 56.86f, -0.002591f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.841f, 0.946f, 1.816f, 1.561f}, {0.0f, 0.0f, 0.0f, 1.0f}, {0.478f, 0.441f, 0.544f, 0.458f}, {0.474f, 0.212f, 0.589f, 0.577f}, {0.097f, 0.452f, 0.429f, 0.277f}, {0.318f, 0.294f, 0.555f, 0.301f}, {0.499f, 0.212f, 0.53f, 0.54f}, {0.277f, 0.341f, 0.552f, 0.434f}},
     {-1.34f, -27.82f, -17.53f}, {119.74f, -550.18f, 281.59f}, {-0.464f, 0.823f, -0.381f}, {0.957f, 0.706f, 0.504f, 0.518f}, 1.5f,
     {0.686f, 0.629f, 0.381f, 0.287f}, {0.561f, 0.941f, 0.661f, 0.168f}, {0.82f, 0.726f, 0.908f, 0.191f},
     {1.93092631f, 1.9650916f, 1.31544523f}, 0.018921336f},
    {"standard: prelit, two points",
     kShadeLit | kShadeBox | kShadeSpecular | kShadePrelit | kShadeTextured, 2,
     {{0.39f, 0.635f, 0.496f, 0.683f}, {0.25f, 0.026f, 0.005f, 0.335f}, {0.407f, 0.387f, 0.997f, 25.42f}, {0.0f, 2.0f, 0.0f, 0.0f}, {0.03f, 0.059f, 0.011f, 10.0f}, {0.711f, 0.32f, 0.708f, 2.57f}, {96.5f, 68.56f, 66.01f, -0.001987f}, {87.53f, 151.53f, 228.53f, -0.001759f}, {1.305f, 0.812f, 0.353f, 1.473f}, {0.971f, 1.662f, 1.056f, 1.879f}, {0.058f, 0.082f, 0.13f, 0.579f}, {0.262f, 0.376f, 0.181f, 0.304f}, {0.232f, 0.211f, 0.351f, 0.351f}, {0.543f, 0.409f, 0.557f, 0.514f}, {0.595f, 0.403f, 0.098f, 0.516f}, {0.579f, 0.543f, 0.341f, 0.428f}},
     {-28.89f, 33.16f, 7.35f}, {-129.03f, -580.96f, 270.79f}, {0.98f, -0.823f, 0.601f}, {0.41f, 0.151f, 0.294f, 0.769f}, 0.0f,
     {0.885f, 0.14f, 0.653f, 0.14f}, {0.747f, 0.398f, 0.893f, 0.983f}, {0.505f, 0.999f, 0.31f, 0.077f},
     {1.39337319f, 0.563714399f, 0.921499702f}, 0.0360661f},
    {"standard: rim, AO",
     kShadeLit | kShadeBox | kShadeSpecular | kShadeRim | kShadeAO | kShadeTextured, 2,
     {{0.389f, 0.283f, 0.517f, 0.324f}, {0.027f, 0.161f, 0.367f, 0.32f}, {0.812f, 0.378f, 0.629f, 6.83f}, {0.0f, 2.0f, 0.0f, 0.0f}, {0.03f, 0.059f, 0.011f, 10.0f}, {0.372f, 0.942f, 0.863f, 3.4f}, {-122.63f, -76.06f, 206.74f, -0.002029f}, {2.38f, -128.88f, 168.4f, -0.004242f}, {1.753f, 1.796f, 0.447f, 1.672f}, {1.889f, 1.771f, 1.231f, 1.909f}, {0.343f, 0.529f, 0.509f, 0.305f}, {0.248f, 0.359f, 0.259f, 0.097f}, {0.183f, 0.488f, 0.026f, 0.028f}, {0.376f, 0.168f, 0.321f, 0.283f}, {0.206f, 0.598f, 0.117f, 0.248f}, {0.122f, 0.38f, 0.166f, 0.213f}},
     {24.69f, -17.93f, 5.85f}, {242.59f, -569.71f, 112.32f}, {-0.542f, 0.53f, 0.231f}, {0.237f, 0.331f, 0.178f, 0.459f}, 0.8f,
     {0.139f, 0.728f, 0.906f, 0.959f}, {0.761f, 0.964f, 0.116f, 0.36f}, {0.966f, 0.775f, 0.41f, 0.943f},
     {0.058887963f, 0.222020463f, 0.197083083f}, 0.09942912f},
    {"standard: no box, no specular",
     kShadeLit | kShadeTextured, 1,
     {{0.698f, 0.793f, 0.836f, 0.954f}, {0.296f, 0.369f, 0.012f, 0.186f}, {0.0f, 0.0f, 0.0f, 15.13f}, {0.0f, 2.0f, 0.0f, 0.0f}, {0.03f, 0.059f, 0.011f, 10.0f}, {0.635f, 0.659f, 0.21f, 1.84f}, {166.54f, 106.29f, 89.9f, -0.001927f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.536f, 1.35f, 0.515f, 1.871f}, {0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}},
     {34.49f, -48.19f, 28.77f}, {-80.29f, -426.44f, 101.82f}, {-0.907f, -0.638f, 0.91f}, {0.197f, 0.756f, 0.93f, 0.942f}, 0.0f,
     {0.41f, 0.419f, 0.572f, 0.798f}, {0.197f, 0.774f, 0.818f, 0.874f}, {0.037f, 0.946f, 0.091f, 0.341f},
     {0.08470928f, 0.122606523f, 0.005738304f}, 0.141600312f},
    {"skin: rim, AO, specular map",
     kShadeLit | kShadeBox | kShadeSpecular | kShadeSpecMap | kShadeRim | kShadeAO | kShadeSkin | kShadeTextured, 2,
     {{0.835f, 0.858f, 0.588f, 0.409f}, {0.0f, 0.265f, 0.188f, 0.304f}, {0.499f, 0.816f, 0.418f, 22.44f}, {0.0f, 2.0f, 0.0f, 0.0f}, {0.03f, 0.059f, 0.011f, 10.0f}, {0.631f, 0.746f, 0.354f, 3.42f}, {-93.79f, 121.35f, 221.42f, -0.00186f}, {-162.49f, -121.14f, 208.73f, -0.00316f}, {0.87f, 0.458f, 1.66f, 1.445f}, {1.917f, 1.301f, 0.641f, 1.36f}, {0.56f, 0.546f, 0.309f, 0.387f}, {0.419f, 0.483f, 0.586f, 0.017f}, {0.217f, 0.361f, 0.183f, 0.353f}, {0.054f, 0.528f, 0.315f, 0.07f}, {0.398f, 0.187f, 0.118f, 0.29f}, {0.083f, 0.126f, 0.519f, 0.419f}},
     {-48.75f, 27.81f, -48.29f}, {-100.25f, -328.67f, 224.05f}, {-0.388f, -0.245f, -0.222f}, {0.127f, 1.0f, 0.054f, 0.423f}, 1.0f,
     {0.772f, 0.458f, 0.983f, 0.321f}, {0.981f, 0.903f, 0.703f, 0.901f}, {0.447f, 0.831f, 0.796f, 0.517f},
     {0.0886097877f, 0.0699664457f, 0.102665491f}, 0.039911856f},
    {"skin: plain",
     kShadeLit | kShadeBox | kShadeSpecular | kShadeSkin | kShadeTextured, 1,
     {{0.459f, 0.321f, 0.721f, 0.258f}, {0.214f, 0.146f, 0.023f, 0.203f}, {0.23f, 0.547f, 0.256f, 13.89f}, {0.0f, 2.0f, 0.0f, 0.0f}, {0.03f, 0.059f, 0.011f, 10.0f}, {0.299f, 0.379f, 0.702f, 2.73f}, {-41.33f, 190.5f, 61.65f, -0.00184f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.792f, 0.545f, 0.5f, 1.816f}, {0.0f, 0.0f, 0.0f, 1.0f}, {0.257f, 0.188f, 0.351f, 0.272f}, {0.18f, 0.477f, 0.419f, 0.146f}, {0.345f, 0.315f, 0.525f, 0.438f}, {0.173f, 0.588f, 0.071f, 0.251f}, {0.454f, 0.091f, 0.293f, 0.024f}, {0.401f, 0.459f, 0.344f, 0.525f}},
     {-18.63f, 19.53f, 9.44f}, {47.94f, -463.14f, 267.99f}, {0.889f, -0.052f, 0.328f}, {0.061f, 0.701f, 0.647f, 0.993f}, 0.0f,
     {0.84f, 0.356f, 0.447f, 0.702f}, {0.12f, 0.516f, 0.251f, 0.205f}, {0.059f, 0.768f, 0.129f, 0.248f},
     {0.370110714f, 0.0764397031f, 0.203472324f}, 0.036766548f},
    {"hair: no specular colour (its strands need the tangent)",
     kShadeLit | kShadeBox | kShadeSpecular | kShadeSpecMap | kShadeHair | kShadeTextured, 2,
     {{0.381f, 0.97f, 0.301f, 0.764f}, {0.034f, 0.099f, 0.4f, 0.084f}, {0.0f, 0.0f, 0.0f, 7.38f}, {0.0f, 2.0f, 0.0f, 0.0f}, {0.03f, 0.059f, 0.011f, 10.0f}, {0.272f, 0.387f, 0.216f, 2.22f}, {160.83f, -48.37f, 78.43f, -0.003297f}, {-64.62f, 76.52f, 174.4f, -0.002174f}, {1.986f, 0.407f, 1.354f, 1.661f}, {1.832f, 1.289f, 0.542f, 1.946f}, {0.293f, 0.116f, 0.568f, 0.347f}, {0.437f, 0.529f, 0.171f, 0.214f}, {0.527f, 0.081f, 0.459f, 0.059f}, {0.414f, 0.421f, 0.57f, 0.506f}, {0.302f, 0.119f, 0.09f, 0.317f}, {0.306f, 0.043f, 0.542f, 0.304f}},
     {20.13f, -28.0f, -25.61f}, {-292.85f, -497.03f, 153.37f}, {-0.153f, -0.246f, 0.669f}, {0.891f, 0.175f, 0.396f, 0.166f}, 0.0f,
     {0.697f, 0.977f, 0.282f, 0.79f}, {0.37f, 0.112f, 0.788f, 0.408f}, {0.17f, 0.436f, 0.232f, 0.41f},
     {0.854856211f, 1.56413291f, 0.186218695f}, 0.05069904f},
    {"standard: projected light, multiply, AO, rim",
     kShadeLit | kShadeBox | kShadeSpecular | kShadeRim | kShadeAO | kShadeProjMultiply | kShadeTextured, 1,
     {{0.57f, 0.499f, 0.311f, 0.893f}, {0.003f, 0.201f, 0.359f, 0.032f}, {0.643f, 0.693f, 0.233f, 21.7f}, {0.0f, 2.0f, 0.0f, 0.0f}, {0.03f, 0.059f, 0.011f, 10.0f}, {0.78f, 0.326f, 0.39f, 2.52f}, {169.53f, 36.17f, 243.55f, -0.002829f}, {0.0f, 0.0f, 0.0f, 0.0f}, {1.568f, 0.473f, 0.795f, 1.726f}, {0.0f, 0.0f, 0.0f, 1.0f}, {0.033f, 0.227f, 0.295f, 0.014f}, {0.255f, 0.544f, 0.067f, 0.358f}, {0.073f, 0.347f, 0.537f, 0.122f}, {0.005f, 0.05f, 0.324f, 0.01f}, {0.051f, 0.298f, 0.553f, 0.252f}, {0.239f, 0.383f, 0.056f, 0.348f}},
     {-32.74f, 10.89f, 45.83f}, {-267.5f, -433.48f, 221.28f}, {-0.701f, -0.463f, 0.99f}, {0.998f, 0.121f, 0.705f, 0.951f}, 1.2f,
     {0.313f, 0.65f, 0.139f, 0.429f}, {0.707f, 0.631f, 0.797f, 0.178f}, {0.347f, 0.864f, 0.584f, 0.451f},
     {0.0698156981f, 0.220943757f, 0.0525876545f}, 0.012259104f,
     {-0.602f, 0.088f, 0.794f, 0.0f}, {0.313f, 0.86f, 0.53f, 1.0f}, {0.528f, 0.666f, 0.595f, 0.453f}, {0.63f, 0.103f, 0.784f, 0.025f}},
    {"standard: projected light, multiply, prelit, two points",
     kShadeLit | kShadeBox | kShadeSpecular | kShadePrelit | kShadeProjMultiply | kShadeTextured, 2,
     {{0.657f, 0.543f, 0.662f, 0.365f}, {0.325f, 0.329f, 0.261f, 0.064f}, {0.617f, 0.462f, 0.4f, 29.9f}, {0.0f, 2.0f, 0.0f, 0.0f}, {0.03f, 0.059f, 0.011f, 10.0f}, {0.888f, 0.683f, 0.505f, 3.02f}, {-17.27f, 74.34f, 215.46f, -0.00395f}, {-198.38f, -146.41f, 285.25f, -0.003114f}, {1.605f, 1.97f, 1.948f, 1.044f}, {0.922f, 1.827f, 0.834f, 1.436f}, {0.039f, 0.351f, 0.506f, 0.094f}, {0.135f, 0.248f, 0.022f, 0.298f}, {0.491f, 0.395f, 0.32f, 0.513f}, {0.09f, 0.34f, 0.225f, 0.361f}, {0.068f, 0.465f, 0.058f, 0.1f}, {0.484f, 0.569f, 0.26f, 0.248f}},
     {-25.45f, -22.53f, 11.73f}, {-192.92f, -564.16f, 191.15f}, {-0.67f, 0.297f, 0.642f}, {0.778f, 0.48f, 0.348f, 0.435f}, 0.0f,
     {0.105f, 0.741f, 0.398f, 0.387f}, {0.172f, 0.503f, 0.625f, 0.452f}, {0.87f, 0.673f, 0.241f, 0.525f},
     {0.787364763f, 0.855996175f, 0.406831899f}, 0.01077408f,
     {-0.209f, 0.379f, 0.902f, 0.0f}, {0.344f, 0.643f, 0.623f, 1.0f}, {0.652f, 0.751f, 0.723f, 0.694f}, {0.166f, 0.441f, 0.969f, 0.415f}},
    {"standard: projected light, gobo, AO, two points",
     kShadeLit | kShadeBox | kShadeSpecular | kShadeAO | kShadeProjGobo | kShadeTextured, 2,
     {{0.562f, 0.648f, 0.939f, 0.573f}, {0.203f, 0.235f, 0.074f, 0.205f}, {0.704f, 0.834f, 0.275f, 4.54f}, {0.0f, 2.0f, 0.0f, 0.0f}, {0.03f, 0.059f, 0.011f, 10.0f}, {0.755f, 0.234f, 0.986f, 2.96f}, {46.23f, -137.0f, 53.75f, -0.002431f}, {-23.79f, 136.97f, 179.78f, -0.002192f}, {0.401f, 0.623f, 0.711f, 1.464f}, {1.15f, 1.426f, 1.077f, 1.998f}, {0.597f, 0.504f, 0.425f, 0.189f}, {0.138f, 0.173f, 0.042f, 0.46f}, {0.24f, 0.508f, 0.232f, 0.575f}, {0.508f, 0.0f, 0.126f, 0.546f}, {0.282f, 0.588f, 0.238f, 0.044f}, {0.378f, 0.467f, 0.162f, 0.052f}},
     {-16.74f, 46.41f, 25.8f}, {-229.2f, -526.08f, 120.21f}, {-0.88f, 0.594f, -0.645f}, {0.559f, 0.447f, 0.191f, 0.732f}, 0.9f,
     {0.218f, 0.679f, 0.205f, 0.479f}, {0.292f, 0.343f, 0.974f, 0.823f}, {0.304f, 0.885f, 0.211f, 0.394f},
     {0.1460358f, 0.349260071f, 0.087071521f}, 0.056265735f,
     {-0.317f, 0.533f, -0.784f, 0.0f}, {0.993f, 0.449f, 0.481f, 1.0f}, {0.197f, 0.178f, 0.044f, 0.054f}, {0.583f, 0.243f, 0.601f, 0.372f}},
    {"standard: in shadow, two points, rim, AO",
     kShadeLit | kShadeBox | kShadeSpecular | kShadeRim | kShadeAO | kShadeShadow | kShadeTextured, 2,
     {{0.58f, 0.726f, 0.733f, 0.314f}, {0.004f, 0.15f, 0.11f, 0.324f}, {0.752f, 0.681f, 0.647f, 6.07f}, {0.0f, 2.0f, 0.0f, 0.0f}, {0.03f, 0.059f, 0.011f, 10.0f}, {0.33f, 0.925f, 0.247f, 1.22f}, {74.78f, -65.2f, 151.15f, -0.001862f}, {194.85f, 178.69f, 78.13f, -0.002708f}, {0.332f, 0.403f, 1.856f, 1.091f}, {0.53f, 0.831f, 1.356f, 1.697f}, {0.031f, 0.103f, 0.489f, 0.24f}, {0.251f, 0.358f, 0.286f, 0.231f}, {0.018f, 0.436f, 0.58f, 0.586f}, {0.398f, 0.214f, 0.218f, 0.414f}, {0.404f, 0.068f, 0.141f, 0.218f}, {0.307f, 0.486f, 0.3f, 0.017f}},
     {32.26f, -6.98f, 1.9f}, {-166.7f, -471.83f, 177.65f}, {0.544f, -0.795f, 0.1f}, {0.178f, 0.696f, 0.037f, 0.277f}, 0.8f,
     {0.41f, 0.673f, 0.147f, 0.513f}, {0.288f, 0.354f, 0.529f, 0.963f}, {0.481f, 0.956f, 0.168f, 0.924f},
     {0.0562764826f, 0.222771904f, 0.155415046f}, 0.052190568f,
     {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f},
     {0.938f, 0.305f, 0.488f, 0.0f}, {-0.835f, 0.498f, 0.234f, 1.0f}, 0.0f},
    {"standard: shadow buffer, lit, prelit",
     kShadeLit | kShadeBox | kShadeSpecular | kShadePrelit | kShadeShadow | kShadeTextured, 1,
     {{0.407f, 0.748f, 0.747f, 0.879f}, {0.074f, 0.092f, 0.059f, 0.09f}, {0.787f, 0.304f, 0.625f, 10.25f}, {0.0f, 2.0f, 0.0f, 0.0f}, {0.03f, 0.059f, 0.011f, 10.0f}, {0.87f, 0.687f, 0.212f, 1.44f}, {148.51f, 123.9f, 251.51f, -0.001885f}, {0.0f, 0.0f, 0.0f, 0.0f}, {1.566f, 1.914f, 1.649f, 1.85f}, {0.0f, 0.0f, 0.0f, 1.0f}, {0.479f, 0.591f, 0.415f, 0.333f}, {0.449f, 0.081f, 0.406f, 0.266f}, {0.106f, 0.122f, 0.314f, 0.152f}, {0.276f, 0.363f, 0.237f, 0.078f}, {0.295f, 0.141f, 0.141f, 0.117f}, {0.22f, 0.044f, 0.391f, 0.572f}},
     {-11.03f, 21.89f, 43.3f}, {-274.07f, -557.48f, 261.22f}, {0.676f, 0.278f, -0.68f}, {0.413f, 0.868f, 0.886f, 0.803f}, 0.0f,
     {0.283f, 0.868f, 0.437f, 0.745f}, {0.223f, 0.974f, 0.643f, 0.75f}, {0.212f, 0.617f, 0.581f, 0.591f},
     {0.0707980007f, 0.371282512f, 0.232116921f}, 0.05384115f,
     {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f},
     {0.507f, 0.415f, 0.779f, 0.0f}, {-0.865f, -0.005f, 0.501f, 1.0f}, 1.0f},
    {"skin: in shadow, rim, AO, two points",
     kShadeLit | kShadeBox | kShadeSpecular | kShadeRim | kShadeAO | kShadeSkin | kShadeShadow | kShadeTextured, 2,
     {{0.285f, 0.762f, 0.722f, 0.952f}, {0.108f, 0.102f, 0.294f, 0.263f}, {0.442f, 0.747f, 0.517f, 5.32f}, {0.0f, 2.0f, 0.0f, 0.0f}, {0.03f, 0.059f, 0.011f, 10.0f}, {0.921f, 0.486f, 0.408f, 2.89f}, {-140.12f, 20.53f, 216.0f, -0.00376f}, {191.2f, -38.53f, 297.61f, -0.002663f}, {1.408f, 0.509f, 0.873f, 1.204f}, {1.333f, 1.783f, 1.468f, 1.589f}, {0.38f, 0.109f, 0.057f, 0.525f}, {0.308f, 0.116f, 0.272f, 0.131f}, {0.479f, 0.3f, 0.061f, 0.491f}, {0.054f, 0.168f, 0.017f, 0.444f}, {0.068f, 0.315f, 0.056f, 0.288f}, {0.411f, 0.321f, 0.273f, 0.291f}},
     {-6.3f, 9.54f, -40.51f}, {-15.29f, -529.12f, 273.09f}, {0.878f, -0.867f, -0.713f}, {0.703f, 0.571f, 0.019f, 0.705f}, 1.0f,
     {0.95f, 0.481f, 0.413f, 0.612f}, {0.945f, 0.337f, 0.372f, 0.27f}, {0.628f, 0.469f, 0.256f, 0.388f},
     {0.160197328f, 0.195285027f, 0.133171351f}, 0.153230112f,
     {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f},
     {0.71f, 0.498f, 0.955f, 0.0f}, {-0.705f, 0.315f, 0.635f, 1.0f}, 0.0f},
    {"hair: in shadow, two points",
     kShadeLit | kShadeBox | kShadeSpecular | kShadeSpecMap | kShadeHair | kShadeShadow | kShadeTextured, 2,
     {{0.972f, 0.209f, 0.789f, 0.326f}, {0.395f, 0.007f, 0.352f, 0.273f}, {0.0f, 0.0f, 0.0f, 21.83f}, {0.0f, 2.0f, 0.0f, 0.0f}, {0.03f, 0.059f, 0.011f, 10.0f}, {0.411f, 0.383f, 0.886f, 3.39f}, {-110.64f, 169.94f, 177.84f, -0.003418f}, {27.85f, 172.09f, 211.39f, -0.002762f}, {1.074f, 1.014f, 0.434f, 1.36f}, {1.768f, 1.992f, 1.133f, 1.019f}, {0.084f, 0.103f, 0.563f, 0.414f}, {0.431f, 0.172f, 0.297f, 0.532f}, {0.06f, 0.07f, 0.028f, 0.281f}, {0.504f, 0.333f, 0.241f, 0.301f}, {0.108f, 0.569f, 0.212f, 0.141f}, {0.187f, 0.495f, 0.191f, 0.543f}},
     {-16.38f, -5.85f, -42.58f}, {244.48f, -361.15f, 194.89f}, {-0.769f, -0.015f, -0.248f}, {0.84f, 0.928f, 0.741f, 0.287f}, 0.0f,
     {0.137f, 0.905f, 0.95f, 0.243f}, {0.55f, 0.533f, 0.979f, 0.139f}, {0.062f, 0.844f, 0.57f, 0.462f},
     {0.138381038f, 0.0892702304f, 0.625476427f}, 0.021626514f,
     {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 0.0f},
     {0.479f, 0.359f, 0.306f, 0.0f}, {0.912f, 0.105f, 0.396f, 1.0f}, 0.0f},
};

void Set(float4& to, const float* from) { to = {from[0], from[1], from[2], from[3]}; }

ShadeParams ParamsFor(const Case& c) {
    ShadeParams sp{};
    sp.flags.x = kShadeModel | c.flags;
    sp.flags.y = c.points;
    Set(sp.color, c.c[0]);
    Set(sp.ambient, c.c[1]);
    Set(sp.specular, c.c[2]);
    Set(sp.emissive, c.c[3]);
    Set(sp.bloom, c.c[4]);
    Set(sp.rim, c.c[5]);
    for (int i = 0; i < 2; i++) {
        Set(sp.point_pos[i], c.c[6 + i]);
        Set(sp.point_color[i], c.c[8 + i]);
    }
    for (int i = 0; i < 6; i++) Set(sp.box[i], c.c[10 + i]);
    sp.eye = {c.eye[0], c.eye[1], c.eye[2], 1};
    sp.ao.x = c.ao;
    Set(sp.proj_dir, c.c66);
    Set(sp.proj_color, c.c69);
    Set(sp.shadow_color, c.c107);
    Set(sp.shadow_dir, c.c108);
    return sp;
}

bool Close(float got, float want) { return std::fabs(got - want) <= 2e-4f * std::max(1.0f, std::fabs(want)); }

void CheckColour(const Case& c, const float out[4]) {
    INFO("case: " << c.name);
    for (int k = 0; k < 3; k++) {
        INFO("channel " << k << ": " << out[k] << ", the model " << c.rgb[k]);
        CHECK(Close(out[k], c.rgb[k]));
    }
    INFO("alpha " << out[3] << ", the shaders' " << c.alpha);
    CHECK(Close(out[3], c.alpha));
}

ShadeState MakeState(uint64_t options, int32_t type = 18) {
    ShadeState s{};
    s.options = options;
    s.shader_type = type;
    s.use_environ = 1;
    for (int i = 0; i < kNumShadeRegs; i++)
        for (int k = 0; k < 4; k++) s.vs[i][k] = s.ps[i][k] = 0.5f;  // stale values
    return s;
}

uint64_t Bit(int b) { return uint64_t(1) << b; }

bool Has(const ShadeParams& sp, uint32_t bits) { return (sp.flags.x & bits) != 0; }

}  // namespace

TEST_CASE("shading matches the game's shader models, per pixel and per vertex") {
    const float zero[3] = {0, 0, 0}, one[4] = {1, 1, 1, 1};
    const float no_sh[2] = {1, 1};
    for (const Case& c : kCases) {
        const ShadeParams sp = ParamsFor(c);
        float out[4];
        ShadePixelCpu(sp, c.p, c.n, c.vc, c.tex, c.spec_map, c.glow, one, 100.0f, no_sh, zero,
                      zero, out, c.proj, c.gobo, c.lit);
        CheckColour(c, out);

        // a vertex-lit material's vertex gives its pixel the same colour, at
        // the vertex; its specular map, the projected light and the shadow
        // buffer are per pixel only
        if (c.flags & (kShadeSpecMap | kShadeProjMultiply | kShadeProjGobo | kShadeShadow))
            continue;
        ShadeParams pv = sp;
        pv.flags.x |= kShadePerVertex;
        float diffuse[3], added[3];
        LightVertexCpu(pv, c.p, c.n, c.vc, no_sh, diffuse, added);
        ShadePixelCpu(pv, c.p, c.n, c.vc, c.tex, c.spec_map, c.glow, one, 100.0f, no_sh, diffuse,
                      added, out);
        CheckColour(c, out);
    }
}

TEST_CASE("unlit materials are colour, ambient, vertex colour if prelit, and texture") {
    ShadeParams sp{};
    sp.flags.x = kShadeModel | kShadePrelit | kShadeTextured;
    sp.color = {0.5f, 1.0f, 0.25f, 0.5f};
    sp.ambient = {1.0f, 0.5f, 1.0f, 1.0f};
    const float p[3] = {0, 0, 0}, n[3] = {0, 0, 1}, vc[4] = {1.0f, 1.0f, 0.5f, 0.5f};
    const float tex[4] = {0.5f, 0.5f, 1.0f, 1.0f}, one[4] = {1, 1, 1, 1}, zero[4] = {0, 0, 0, 0};
    float out[4];
    ShadePixelCpu(sp, p, n, vc, tex, one, zero, one, 1.0f, one, zero, zero, out);
    CHECK(out[0] == doctest::Approx(0.25f));
    CHECK(out[1] == doctest::Approx(0.25f));
    CHECK(out[2] == doctest::Approx(0.125f));
    CHECK(out[3] == doctest::Approx(0.25f));

    // fading to c104's colour beyond the end of the fade (c55)
    sp.flags.x |= kShadeFadeColor;
    sp.fade[0] = sp.fade[1] = {0, 0, 0, 1};
    sp.fade[2] = {100.0f, 0.1f, 1.0f, 0.0f};
    sp.fade_color = {1, 0, 0, 0};
    ShadePixelCpu(sp, p, n, vc, tex, one, zero, one, 95.0f, one, zero, zero, out);  // half way
    CHECK(out[0] == doctest::Approx(0.625f));
    CHECK(out[1] == doctest::Approx(0.125f));
    CHECK(out[3] == doctest::Approx(0.25f));

    // REFRACT_WORLD: the texture's rgb times the picture behind it, alpha the
    // texture's; the picture isn't read without the flag
    sp.flags.x = kShadeModel | kShadePrelit | kShadeTextured | kShadeRefract;
    const float behind[4] = {0.5f, 1.0f, 0.0f, 0.25f};
    ShadePixelCpu(sp, p, n, vc, tex, one, zero, behind, 1.0f, one, zero, zero, out);
    CHECK(out[0] == doctest::Approx(0.125f));
    CHECK(out[1] == doctest::Approx(0.25f));
    CHECK(out[2] == doctest::Approx(0.0f));
    CHECK(out[3] == doctest::Approx(0.25f));
    sp.flags.x &= ~kShadeRefract;
    ShadePixelCpu(sp, p, n, vc, tex, one, zero, behind, 1.0f, one, zero, zero, out);
    CHECK(out[0] == doctest::Approx(0.25f));
}

TEST_CASE("SH occlusion: light 0's visibility over a bare surface's, from the vertex colour") {
    ShadeParams sp{};
    sp.flags.x = kShadeModel | kShadeLit | kShadeAO | kShadeAoSh;
    sp.flags.y = 1;
    sp.point_pos[0] = {0, 0, 10, 0};
    sp.ao.x = 1.0f;
    const float p[3] = {0, 0, 0}, up[3] = {0, 0, 1}, down[3] = {0, 0, -1};

    // the linear terms are alpha, green and blue, from 0..1 to -1..1
    const float vc[4] = {0.2f, 0.75f, 0.25f, 1.0f};
    float dir[3];
    AoShDirectionCpu(vc, dir);
    CHECK(dir[0] == doctest::Approx(1.0f));
    CHECK(dir[1] == doctest::Approx(0.5f));
    CHECK(dir[2] == doctest::Approx(-0.5f));

    // L = +z: vis = 0.282095 0.6 + 0.488603 0.5 = 0.4135585, bare =
    // 2.356194 (0.079577 + 0.238732) = 0.7499978
    const float half_up[3] = {0, 0, 0.5f};
    CHECK(AoShRatioCpu(sp, 0, p, up, half_up, 0.6f) == doctest::Approx(0.551413f).epsilon(1e-5));
    const float vc_up[4] = {0.6f, 0.5f, 0.75f, 0.5f};  // the same as a vertex colour
    AoShDirectionCpu(vc_up, dir);
    float ao[2];
    AoShVertexCpu(sp, p, up, dir, vc_up, ao);
    CHECK(ao[0] == doctest::Approx(0.551413f).epsilon(1e-5));
    CHECK(ao[1] == 1.0f);  // one light
    sp.ao.x = 2.0f;  // 1 + 2 (ratio - 1)
    AoShVertexCpu(sp, p, up, dir, vc_up, ao);
    CHECK(ao[0] == doctest::Approx(0.102826f).epsilon(1e-4));
    sp.ao.x = 1.0f;

    // a second light has its own, toward it: below, facing away
    sp.flags.y = 2;
    sp.point_pos[1] = {0, 0, -10, 0};
    AoShVertexCpu(sp, p, up, dir, vc_up, ao);
    CHECK(ao[0] == doctest::Approx(0.551413f).epsilon(1e-5));
    CHECK(ao[1] == 1.0f);  // bare < 0 toward it
    sp.flags.y = 1;

    // facing away, bare is 2.356194 (0.079577 - 0.238732) < 0: no occlusion
    CHECK(AoShRatioCpu(sp, 0, p, down, half_up, 0.6f) == 1.0f);

    // vertex colour (1, 1, 1, 1), its terms (1, 1, 1) turned away from the
    // light, which the normal faces (a light at the camera): vis = 0.282095 -
    // 0.488603 sqrt(3) < 0, so the ratio is below 0 and light 0 is gone
    const float s3 = 1.0f / std::sqrt(3.0f);
    sp.point_pos[0] = {-10 * s3, -10 * s3, -10 * s3, 0};
    const float toward[3] = {-s3, -s3, -s3}, ones[3] = {1, 1, 1};
    const float white[4] = {1, 1, 1, 1};
    CHECK(AoShRatioCpu(sp, 0, p, toward, ones, 1.0f) ==
          doctest::Approx(-0.752256f).epsilon(1e-4));
    AoShVertexCpu(sp, p, toward, ones, white, ao);
    CHECK(ao[0] == 0.0f);
    sp.ao.x = 2.0f;
    AoShVertexCpu(sp, p, toward, ones, white, ao);
    CHECK(ao[0] == 0.0f);

    // without the flag it's 1
    sp.flags.x &= ~kShadeAoSh;
    AoShVertexCpu(sp, p, toward, ones, white, ao);
    CHECK(ao[0] == 1.0f);
}

TEST_CASE("SH occlusion dims each point light by its own; without it light 1 takes aoA") {
    // two white lights straight above, unattenuated, a white material with no
    // ambient: each light adds its occlusion
    ShadeParams sp{};
    sp.flags.x = kShadeModel | kShadeLit | kShadeAO | kShadeAoSh | kShadeTextured;
    sp.flags.y = 1;
    sp.color = {1, 1, 1, 1};
    sp.ambient = {0, 0, 0, 1};
    sp.ao.x = 1.0f;
    for (int i = 0; i < 2; i++) {
        sp.point_pos[i] = {0, 0, 10, 0};
        sp.point_color[i] = {1, 1, 1, 1};
    }
    sp.eye = {0, -10, 10, 1};
    const float p[3] = {0, 0, 0}, n[3] = {0, 0, 1}, vc[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    const float one[4] = {1, 1, 1, 1}, zero[4] = {0, 0, 0, 0};
    const float ao_sh[2] = {0.25f, 0.5f};
    float out[4];
    ShadePixelCpu(sp, p, n, vc, one, one, zero, one, 1.0f, ao_sh, zero, zero, out);
    CHECK(out[0] == doctest::Approx(0.25f));
    // the second light by its own
    sp.flags.y = 2;
    ShadePixelCpu(sp, p, n, vc, one, one, zero, one, 1.0f, ao_sh, zero, zero, out);
    CHECK(out[0] == doctest::Approx(0.75f));
    // a vertex-lit material's vertex takes them too
    sp.flags.x |= kShadePerVertex;
    float diffuse[3], added[3];
    LightVertexCpu(sp, p, n, vc, ao_sh, diffuse, added);
    CHECK(diffuse[0] == doctest::Approx(0.75f));
    // without the flag, the plain: light 0 aoD, 1 + (1.504505 0.5 - 1), and
    // light 1 aoA, 1 + (1.128379 0.5 - 1)
    sp.flags.x &= ~(kShadeAoSh | kShadePerVertex);
    ShadePixelCpu(sp, p, n, vc, one, one, zero, one, 1.0f, ao_sh, zero, zero, out);
    CHECK(out[0] == doctest::Approx(0.7522525f + 0.5641895f));
}

TEST_CASE("the projected light: where its maps are read, and its two forms") {
    ShadeParams sp{};
    // uv = (c95 P, c96 P) / c97 P: P = (1, 2, 3) gives (2, 5) / 4
    sp.proj[0] = {1, 0, 0, 1};
    sp.proj[1] = {0, 1, 1, 0};
    sp.proj[2] = {0, 0, 1, 1};
    const float at[3] = {1, 2, 3};
    float uv[2];
    ProjUvCpu(sp, at, uv);
    CHECK(uv[0] == doctest::Approx(0.5f));
    CHECK(uv[1] == doctest::Approx(1.25f));

    // a white light straight above, unattenuated, a white material with
    // ambient 0.25, the projected light's colour 0.8 from above too
    sp.flags.x = kShadeModel | kShadeLit | kShadeProjMultiply | kShadeTextured;
    sp.flags.y = 1;
    sp.color = {1, 1, 1, 1};
    sp.ambient = {0.25f, 0.25f, 0.25f, 1};
    sp.point_pos[0] = {0, 0, 10, 0};
    sp.point_color[0] = {1, 1, 1, 1};
    sp.eye = {0, -10, 10, 1};
    sp.proj_dir = {0, 0, 1, 0};
    sp.proj_color = {0.8f, 0.8f, 0.8f, 1};
    const float p[3] = {0, 0, 0}, n[3] = {0, 0, 1}, vc[4] = {1, 1, 1, 1};
    const float one[4] = {1, 1, 1, 1}, zero[4] = {0, 0, 0, 0}, no_sh[2] = {1, 1};
    const float proj[4] = {0, 0, 0, 0.5f}, gobo[4] = {0.5f, 0.25f, 1, 1};
    float out[4];
    // multiply: the light times 1 - 0.75 0.8 0.5 = 0.7, the ambient as it was
    ShadePixelCpu(sp, p, n, vc, one, one, zero, one, 1.0f, no_sh, zero, zero, out, proj, gobo);
    CHECK(out[0] == doctest::Approx(0.25f + 0.7f));
    // a surface it doesn't face isn't darkened
    sp.proj_dir = {0, 0, -1, 0};
    ShadePixelCpu(sp, p, n, vc, one, one, zero, one, 1.0f, no_sh, zero, zero, out, proj, gobo);
    CHECK(out[0] == doctest::Approx(1.25f));
    // the gobo adds 0.8 s10 (1 - 0.5) where it faces, under the light's
    sp.proj_dir = {0, 0, 1, 0};
    sp.flags.x = kShadeModel | kShadeLit | kShadeProjGobo | kShadeTextured;
    ShadePixelCpu(sp, p, n, vc, one, one, zero, one, 1.0f, no_sh, zero, zero, out, proj, gobo);
    CHECK(out[0] == doctest::Approx(1.25f + 0.2f));
    CHECK(out[1] == doctest::Approx(1.25f + 0.1f));
    CHECK(out[2] == doctest::Approx(1.25f + 0.4f));
    // and nothing without the flags, whatever the texels
    sp.flags.x = kShadeModel | kShadeLit | kShadeTextured;
    ShadePixelCpu(sp, p, n, vc, one, one, zero, one, 1.0f, no_sh, zero, zero, out, proj, gobo);
    CHECK(out[0] == doctest::Approx(1.25f));
}

TEST_CASE("the shadow buffer's taps: four texels around the coordinate, bilinear, clamped") {
    // S = (u w, v w, z w, w): the coordinate is divided by its w
    const float w = 2.0f;
    // u 0.3 in 8 texels: x = 2.4 - 0.5 = 1.9, taps 1 and 2, fx 0.9; v 0.5 in 4:
    // y = 1.5, taps 1 and 2, fy 0.5
    const float s[4] = {0.3f * w, 0.5f * w, 0.6f * w, w};
    ShadowTapsCpu t = ShadowTapsOf(s, 8, 4);
    CHECK(t.x[0] == 1);
    CHECK(t.x[1] == 2);
    CHECK(t.x[2] == 1);
    CHECK(t.x[3] == 2);
    CHECK(t.y[0] == 1);
    CHECK(t.y[1] == 1);
    CHECK(t.y[2] == 2);
    CHECK(t.y[3] == 2);
    CHECK(t.weight[0] == doctest::Approx(0.1f * 0.5f));
    CHECK(t.weight[1] == doctest::Approx(0.9f * 0.5f));
    CHECK(t.weight[2] == doctest::Approx(0.1f * 0.5f));
    CHECK(t.weight[3] == doctest::Approx(0.9f * 0.5f));
    CHECK(t.depth == doctest::Approx(0.6f));

    // at the map's edges the taps outside it are the edge's, their weights
    // as they were: x -0.3 (u 0.025) takes 0 twice; y 3.7 (v 1.05) 3 twice
    const float edge[4] = {0.025f, 1.05f, 0.5f, 1.0f};
    t = ShadowTapsOf(edge, 8, 4);
    CHECK(t.x[0] == 0);
    CHECK(t.x[1] == 0);
    CHECK(t.y[0] == 3);
    CHECK(t.y[2] == 3);
    CHECK(t.weight[0] == doctest::Approx(0.3f * 0.3f));
    CHECK(t.weight[1] == doctest::Approx(0.7f * 0.3f));
    CHECK(t.weight[3] == doctest::Approx(0.7f * 0.7f));
    // far outside, every tap is a corner's
    const float out[4] = {-5.0f, 9.0f, 0.5f, 1.0f};
    t = ShadowTapsOf(out, 8, 4);
    for (int k = 0; k < 4; k++) {
        CHECK(t.x[k] == 0);
        CHECK(t.y[k] == 3);
    }

    // how lit: a map whose left half holds the caster at 0.4, the right
    // nothing (1); a pixel at depth 0.6 whose taps straddle the edge
    // (texels 3 and 4 of 8, fx 0.25) is lit by the right taps' weight
    std::vector<float> depth(8 * 4, 1.0f);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) depth[size_t(y) * 8 + x] = 0.4f;
    ShadeParams sp{};
    // S = (x, y, z, 1): world x straight to u, y to v, z to depth
    sp.shadow[0] = {1, 0, 0, 0};
    sp.shadow[1] = {0, 1, 0, 0};
    sp.shadow[2] = {0, 0, 1, 0};
    sp.shadow[3] = {0, 0, 0, 1};
    const float p[3] = {(3.25f + 0.5f) / 8.0f, 0.5f, 0.6f};
    CHECK(ShadowLitCpu(sp, p, depth.data(), 8, 4) == doctest::Approx(0.25f));
    // nearer than the caster: lit whatever the taps
    const float near_p[3] = {p[0], p[1], 0.3f};
    CHECK(ShadowLitCpu(sp, near_p, depth.data(), 8, 4) == 1.0f);
    // at the caster's depth: lit too (the game's sge)
    const float at_p[3] = {p[0], p[1], 0.4f};
    CHECK(ShadowLitCpu(sp, at_p, depth.data(), 8, 4) == 1.0f);
    // wholly over it, behind: none
    const float in_p[3] = {1.5f / 8.0f, 0.5f, 0.6f};
    CHECK(ShadowLitCpu(sp, in_p, depth.data(), 8, 4) == 0.0f);
}

TEST_CASE("the shadow buffer darkens the point lights by 0.75 c107 as the surface faces away") {
    // a white light straight above, unattenuated, a white material with
    // ambient 0.25; the light camera looking down (c108 -z), c107 (1, 0.5, 0)
    ShadeParams sp{};
    sp.flags.x = kShadeModel | kShadeLit | kShadeShadow | kShadeSpecular | kShadeTextured;
    sp.flags.y = 1;
    sp.color = {1, 1, 1, 1};
    sp.ambient = {0.25f, 0.25f, 0.25f, 1};
    sp.specular = {0, 0, 0, 8};
    sp.point_pos[0] = {0, 0, 10, 0};
    sp.point_color[0] = {1, 1, 1, 1};
    sp.eye = {0, -10, 10, 1};
    sp.shadow_color = {1, 0.5f, 0, 0};
    sp.shadow_dir = {0, 0, -1, 1};
    const float p[3] = {0, 0, 0}, n[3] = {0, 0, 1}, vc[4] = {1, 1, 1, 1};
    const float one[4] = {1, 1, 1, 1}, zero[4] = {0, 0, 0, 0}, no_sh[2] = {1, 1};
    float out[4];
    // in shadow: the light times 1 - 0.75 c107, the ambient as it was
    ShadePixelCpu(sp, p, n, vc, one, one, zero, one, 1.0f, no_sh, zero, zero, out, zero, zero,
                  0.0f);
    CHECK(out[0] == doctest::Approx(0.25f + 0.25f));
    CHECK(out[1] == doctest::Approx(0.25f + 0.625f));
    CHECK(out[2] == doctest::Approx(0.25f + 1.0f));
    // half lit: half of that
    ShadePixelCpu(sp, p, n, vc, one, one, zero, one, 1.0f, no_sh, zero, zero, out, zero, zero,
                  0.5f);
    CHECK(out[0] == doctest::Approx(0.25f + 0.625f));
    // lit, or without the flag: as without a shadow
    ShadePixelCpu(sp, p, n, vc, one, one, zero, one, 1.0f, no_sh, zero, zero, out, zero, zero,
                  1.0f);
    CHECK(out[0] == doctest::Approx(1.25f));
    sp.flags.x &= ~kShadeShadow;
    ShadePixelCpu(sp, p, n, vc, one, one, zero, one, 1.0f, no_sh, zero, zero, out, zero, zero,
                  0.0f);
    CHECK(out[0] == doctest::Approx(1.25f));
    // a surface facing along the light camera's forward isn't darkened
    sp.flags.x |= kShadeShadow;
    sp.shadow_dir = {0, 0, 1, 1};
    ShadePixelCpu(sp, p, n, vc, one, one, zero, one, 1.0f, no_sh, zero, zero, out, zero, zero,
                  0.0f);
    CHECK(out[0] == doctest::Approx(1.25f));
    // the coordinate: VS c40..c43 dotted with the position
    sp.shadow[0] = {1, 2, 3, 4};
    sp.shadow[3] = {0, 0, 0, 2};
    const float at[3] = {1, 1, 1};
    float s[4];
    ShadowCoordCpu(sp, at, s);
    CHECK(s[0] == 10.0f);
    CHECK(s[3] == 2.0f);
}

TEST_CASE("PackShade takes each term from the option word, not stale registers") {
    using namespace shader_opt;
    DrawItem it{};
    RasterOptions o;
    ShadeParams sp;

    // one point light: the second slot's registers are stale, and no box map
    // or specular without their bits
    ShadeState s = MakeState(Bit(kRealLights) | Bit(kPerPixel) | (uint64_t(1) << kNumPoint));
    PackShade(it, &s, o, true, sp);
    CHECK(Has(sp, kShadeModel));
    CHECK(Has(sp, kShadeLit));
    CHECK(Has(sp, kShadeTextured));
    CHECK(sp.flags.y == 1);
    CHECK_FALSE(Has(sp, (kShadeBox | kShadeSpecular | kShadeAO | kShadeAoSh | kShadePerVertex |
                         kShadeRefract)));

    // AO with a point light is the SH kind; without one, the plain
    s.options |= Bit(kEnableAO);
    PackShade(it, &s, o, true, sp);
    CHECK(Has(sp, kShadeAO));
    CHECK(Has(sp, kShadeAoSh));
    s = MakeState(Bit(kApproxLights) | Bit(kEnableAO));
    PackShade(it, &s, o, true, sp);
    CHECK(Has(sp, kShadeAO));
    CHECK_FALSE(Has(sp, kShadeAoSh));

    // REFRACT_WORLD, option bit 46, on an unlit material (the score box's)
    s = MakeState(Bit(kDiffuseMap) | Bit(46));
    PackShade(it, &s, o, true, sp);
    CHECK(Has(sp, kShadeRefract));
    CHECK_FALSE(Has(sp, kShadeLit));

    // vertex-lit without PER_PIXEL; maps only where the capture decoded them
    s = MakeState(Bit(kApproxLights) | Bit(kSpecular) | Bit(kSpecularMap) | Bit(kGlowMap));
    PackShade(it, &s, o, false, sp);
    CHECK(Has(sp, kShadePerVertex));
    CHECK(Has(sp, kShadeBox));
    CHECK(sp.flags.y == 0);
    CHECK_FALSE(Has(sp, (kShadeSpecMap | kShadeGlow)));
    auto map = std::make_shared<Texture>();
    s.maps[kMapSpecular] = map;
    s.maps[kMapGlow] = map;
    PackShade(it, &s, o, false, sp);
    CHECK(Has(sp, kShadeSpecMap));
    CHECK(Has(sp, kShadeGlow));

    // the projected light, per pixel, where its map was decoded: the
    // multiply form with PROJ_MULTIPLY, else the gobo's, which needs s10 too
    s = MakeState(Bit(kRealLights) | Bit(kPerPixel) | (uint64_t(1) << kNumProj) |
                  Bit(kProjLightMultiply));
    s.ps[ShadeRegIndex(95)][3] = 7.0f;
    s.ps[ShadeRegIndex(69)][0] = 0.75f;
    PackShade(it, &s, o, true, sp);
    CHECK_FALSE(Has(sp, (kShadeProjMultiply | kShadeProjGobo)));
    s.maps[kMapProjected] = map;
    PackShade(it, &s, o, true, sp);
    CHECK(Has(sp, kShadeProjMultiply));
    CHECK_FALSE(Has(sp, kShadeProjGobo));
    CHECK(sp.proj[0].w == 7.0f);
    CHECK(sp.proj_color.x == 0.75f);
    s.options &= ~Bit(kProjLightMultiply);
    PackShade(it, &s, o, true, sp);
    CHECK_FALSE(Has(sp, (kShadeProjMultiply | kShadeProjGobo)));
    s.maps[kMapGobo] = map;
    PackShade(it, &s, o, true, sp);
    CHECK(Has(sp, kShadeProjGobo));
    CHECK_FALSE(Has(sp, kShadeProjMultiply));
    s.options &= ~Bit(kPerPixel);  // vertex-lit: left out
    PackShade(it, &s, o, true, sp);
    CHECK_FALSE(Has(sp, (kShadeProjMultiply | kShadeProjGobo)));

    // the shadow buffer, per pixel, where s5 is a shadow map the capture kept
    // as a render target: its registers, VS c40..c43, PS c107 and c108
    s = MakeState(Bit(kRealLights) | Bit(kPerPixel) | (uint64_t(1) << kNumPoint) |
                  Bit(kShadowBuffer));
    s.vs[ShadeRegIndex(41)][3] = 0.25f;
    s.ps[ShadeRegIndex(107)][1] = 0.75f;
    s.ps[ShadeRegIndex(108)][2] = -1.0f;
    PackShade(it, &s, o, true, sp);
    CHECK_FALSE(Has(sp, kShadeShadow));  // no map
    s.maps[kMapProjected] = map;         // a loaded texture: not one
    PackShade(it, &s, o, true, sp);
    CHECK_FALSE(Has(sp, kShadeShadow));
    auto shadow_map = std::make_shared<Texture>();
    shadow_map->tex_obj = 0x2251A0C0;
    shadow_map->tex_type = kTexTypeShadowMap;
    shadow_map->version = 3;
    s.maps[kMapProjected] = shadow_map;
    PackShade(it, &s, o, true, sp);
    CHECK(Has(sp, kShadeShadow));
    CHECK_FALSE(Has(sp, (kShadeProjMultiply | kShadeProjGobo)));
    CHECK(sp.shadow[1].w == 0.25f);
    CHECK(sp.shadow_color.y == 0.75f);
    CHECK(sp.shadow_dir.z == -1.0f);
    RasterOptions no_shadow;
    no_shadow.self_shadow = false;
    PackShade(it, &s, no_shadow, true, sp);
    CHECK_FALSE(Has(sp, kShadeShadow));
    s.options &= ~Bit(kPerPixel);  // vertex-lit: left out
    PackShade(it, &s, o, true, sp);
    CHECK_FALSE(Has(sp, kShadeShadow));

    // no light bits: unlit, with the ambient it was given
    s = MakeState(Bit(kDiffuseMap));
    PackShade(it, &s, o, true, sp);
    CHECK_FALSE(Has(sp, kShadeLit));
    CHECK(sp.ambient.x == 0.5f);

    // particles: vertex colour times the VS's c1 and c0, not the PS's
    s = MakeState(Bit(kPrelit), 14);
    s.vs[ShadeRegIndex(0)][0] = 0.25f;
    s.vs[ShadeRegIndex(1)][1] = 1.5f;
    PackShade(it, &s, o, true, sp);
    CHECK_FALSE(Has(sp, kShadeLit));
    CHECK(Has(sp, kShadePrelit));
    CHECK(sp.color.x == 0.25f);
    CHECK(sp.ambient.x == 0.5f);
    CHECK(sp.ambient.y == 1.5f);

    // lighting off: lit materials unlit, ambient 1
    s = MakeState(Bit(kRealLights) | Bit(kApproxLights));
    o.lighting = false;
    PackShade(it, &s, o, true, sp);
    CHECK_FALSE(Has(sp, kShadeLit));
    CHECK(sp.ambient.x == 1.0f);

    // the placeholder: without a shade state, or asked for
    o = RasterOptions{};
    it.prelit = false;
    PackShade(it, nullptr, o, true, sp);
    CHECK_FALSE(Has(sp, kShadeModel));
    CHECK(Has(sp, kShadeLegacyLight));
    o.legacy_light = true;
    PackShade(it, &s, o, true, sp);
    CHECK_FALSE(Has(sp, kShadeModel));
}

namespace {

// The particle VS (2E5F05321D973646.ucode.vert) instrs 61-72 as written, with
// its permuted registers: r4 = (Pz, Px, Py), r3 = (-, swing, angle, size),
// r0.y = 2u - 1 and r0.x = 2v - 1 (instrs 22-28), c47 and c48 read .zxy
void ParticleCornerUcode(const float p[3], const float c47[3], const float c48[3], float size,
                         float angle, float swing, float u, float v, float out[3]) {
    const float r4[3] = {p[2], p[0], p[1]};
    float r0x = 2 * v - 1, r0y = 2 * u - 1;
    // 61-63: the angle wrapped into [-pi, pi)
    float a = angle * 0.15915493667125702f + 0.5f;
    a = (a - std::floor(a)) * 6.2831854820251465f - 3.1415927410125732f;
    // 64-66
    float r2z = std::cos(a), r2w = std::sin(a);
    const float x = r0y * size, y = r0x * size;
    float r0[4] = {x * r2z, y * r2w, y * r2z, x * r2w};
    // 67-68
    r2z *= swing;
    r2w *= swing;
    r0[3] = r0[3] + r0[2];
    const float r3x = r2z + r2z, r3y = r2w + r2w;
    r2z = r0[0] - r0[1];
    // 69-72
    const float c47p[3] = {c47[2], c47[0], c47[1]}, c48p[3] = {c48[2], c48[0], c48[1]};
    float t[3];
    for (int i = 0; i < 3; i++) t[i] = r3y * c47p[i] + r4[i];
    for (int i = 0; i < 3; i++) t[i] = r3x * c48p[i] + t[i];
    for (int i = 0; i < 3; i++) t[i] = r2z * c47p[i] + t[i];
    const float ty[3] = {t[1], t[2], t[0]};
    for (int i = 0; i < 3; i++) out[i] = -r0[3] * c48[i] + ty[i];
}

}  // namespace

TEST_CASE("particle quads are built as the particle VS builds them") {
    // no angle or swing: a quad of 2 size |R| by 2 size |U|, v = 0 at +U
    const float p[3] = {1, 2, 3}, r[3] = {0.5f, 0, 0}, u[3] = {0, 0, 0.25f};
    const float want[4][3] = {{0, 2, 3.5f}, {0, 2, 2.5f}, {2, 2, 2.5f}, {2, 2, 3.5f}};
    const float want_uv[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
    for (int k = 0; k < 4; k++) {
        float out[3], uv[2];
        ParticleCorner(p, r, u, 2, 0, 0, k, out, uv);
        for (int i = 0; i < 3; i++) CHECK(out[i] == doctest::Approx(want[k][i]));
        CHECK(uv[0] == want_uv[k][0]);
        CHECK(uv[1] == want_uv[k][1]);
    }

    // against the instructions, over angles past +-pi, swing arms and axes
    // that aren't the camera's
    uint32_t seed = 12345;
    auto rnd = [&](float lo, float hi) {
        seed = seed * 1664525u + 1013904223u;
        return lo + (hi - lo) * float(seed >> 8) / float(1u << 24);
    };
    for (int trial = 0; trial < 200; trial++) {
        float pp[3], rr[3], uu[3];
        for (int i = 0; i < 3; i++) {
            pp[i] = rnd(-100, 100);
            rr[i] = rnd(-1, 1);
            uu[i] = rnd(-1, 1);
        }
        const float size = rnd(0, 10), angle = rnd(-20, 20), swing = rnd(-3, 3);
        for (int k = 0; k < 4; k++) {
            float out[3], uv[2], ref[3];
            ParticleCorner(pp, rr, uu, size, angle, swing, k, out, uv);
            ParticleCornerUcode(pp, rr, uu, size, angle, swing, uv[0], uv[1], ref);
            for (int i = 0; i < 3; i++) CHECK(out[i] == doctest::Approx(ref[i]).epsilon(1e-4));
        }
    }
}

// The soft particle pixel shader (66C00A7A56838997, out/research/
// softparticle_survey.md 2) reads the scene's depth from s9, the D3D depth
// the camera's projection gave it (post_model's GameDepth: 1 - z, z the near
// and far planes mapped into the camera's z range), back to view depth by
// c89 = (near, far, 1/(zmax-zmin), zmin/(zmax-zmin)), and fades the alpha by
// sat((Zs - w) * c254.z), c254.z = 1/48. SoftFade takes the native depth
// (1/w) instead, which should be the same view depth.
TEST_CASE("a soft particle fades as the game's shader does") {
    uint32_t seed = 4242;
    auto rnd = [&](double lo, double hi) {
        seed = seed * 1664525u + 1013904223u;
        return lo + (hi - lo) * double(seed >> 8) / double(1u << 24);
    };
    int none = 0, part = 0, full = 0;
    for (int trial = 0; trial < 2000; trial++) {
        const double near_plane = rnd(1, 20), far_plane = rnd(1000, 20000);
        const double zmin = rnd(0, 0.2), zmax = rnd(0.8, 1);
        const double c89[4] = {near_plane, far_plane, 1 / (zmax - zmin), zmin / (zmax - zmin)};
        // the scene's view depth, and the particle's around it
        const double scene = rnd(near_plane, far_plane);
        const double w = scene + rnd(-120, 60);
        const double z = (far_plane - far_plane * near_plane / scene) / (far_plane - near_plane) *
                             (zmax - zmin) + zmin;
        const double s9 = 1 - z;
        const double zs = c89[0] * c89[1] /
                          (c89[1] - ((1 - s9) * c89[2] - c89[3]) * (c89[1] - c89[0]));
        const double want = std::clamp((zs - w) * (1.0 / 48.0), 0.0, 1.0);
        const float got = SoftFadeCpu(float(far_plane), float(1 / scene), float(w));
        CHECK(got == doctest::Approx(want).epsilon(2e-4));
        if (want <= 0) none++;
        else if (want >= 1) full++;
        else part++;
    }
    // every case: hidden behind the scene, fading, and clear of it
    CHECK(none > 100);
    CHECK(part > 100);
    CHECK(full > 100);

    // where nothing drew the game's depth is the clear's 0, which with zmax 1
    // reads back as the far plane, as 1/w's 0 does
    const double near_plane = 10, far_plane = 10000, zmin = 0.1, zmax = 1;
    const double zs = near_plane * far_plane /
                      (far_plane - ((1 - 0.0) / (zmax - zmin) - zmin / (zmax - zmin)) *
                                       (far_plane - near_plane));
    CHECK(zs == doctest::Approx(far_plane));
    CHECK(SoftFadeCpu(float(far_plane), 0.0f, float(far_plane - 24)) == doctest::Approx(0.5f));
    CHECK(SoftFadeCpu(float(far_plane), 0.0f, 500.0f) == 1.0f);
}
