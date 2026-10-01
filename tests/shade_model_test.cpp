// Checks the native view's shading (src/Render/shade_model.cpp, which runs
// src/Render/shaders/shade.hlsli on the CPU, as mesh.hlsl does on the GPU)
// against the M2 research's Python models of the game's own shaders
// (tools/shaders/research: fam3.py, skin2.py and hair3.py, checked there
// against the shaders' microcode), the SH occlusion and REFRACT_WORLD's
// picture behind against hand-worked numbers, and PackShade's reading of the
// option word.

#include <doctest/doctest.h>
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
};

// Made from the models (fam3.model, skin2.skin, hair3.hair) by
// tools/shaders/research/gen_shade_cases.py, with no normal or
// environment map, shadow or projected light. The hair's case has no
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
                      zero, out);
        CheckColour(c, out);

        // a vertex-lit material's vertex gives its pixel the same colour, at
        // the vertex; its specular map is per pixel only
        if (c.flags & kShadeSpecMap) continue;
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

    // no light bits: unlit, with the ambient it was given
    s = MakeState(Bit(kDiffuseMap));
    PackShade(it, &s, o, true, sp);
    CHECK_FALSE(Has(sp, kShadeLit));
    CHECK(sp.ambient.x == 0.5f);

    // particles: vertex colour times c0, no ambient
    s = MakeState(Bit(kPrelit), 14);
    PackShade(it, &s, o, true, sp);
    CHECK_FALSE(Has(sp, kShadeLit));
    CHECK(Has(sp, kShadePrelit));
    CHECK(sp.ambient.x == 1.0f);

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
