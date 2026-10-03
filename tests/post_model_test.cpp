// Checks src/Render/post_model.cpp (which runs src/Render/shaders/
// post_model.hlsli on the CPU, as post.hlsl does on the GPU): the blurs' taps
// against what the game gave its shaders, the native depth read as the game's
// depth texture against the c24 the game's composite drew with, the
// composite (the spotlights', soft particles' and noise's terms too)
// against the models of the game's composite shaders that
// tools/shaders/research/post/check_post.py and check_noise.py check in an
// interpreter, glare's pass over bloom's level 0
// against its model there too, the camera motion blur (the velocity pass
// against the geometry it stands for, the composite's blur against the
// model check_velocity.py checks), and the passes together on a plain
// picture. Captured
// numbers are from render_song.b3t's 10s and render_song_evenodd.b3t's 25s
// (kept in out/m4), and its intro (out/parity_spot) for the spotlights.

#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "src/Render/post_model.h"
#include "src/Render/sample_model.h"

using namespace band3::render;
using namespace band3::render::post;

namespace {

bool Near(float a, float b, float tolerance = 1e-5f) { return std::fabs(a - b) <= tolerance; }
float Sat(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

// check_post.py's dof_amount
float ModelDofAmount(const float c24[4], float depth) {
    const float t = (1 - depth) * c24[0] + c24[1];
    return Sat(std::fmin(std::fmax(std::fabs(t), c24[2]), c24[3]));
}

// One composite's inputs, and check_post.py's models of the variants the
// native composite stands for. vol and dens are the spotlights' depth volume
// and the density map's red, spot c127.x, c127.y and c91.x, soft the
// soft-particle buffer (s4), which 63306D35 and the spotlights' 0F105E2D
// read (the others' models have none: it reads 0 there).
// n0 and n1 are the noise map's two taps (s13), c112 and c113 the noise's
// constants.
struct Inputs {
    float scene[4], dof[4], depth, l0[3], l1[3], l2[3], c6[3], c24[4], rows[3][4];
    float vol[3], dens, spot[3], soft[3];
    float n0[3], n1[3], c112[4], c113[4];
};

// the spotlights' term in channel k (check_post.py's spot_term)
float SpotTerm(const Inputs& in, int k) {
    return in.vol[k] * (in.spot[0] + in.spot[1] * in.dens) * in.spot[2];
}

// 63306D35: DOF, soft particles, bloom (screen), colour matrix
void Model63306D35(const Inputs& in, float out[3]) {
    const float a = ModelDofAmount(in.c24, in.depth);
    float rgb[3];
    for (int k = 0; k < 3; k++) {
        rgb[k] = in.scene[k] + (in.dof[k] - in.scene[k]) * a + in.soft[k];
        rgb[k] = 1 - (1 - rgb[k]) * (1 - (in.l0[k] + in.l1[k] + in.l2[k]) * in.c6[k]);
    }
    for (int ch = 0; ch < 3; ch++)
        out[ch] = Sat(in.rows[ch][0] * rgb[0] + in.rows[ch][1] * rgb[1] + in.rows[ch][2] * rgb[2] +
                      in.rows[ch][3]);
}
// C91275BB: DOF only
void ModelC91275BB(const Inputs& in, float out[3]) {
    const float a = ModelDofAmount(in.c24, in.depth);
    for (int k = 0; k < 3; k++) out[k] = Sat(in.scene[k] + (in.dof[k] - in.scene[k]) * a);
}
// 2F002AB2: bloom only
void Model2F002AB2(const Inputs& in, float out[3]) {
    for (int k = 0; k < 3; k++)
        out[k] = Sat(1 - (1 - in.scene[k]) * (1 - (in.l0[k] + in.l1[k] + in.l2[k]) * in.c6[k]));
}
// C6A009EA: glare only
void ModelC6A009EA(const Inputs& in, float out[3]) {
    for (int k = 0; k < 3; k++) out[k] = Sat(in.scene[k] + 0.5f * in.l0[k] * in.c6[k]);
}
// 0F105E2D: DOF, glare, colour matrix (4AAFF3D3 is the same without it)
void Model0F105E2D(const Inputs& in, float out[3]) {
    const float a = ModelDofAmount(in.c24, in.depth);
    float rgb[3];
    for (int k = 0; k < 3; k++)
        rgb[k] = in.scene[k] + (in.dof[k] - in.scene[k]) * a + 0.5f * in.l0[k] * in.c6[k];
    for (int ch = 0; ch < 3; ch++)
        out[ch] = Sat(in.rows[ch][0] * rgb[0] + in.rows[ch][1] * rgb[1] + in.rows[ch][2] * rgb[2] +
                      in.rows[ch][3]);
}

// 0F105E2D with the spotlights' term (+0x25): DOF, soft particles, glare,
// spotlights, colour matrix
void Model0F105E2DSpot(const Inputs& in, float out[3]) {
    const float a = ModelDofAmount(in.c24, in.depth);
    float rgb[3];
    for (int k = 0; k < 3; k++)
        rgb[k] = in.scene[k] + (in.dof[k] - in.scene[k]) * a + in.soft[k] +
                 0.5f * in.l0[k] * in.c6[k] + SpotTerm(in, k);
    for (int ch = 0; ch < 3; ch++)
        out[ch] = Sat(in.rows[ch][0] * rgb[0] + in.rows[ch][1] * rgb[1] + in.rows[ch][2] * rgb[2] +
                      in.rows[ch][3]);
}
// 6EF4844D: DOF, bloom (screen), spotlights, colour matrix
void Model6EF4844D(const Inputs& in, float out[3]) {
    const float a = ModelDofAmount(in.c24, in.depth);
    float rgb[3];
    for (int k = 0; k < 3; k++) {
        rgb[k] = in.scene[k] + (in.dof[k] - in.scene[k]) * a;
        rgb[k] = 1 - (1 - rgb[k]) * (1 - (in.l0[k] + in.l1[k] + in.l2[k]) * in.c6[k]) +
                 SpotTerm(in, k);
    }
    for (int ch = 0; ch < 3; ch++)
        out[ch] = Sat(in.rows[ch][0] * rgb[0] + in.rows[ch][1] * rgb[1] + in.rows[ch][2] * rgb[2] +
                      in.rows[ch][3]);
}
// F7E2A8FB: spotlights only
void ModelF7E2A8FB(const Inputs& in, float out[3]) {
    for (int k = 0; k < 3; k++) out[k] = Sat(in.scene[k] + SpotTerm(in, k));
}

// check_noise.py's noise(): the taps' geometric mean, overlaid on rgb by
// its luminance, moved toward by 6.75 c113.w L (1 - L)^2 (or, without the
// midtone weight, c113.w: band3's guess), in place
void NoiseModel(const Inputs& in, bool midtone, float rgb[3]) {
    float n[3];
    for (int k = 0; k < 3; k++) n[k] = std::sqrt(std::fabs(in.n0[k] * in.n1[k]));
    const float l = 0.30f * rgb[0] + 0.59f * rgb[1] + 0.11f * rgb[2];
    const float w = midtone ? 6.75f * in.c113[3] * l * (1 - l) * (1 - l) : in.c113[3];
    for (int k = 0; k < 3; k++) {
        const float ov = l <= 0.5f ? 2 * n[k] * rgb[k] : 1 - 2 * (1 - n[k]) * (1 - rgb[k]);
        rgb[k] = rgb[k] + w * (ov - rgb[k]);
    }
}
// 4FD49280: glare, spotlights, noise (no colour matrix)
void Model4FD49280(const Inputs& in, float out[3]) {
    float rgb[3];
    for (int k = 0; k < 3; k++) rgb[k] = in.scene[k] + 0.5f * in.l0[k] * in.c6[k] + SpotTerm(in, k);
    NoiseModel(in, true, rgb);
    for (int k = 0; k < 3; k++) out[k] = Sat(rgb[k]);
}
// 0A9D6EAE: DOF, spotlights, noise
void Model0A9D6EAE(const Inputs& in, float out[3]) {
    const float a = ModelDofAmount(in.c24, in.depth);
    float rgb[3];
    for (int k = 0; k < 3; k++)
        rgb[k] = in.scene[k] + (in.dof[k] - in.scene[k]) * a + SpotTerm(in, k);
    NoiseModel(in, true, rgb);
    for (int k = 0; k < 3; k++) out[k] = Sat(rgb[k]);
}
// D1942A59: DOF, bloom (screen), spotlights, noise
void ModelD1942A59(const Inputs& in, float out[3]) {
    const float a = ModelDofAmount(in.c24, in.depth);
    float rgb[3];
    for (int k = 0; k < 3; k++) {
        rgb[k] = in.scene[k] + (in.dof[k] - in.scene[k]) * a;
        rgb[k] = 1 - (1 - rgb[k]) * (1 - (in.l0[k] + in.l1[k] + in.l2[k]) * in.c6[k]) +
                 SpotTerm(in, k);
    }
    NoiseModel(in, true, rgb);
    for (int k = 0; k < 3; k++) out[k] = Sat(rgb[k]);
}
// the music-video venues' composite (not dumped): glare, noise, then the
// colour matrix, as 4FD49280 (glare -> noise) and 5C1E47C0 (noise ->
// matrix, unsaturated between) put them together
void ModelVideoVenue(const Inputs& in, float out[3]) {
    float rgb[3];
    for (int k = 0; k < 3; k++) rgb[k] = in.scene[k] + 0.5f * in.l0[k] * in.c6[k];
    NoiseModel(in, true, rgb);
    for (int ch = 0; ch < 3; ch++)
        out[ch] = Sat(in.rows[ch][0] * rgb[0] + in.rows[ch][1] * rgb[1] + in.rows[ch][2] * rgb[2] +
                      in.rows[ch][3]);
}
// the noise without the midtone weight (band3's guess: no such shader)
void ModelNoiseFlat(const Inputs& in, float out[3]) {
    float rgb[3] = {in.scene[0], in.scene[1], in.scene[2]};
    NoiseModel(in, false, rgb);
    for (int k = 0; k < 3; k++) out[k] = Sat(rgb[k]);
}

// deterministic inputs in check_post.py's ranges (the spotlights' gains
// about the game's: c127 around 0.01, c91.x 32)
Inputs MakeInputs(uint32_t seed) {
    uint32_t s = seed * 2654435761u + 1;
    auto next = [&](float lo, float hi) {
        s = s * 1664525u + 1013904223u;
        return lo + (hi - lo) * float(s >> 8) / float(1u << 24);
    };
    Inputs in{};
    for (int k = 0; k < 4; k++) {
        in.scene[k] = next(0, 1);
        in.dof[k] = next(0, 1);
    }
    in.depth = next(0, 1);
    for (int k = 0; k < 3; k++) {
        in.l0[k] = next(0, 1);
        in.l1[k] = next(0, 1);
        in.l2[k] = next(0, 1);
        in.c6[k] = next(0, 2);
    }
    float scale = next(0, 1), bias = next(0, 1);
    if (std::fabs(scale - bias) < 1e-3f) bias = scale + 0.01f;
    const float mx = next(0, 1), mn = next(0, 1);
    in.c24[0] = 1 / (scale - bias);
    in.c24[1] = -scale / (scale - bias);
    in.c24[2] = std::fmin(mx, mn);
    in.c24[3] = mx;
    for (int j = 0; j < 3; j++)
        for (int k = 0; k < 4; k++) in.rows[j][k] = next(-1, 1);
    for (int k = 0; k < 3; k++) in.vol[k] = next(0, 1);
    in.dens = next(0, 1);
    in.spot[0] = next(0, 0.03f);
    in.spot[1] = next(0, 0.03f);
    in.spot[2] = next(0, 40);
    for (int k = 0; k < 3; k++) in.soft[k] = next(0, 1);
    // check_noise.py's ranges: seeds 0..1, base scales 1..40, top 0.5..2,
    // intensity -3..3; the taps 0..1
    for (int k = 0; k < 3; k++) {
        in.n0[k] = next(0, 1);
        in.n1[k] = next(0, 1);
    }
    for (int k = 0; k < 4; k++) in.c112[k] = next(0, 1);
    in.c113[0] = next(1, 40);
    in.c113[1] = next(1, 40);
    in.c113[2] = next(0.5f, 2);
    in.c113[3] = next(-3, 3);
    return in;
}

PostPass PassFor(const Inputs& in, uint32_t flags) {
    PostPass p{};
    p.flags = {flags, 0, 0, 0};
    p.c6 = {in.c6[0], in.c6[1], in.c6[2], 0};
    p.c24 = {in.c24[0], in.c24[1], in.c24[2], in.c24[3]};
    for (int j = 0; j < 3; j++)
        p.xfm[j] = {in.rows[j][0], in.rows[j][1], in.rows[j][2], in.rows[j][3]};
    p.spot = {in.spot[0], in.spot[1], in.spot[2], 0};
    p.noise_seeds = {in.c112[0], in.c112[1], in.c112[2], in.c112[3]};
    p.noise = {in.c113[0], in.c113[1], in.c113[2], in.c113[3]};
    return p;
}

}  // namespace

TEST_CASE("the bloom blur's weights add up to 1 and its taps are the game's") {
    float sum = 0;
    for (float w : kBloomWeights) sum += w;
    CHECK(Near(sum, 1.0f, 1e-6f));
    for (int i = 0; i < 7; i++) CHECK(kBloomWeights[i] == kBloomWeights[14 - i]);

    // the first Bloom_Blur of render_song.b3t's 10s: level 0 (320 across),
    // across; c31..c45 x and c47..c61
    const float captured[15][2] = {
        {-0.0203125011f, 0.0159283932f}, {-0.0171875004f, 0.0270778369f},
        {-0.0140625006f, 0.0424231887f}, {-0.0109374998f, 0.0612547919f},
        {-0.0078125f, 0.0815124959f},    {-0.00468750019f, 0.0999667868f},
        {-0.00156250002f, 0.112988606f}, {0.00156250002f, 0.117695794f},
        {0.00468750019f, 0.112988606f},  {0.0078125f, 0.0999667868f},
        {0.0109374998f, 0.0815124959f},  {0.0140625006f, 0.0612547919f},
        {0.0171875004f, 0.0424231887f},  {0.0203125011f, 0.0270778369f},
        {0.0234375f, 0.0159283932f}};
    float4 taps[15];
    BloomTaps(false, 320, taps);
    for (int i = 0; i < 15; i++) {
        CHECK(Near(taps[i].x, captured[i][0], 1e-8f));
        CHECK(taps[i].y == 0);
        CHECK(Near(taps[i].z, captured[i][1], 1e-8f));
    }
    BloomTaps(true, 180, taps);
    CHECK(taps[0].x == 0);
    CHECK(Near(taps[0].y, -6.5f / 180.0f, 1e-8f));
}

TEST_CASE("the DOF blur's taps are the game's") {
    // NgDOFProc::DoPost's last blur (down) in render_song.b3t's 10s, c31..c38
    // (the evenodd 25s has the same), blur width scale 1
    const float captured[8][2] = {
        {-0.00490142778f, -0.0036913252f}, {0.00492000207f, -0.00711239222f},
        {-0.000490051578f, -0.0085968459f}, {0.00179486675f, 0.00271836785f},
        {-0.00476546772f, 0.00423385762f}, {-0.00424284814f, -0.0081319036f},
        {-0.0019916282f, 0.0025601082f},   {0.00507223466f, 0.0069974754f}};
    float4 taps[8];
    DofTaps(true, 1.0f, taps);
    for (int i = 0; i < 8; i++) {
        CHECK(Near(taps[i].x, captured[i][0], 1e-8f));
        CHECK(Near(taps[i].y, captured[i][1], 1e-8f));
        CHECK(taps[i].z == 0.125f);
    }
    // across is a disc of its own, and the scale widens both
    DofTaps(false, 2.0f, taps);
    CHECK(Near(taps[0].x, 0.4432332516f * 5 * 2 * 0.666f / 640, 1e-7f));
}

TEST_CASE("the native depth reads as the game's depth texture by the captured c24") {
    // render_song.b3t's 10s: c24 as the game's composite drew with it, the
    // world camera, and the DOF's focal plane and blur depth (NgDOFProc)
    struct Frame {
        float c24[4], focal, blur_depth;
    };
    const Frame frames[] = {
        {{10.5741863f, -8.72656918f, 0, 1}, 51.295414f, 0.349999994f},
        {{23.145937f, -21.3096447f, 0, 1}, 112.280983f, 0.349999994f},  // evenodd 25s
    };
    for (const Frame& f : frames) {
        PostPass p{};
        p.camera = {10.0f, 10000.0f, 0.100000001f, 1.0f};
        // in focus at the focal plane, fully blurred where the blur depth
        // ends in front of it and far behind it, and at nothing drawn (w
        // infinite)
        CHECK(DofAmountCpu(f.c24, GameDepthCpu(p, 1 / f.focal)) < 2e-3f);
        CHECK(DofAmountCpu(f.c24, GameDepthCpu(p, 1 / (f.focal * (1 - f.blur_depth)))) >
              0.998f);
        CHECK(DofAmountCpu(f.c24, GameDepthCpu(p, 0)) == 1.0f);
        // and in between as the view depth says: t = (1 - F/d) (1 - b)/b
        for (float d : {0.7f, 0.8f, 0.9f, 0.95f, 1.05f, 1.2f, 1.5f}) {
            const float depth = f.focal * d;
            const float t = (1 - f.focal / depth) * (1 - f.blur_depth) / f.blur_depth;
            CHECK(Near(DofAmountCpu(f.c24, GameDepthCpu(p, 1 / depth)), Sat(std::fabs(t)), 2e-3f));
        }
    }
}

TEST_CASE("the composite is the game's composite shaders' maths") {
    using Model = void (*)(const Inputs&, float[3]);
    struct Variant {
        const char* name;
        uint32_t flags;
        Model model;
    };
    const Variant variants[] = {
        {"63306D35 DOF soft bloom xfm", kPostDof | kPostSoft | kPostBloom | kPostXfm,
         Model63306D35},
        {"C91275BB DOF", kPostDof, ModelC91275BB},
        {"2F002AB2 bloom", kPostBloom, Model2F002AB2},
        {"C6A009EA glare", kPostGlare, ModelC6A009EA},
        {"0F105E2D DOF glare xfm", kPostDof | kPostGlare | kPostXfm, Model0F105E2D},
        {"0F105E2D DOF soft glare spot xfm",
         kPostDof | kPostSoft | kPostGlare | kPostSpot | kPostXfm, Model0F105E2DSpot},
        {"6EF4844D DOF bloom spot xfm", kPostDof | kPostBloom | kPostSpot | kPostXfm,
         Model6EF4844D},
        {"F7E2A8FB spot", kPostSpot, ModelF7E2A8FB},
        {"4FD49280 glare spot noise", kPostGlare | kPostSpot | kPostNoise | kPostNoiseMidtone,
         Model4FD49280},
        {"0A9D6EAE DOF spot noise", kPostDof | kPostSpot | kPostNoise | kPostNoiseMidtone,
         Model0A9D6EAE},
        {"D1942A59 DOF bloom spot noise",
         kPostDof | kPostBloom | kPostSpot | kPostNoise | kPostNoiseMidtone, ModelD1942A59},
        {"video venues: glare noise xfm", kPostGlare | kPostNoise | kPostNoiseMidtone | kPostXfm,
         ModelVideoVenue},
        {"noise without the midtone weight", kPostNoise, ModelNoiseFlat},
    };
    for (const Variant& v : variants) {
        CAPTURE(v.name);
        for (uint32_t seed = 0; seed < 30; seed++) {
            const Inputs in = MakeInputs(seed);
            float want[3], got[3];
            v.model(in, want);
            CompositeCpu(PassFor(in, v.flags), in.scene, in.dof, in.depth, in.l0, in.l1, in.l2,
                         in.vol, in.dens, in.soft, in.n0, in.n1, got);
            for (int k = 0; k < 3; k++) CHECK(Near(got[k], want[k], 1e-5f));
        }
    }
}

TEST_CASE("PlanPost takes the composite's constants, on frames that drew post") {
    FrameCapture f;
    PostPlan plan{};
    CHECK_FALSE(PlanPost(f, 0, plan));  // no DoPostProcess read

    f.post.valid = 1;
    f.post.proc = 0x1000;
    f.post.cam_near = 10;
    f.post.cam_far = 10000;
    f.post.cam_zrange[0] = 0.1f;
    f.post.cam_zrange[1] = 1;
    f.post_boundary = 0;
    // parameters that would turn glare bloom, DOF and the colour matrix on
    f.post.bloom_intensity = 2;
    f.post.bloom_glare = 1;
    f.post.dof = 0x2000;
    f.post.dof_enabled = 1;
    f.post.dof_scale = 0.9f;
    f.post.dof_bias = 0.8f;
    f.post.dof_max_blur = 1;
    f.post.color_mod = 0.5f;

    // without FinishPostProcess the game drew no post, whatever the
    // parameters say: a frame that does neither, and a post frame whose
    // FinishPostProcess didn't run
    for (uint32_t proc_cmds : {0u, 2u, 7u}) {
        f.proc_cmds = proc_cmds;
        CHECK_FALSE(PlanPost(f, 0, plan));
    }
    // a world frame with even/odd rendering gets the post the next frame
    // gives its world, worked out from its parameters
    f.proc_cmds = 1;
    REQUIRE(PlanPost(f, 0, plan));
    CHECK((plan.composite.flags.x & (kPostDof | kPostGlare)) == (kPostDof | kPostGlare));
    CHECK((plan.composite.flags.x & kPostBloom) == 0u);

    // the composite's own, when it ran: its flags and constants
    f.post_consts.valid = 1;
    f.post_consts.flags[kPostFlagBloom] = 1;
    f.post_consts.flags[kPostFlagColorXfm] = 1;
    f.post_consts.c6[0] = 0.25f;
    f.post_consts.c24[0] = 10;
    f.post_consts.c92[0][0] = 0.5f;
    for (uint32_t proc_cmds : {7u, 2u}) {
        f.proc_cmds = proc_cmds;
        REQUIRE(PlanPost(f, 0, plan));
        CHECK(plan.composite.flags.x == (kPostBloom | kPostXfm));
        CHECK(Near(plan.composite.c6.x, 0.25f));
        CHECK(Near(plan.composite.c24.x, 10));
        CHECK(Near(plan.composite.xfm[0].x, 0.5f));
        CHECK(Near(plan.composite.camera.y, 10000));
        CHECK(Near(plan.bloom_taps[2][0][0].x, -6.5f / 20, 1e-7f));  // level 2, 20 across
        CHECK(Near(plan.bloom_taps[2][1][0].y, -6.5f / 11, 1e-7f));  // 11 down
    }
    REQUIRE(PlanPost(f, kPostXfm, plan));
    CHECK(plan.composite.flags.x == kPostXfm);
    CHECK_FALSE(PlanPost(f, kPostDof, plan));  // the composite had no DOF

    // constants on a frame without the post bit (none should be) aren't used:
    // a world frame works its post out from its parameters
    f.proc_cmds = 1;
    REQUIRE(PlanPost(f, 0, plan));
    CHECK((plan.composite.flags.x & kPostGlare) != 0u);
    CHECK_FALSE(Near(plan.composite.c6.x, 0.25f));
    f.proc_cmds = 0;
    CHECK_FALSE(PlanPost(f, 0, plan));
    f.proc_cmds = 2;

    // DOF without the camera's planes can't read the depth
    f.post_consts.flags[kPostFlagDof] = 1;
    REQUIRE(PlanPost(f, 0, plan));
    CHECK(plan.composite.flags.x == (kPostDof | kPostBloom | kPostXfm));
    f.post.cam_far = 0;
    REQUIRE(PlanPost(f, 0, plan));
    CHECK(plan.composite.flags.x == (kPostBloom | kPostXfm));

    f.post.disabled = 1;
    CHECK_FALSE(PlanPost(f, 0, plan));
}

TEST_CASE("PlanPost turns the spotlights' term on where the game's composite had it") {
    // the intro of render_song.b3t: the composite's constants (c127, c91 as
    // NgSpotlightDrawer left them) and the drawer's passes after the post
    // boundary: the density map, the depth volume's cones, its two blurs
    FrameCapture f;
    f.post.valid = 1;
    f.post.proc = 0x1000;
    f.post_boundary = 81;
    f.proc_cmds = 7;
    f.post_consts.valid = 1;
    f.post_consts.c127[0] = 0.01f;
    f.post_consts.c127[1] = 0.0099f;
    f.post_consts.c91[0] = 32;
    f.post_consts.c91[1] = 10000;
    auto pass = [&](uint32_t tex_obj, uint32_t tex_type, uint32_t first) {
        Pass p;
        p.tex_obj = tex_obj;
        p.tex_type = tex_type;
        p.first_draw = first;
        p.draw_count = 1;
        f.passes.push_back(p);
    };
    PostPlan plan{};
    // without the flag (or before captures had it) there's no term
    pass(0x241B7B38, kTexTypeDensityMap, 81);
    pass(0x241B7A08, kTexTypeDepthVolume, 82);
    pass(0x241B7A08, kTexTypeDepthVolume, 85);
    pass(0x241B7A08, kTexTypeDepthVolume, 86);
    CHECK_FALSE(PlanPost(f, 0, plan));
    f.post_consts.spot_flag = 1;
    REQUIRE(PlanPost(f, 0, plan));
    CHECK(plan.composite.flags.x == kPostSpot);
    CHECK(plan.spot_volume == 0x241B7A08u);
    CHECK(plan.spot_density == 0x241B7B38u);
    CHECK(Near(plan.composite.spot.x, 0.01f));
    CHECK(Near(plan.composite.spot.y, 0.0099f));
    CHECK(Near(plan.composite.spot.z, 32));
    CHECK(plan.composite.spot.w == 0);
    // alone, and left off
    REQUIRE(PlanPost(f, kPostSpot, plan));
    CHECK(plan.composite.flags.x == kPostSpot);
    CHECK_FALSE(PlanPost(f, kPostXfm, plan));
    // a depth volume from before the post boundary isn't the drawer's
    // (nor is a frame without one); without a density map the term reads 0
    f.passes.clear();
    pass(0x241B7A08, kTexTypeDepthVolume, 40);
    CHECK_FALSE(PlanPost(f, 0, plan));
    pass(0x241B7A08, kTexTypeDepthVolume, 82);
    REQUIRE(PlanPost(f, 0, plan));
    CHECK(plan.spot_density == 0u);
    // world frames (even/odd) have no drawer: no term
    f.proc_cmds = 1;
    f.post.cam_near = 10;
    f.post.cam_far = 10000;
    f.post.color_mod = 0.5f;
    REQUIRE(PlanPost(f, 0, plan));
    CHECK((plan.composite.flags.x & kPostSpot) == 0u);
    CHECK(plan.spot_volume == 0u);
}

TEST_CASE("PlanPost adds the soft particles where the game's composite had them") {
    // render_song.b3t's 10s: RndSoftParticleBuffer's surfaces, the pass that
    // cleared the first and drew its particles, then its blur into the
    // second and back
    FrameCapture f;
    f.post.valid = 1;
    f.post.proc = 0x1000;
    f.post_boundary = 641;
    f.proc_cmds = 7;
    f.post_consts.valid = 1;
    auto pass = [&](uint32_t tex_obj, uint32_t clear, uint32_t first) {
        Pass p;
        p.tex_obj = tex_obj;
        p.tex_type = 0x22;
        p.clear_flags = clear;
        p.first_draw = first;
        p.draw_count = 1;
        f.passes.push_back(p);
    };
    pass(0x2387CBE8, 0x0f, 706);
    pass(0x2387CC98, 0, 709);
    pass(0x2387CBE8, 0, 710);
    PostPlan plan{};
    // not without the flag, nor without the surfaces (captures from before)
    f.post_consts.soft_surface[0] = 0x2387CBE8;
    f.post_consts.soft_surface[1] = 0x2387CC98;
    CHECK_FALSE(PlanPost(f, 0, plan));
    f.post_consts.flags[kPostFlagSoft] = 1;
    REQUIRE(PlanPost(f, 0, plan));
    CHECK(plan.composite.flags.x == kPostSoft);
    CHECK(plan.soft == 0x2387CBE8u);
    CHECK_FALSE(PlanPost(f, kPostXfm, plan));
    f.post_consts.soft_surface[0] = 0;
    CHECK_FALSE(PlanPost(f, 0, plan));
    // nor without the pass that drew the particles: the blurs alone have
    // nothing to blur the capture has
    f.post_consts.soft_surface[0] = 0x2387CBE8;
    f.passes.erase(f.passes.begin());
    CHECK_FALSE(PlanPost(f, 0, plan));
}

TEST_CASE("the passes on a plain picture: bloom of a flat colour is that colour") {
    // flat scene and depth: every level is the colour times its alpha (the
    // bright pass), blurs of a flat level are that level (the weights add up
    // to 1), and the DOF blur is the scene, so the composite is the scene
    // screen-blended with three times colour * alpha * c6
    const uint32_t w = 64, h = 36;
    const uint32_t r = 200, g = 100, b = 40, a = 128;
    std::vector<uint32_t> scene(size_t(w) * h, r | g << 8 | b << 16 | a << 24);
    std::vector<float> depth(scene.size(), 1.0f / 50.0f);
    PostPlan plan{};
    plan.composite.flags = {kPostBloom | kPostDof, 0, 0, 0};
    plan.composite.c6 = {0.5f, 0.5f, 0.5f, 0};
    plan.composite.c24 = {1, 0, 0, 1};
    plan.composite.camera = {10, 10000, 0.1f, 1};
    DofTaps(false, 1, plan.dof_taps[0]);
    DofTaps(true, 1, plan.dof_taps[1]);
    for (int k = 0; k < 3; k++) {
        BloomTaps(false, 320 >> (2 * k), plan.bloom_taps[k][0]);
        BloomTaps(true, 180 >> (2 * k), plan.bloom_taps[k][1]);
    }
    std::vector<uint32_t> out;
    RunPost(plan, scene, depth, w, h, {}, {}, {}, out);
    REQUIRE(out.size() == scene.size());
    const float alpha = float(a) / 255;
    const uint32_t in[3] = {r, g, b};
    for (size_t i : {size_t(0), out.size() / 2, out.size() - 1}) {
        for (int k = 0; k < 3; k++) {
            const float c = float(in[k]) / 255;
            // each level keeps 8 bits of c * alpha
            const float level = std::floor(c * alpha * 255 + 0.5f) / 255;
            const float want = 1 - (1 - c) * (1 - 3 * level * 0.5f);
            const float got = float(out[i] >> (8 * k) & 0xff) / 255;
            CHECK(Near(got, want, 1.5f / 255));
        }
        CHECK((out[i] >> 24) == 0xff);
    }
}

TEST_CASE("the spotlights' term adds the depth volume by the density map's red") {
    // a flat scene, depth volume and density map at sizes of their own (the
    // game's are 640x360 and 320x180): every pixel is the scene plus the
    // volume times (c127.x + c127.y * red) * c91.x, rounded to 8 bits once
    const uint32_t w = 64, h = 36;
    const uint32_t r = 40, g = 60, b = 20;
    std::vector<uint32_t> scene(size_t(w) * h, r | g << 8 | b << 16);
    std::vector<float> depth(scene.size(), 0.0f);
    const uint32_t vol_px = 50 | 80 << 8 | 30 << 16 | 0xffu << 24;
    const uint32_t dens_px = 150 | 200 << 8 | 0x99u << 24;
    std::vector<uint32_t> vol(40 * 20, vol_px), dens(16 * 9, dens_px);
    PostPlan plan{};
    plan.composite.flags = {kPostSpot, 0, 0, 0};
    plan.composite.spot = {0.01f, 0.0099f, 32, 0};
    std::vector<uint32_t> out;
    RunPost(plan, scene, depth, w, h, {vol.data(), 40, 20}, {dens.data(), 16, 9}, {}, out);
    REQUIRE(out.size() == scene.size());
    const float gain = (0.01f + 0.0099f * 150.0f / 255.0f) * 32.0f;
    const uint32_t in[3] = {r, g, b}, v[3] = {50, 80, 30};
    for (size_t i : {size_t(0), out.size() / 2, out.size() - 1}) {
        for (int k = 0; k < 3; k++) {
            const float want = float(in[k]) / 255 + float(v[k]) / 255 * gain;
            const float got = float(out[i] >> (8 * k) & 0xff) / 255;
            CHECK(Near(got, Sat(want), 1.0f / 255));
        }
    }
    // no density map: the term is c127.x's alone; no volume: none
    RunPost(plan, scene, depth, w, h, {vol.data(), 40, 20}, {}, {}, out);
    CHECK(Near(float(out[0] & 0xff) / 255, float(r) / 255 + 50.0f / 255 * 0.01f * 32, 1.0f / 255));
    RunPost(plan, scene, depth, w, h, {}, {}, {}, out);
    CHECK((out[0] & 0xffffffu) == (scene[0] & 0xffffffu));
}

TEST_CASE("the soft particles add before bloom, which they don't brighten") {
    // a flat scene with bloom, and a soft-particle buffer at the game's
    // 320x180: the composite is (scene + soft) screen-blended with the
    // bloom of the scene alone (the bright pass reads the scene)
    const uint32_t w = 64, h = 36;
    const uint32_t r = 60, g = 30, b = 90, a = 100;
    std::vector<uint32_t> scene(size_t(w) * h, r | g << 8 | b << 16 | a << 24);
    std::vector<float> depth(scene.size(), 0.0f);
    const uint32_t soft_px = 40 | 20 << 8 | 0 << 16 | 0x80u << 24;
    std::vector<uint32_t> soft(320 * 180, soft_px);
    PostPlan plan{};
    plan.composite.flags = {kPostBloom | kPostSoft, 0, 0, 0};
    plan.composite.c6 = {0.5f, 0.5f, 0.5f, 0};
    for (int k = 0; k < 3; k++) {
        BloomTaps(false, 320 >> (2 * k), plan.bloom_taps[k][0]);
        BloomTaps(true, 180 >> (2 * k), plan.bloom_taps[k][1]);
    }
    std::vector<uint32_t> out;
    RunPost(plan, scene, depth, w, h, {}, {}, {soft.data(), 320, 180}, out);
    const float alpha = float(a) / 255;
    const uint32_t in[3] = {r, g, b}, s[3] = {40, 20, 0};
    for (size_t i : {size_t(0), out.size() / 2, out.size() - 1}) {
        for (int k = 0; k < 3; k++) {
            const float c = float(in[k]) / 255;
            const float level = std::floor(c * alpha * 255 + 0.5f) / 255;
            const float want = 1 - (1 - (c + float(s[k]) / 255)) * (1 - 3 * level * 0.5f);
            CHECK(Near(float(out[i] >> (8 * k) & 0xff) / 255, want, 1.5f / 255));
        }
    }
    // without the flag the buffer isn't read
    plan.composite.flags.x = kPostBloom;
    std::vector<uint32_t> none;
    RunPost(plan, scene, depth, w, h, {}, {}, {soft.data(), 320, 180}, none);
    CHECK(none[0] != out[0]);
}

TEST_CASE("glare's pass over level 0 is the game's bloom_glare shader") {
    // check_post.py's model of 2789C57F87CFFD5D: ten taps of the level from
    // the pixel's uv toward the centre, (1 - 2 uv) / 10 apart, each weighed
    // by (1 - 4 min(r^2, 1/4))^2 at its uv, summed as 1 / (1 - weight *
    // texel); the output is 2 - 20 / the sum, which the target saturates
    auto model = [](float level, float u, float v) {
        const float su = (1 - 2 * u) * 0.1f, sv = (1 - 2 * v) * 0.1f;
        float sum = 0;
        for (int i = 0; i < 10; i++) {
            const float r2 = (u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f);
            const float w = (1 - 4 * std::fmin(r2, 0.25f)) * (1 - 4 * std::fmin(r2, 0.25f));
            sum += 1 / (1 - w * level);
            u += su;
            v += sv;
        }
        return Sat(2 - 20 / sum);
    };
    // a flat scene: level 0 is its colour times its alpha everywhere (the
    // bright pass, and blurs of a flat level), so each tap reads that
    const uint32_t w = 64, h = 36;
    const uint32_t r = 250, g = 200, b = 40, a = 255;
    std::vector<uint32_t> scene(size_t(w) * h, r | g << 8 | b << 16 | a << 24);
    std::vector<float> depth(scene.size(), 0.0f);
    PostPlan plan{};
    plan.composite.flags = {kPostGlare, 0, 0, 0};
    plan.composite.c6 = {0.5f, 0.5f, 0.5f, 0};
    BloomTaps(false, 320, plan.bloom_taps[0][0]);
    BloomTaps(true, 180, plan.bloom_taps[0][1]);
    std::vector<uint32_t> out, level;
    RunPost(plan, scene, depth, w, h, {}, {}, {}, out, &level);
    const uint32_t lw = Quarter(w), lh = Quarter(h);
    REQUIRE(level.size() == size_t(lw) * lh);
    const uint32_t in[3] = {r, g, b};
    // the corner (taps all the way across), the middle (taps on the spot,
    // weight 1 at the centre) and one in between
    const uint32_t at[][2] = {{0, 0}, {lw / 2, lh / 2}, {3, 6}, {lw - 1, lh - 1}};
    for (const auto& p : at) {
        CAPTURE(p[0]);
        CAPTURE(p[1]);
        const float u = (float(p[0]) + 0.5f) / float(lw), v = (float(p[1]) + 0.5f) / float(lh);
        const uint32_t texel = level[size_t(p[1]) * lw + p[0]];
        for (int k = 0; k < 3; k++) {
            const float l = std::floor(float(in[k]) / 255 * float(a) / 255 * 255 + 0.5f) / 255;
            const float want = model(l, u, v);
            CHECK(Near(float(texel >> (8 * k) & 0xff) / 255, want, 1.0f / 255));
        }
        CHECK((texel >> 24) == 0xff);  // alpha 1
    }
    // the composite adds half of the level times c6: in the corner, whose
    // pixel reads level 0's corner texel alone
    for (int k = 0; k < 3; k++) {
        const float glare = float(level[0] >> (8 * k) & 0xff) / 255;
        const float want = Sat(float(in[k]) / 255 + 0.5f * 0.5f * glare);
        CHECK(Near(float(out[0] >> (8 * k) & 0xff) / 255, want, 1.0f / 255));
    }
    // a black level stays black
    std::vector<uint32_t> dark(scene.size(), r | g << 8 | b << 16);
    RunPost(plan, dark, depth, w, h, {}, {}, {}, out, &level);
    for (uint32_t c : level) CHECK((c & 0xffffffu) == 0u);
    // bloom (not glare) has no glare pass: level 0 is the bright pass
    plan.composite.flags.x = kPostBloom;
    for (int k = 1; k < 3; k++) {
        BloomTaps(false, 320 >> (2 * k), plan.bloom_taps[k][0]);
        BloomTaps(true, 180 >> (2 * k), plan.bloom_taps[k][1]);
    }
    RunPost(plan, scene, depth, w, h, {}, {}, {}, out, &level);
    CHECK((level[0] & 0xffffffu) == (r | g << 8 | b << 16));
    // nor does a frame without either
    plan.composite.flags.x = kPostXfm;
    RunPost(plan, scene, depth, w, h, {}, {}, {}, out, &level);
    CHECK(level.empty());
}

TEST_CASE("the noise's taps are the game's: (uv + c112.xy) c113.xy and (uv + c112.zw) c113.xyz") {
    // check_noise.py's noise_taps, and the derivatives the taps are read
    // with: their scales over the target's size
    for (uint32_t seed = 0; seed < 20; seed++) {
        const Inputs in = MakeInputs(seed);
        PostPass p = PassFor(in, kPostNoise);
        p.target = {1280, 720, 1.0f / 1280, 1.0f / 720};
        const float uv[2] = {in.scene[0], in.scene[1]};
        float at[2], dx[2], dy[2];
        NoiseTapCpu(p, uv, 0, at, dx, dy);
        CHECK(Near(at[0], (uv[0] + in.c112[0]) * in.c113[0], 1e-4f));
        CHECK(Near(at[1], (uv[1] + in.c112[1]) * in.c113[1], 1e-4f));
        CHECK(Near(dx[0], in.c113[0] / 1280, 1e-7f));
        CHECK(dx[1] == 0);
        CHECK(dy[0] == 0);
        CHECK(Near(dy[1], in.c113[1] / 720, 1e-7f));
        NoiseTapCpu(p, uv, 1, at, dx, dy);
        CHECK(Near(at[0], (uv[0] + in.c112[2]) * in.c113[0] * in.c113[2], 1e-4f));
        CHECK(Near(at[1], (uv[1] + in.c112[3]) * in.c113[1] * in.c113[2], 1e-4f));
        CHECK(Near(dx[0], in.c113[0] * in.c113[2] / 1280, 1e-7f));
        CHECK(Near(dy[1], in.c113[1] * in.c113[2] / 720, 1e-7f));
    }
}

TEST_CASE("the noise's midtone weight peaks at a third and leaves black and white be") {
    // the song-video capture's c113.w (render_screens_song.b3t), and taps
    // whose geometric mean is n: the move toward the overlay is 6.75 I L
    // (1 - L)^2 of the way, 2.65 times it at L = 1/3, none at 0 and 1
    Inputs in{};
    in.c113[3] = 2.65f;
    const float n = 0.6f;
    for (int k = 0; k < 3; k++) in.n0[k] = in.n1[k] = n;
    const uint32_t flags = kPostNoise | kPostNoiseMidtone;
    for (float grey : {0.0f, 1.0f / 3, 0.5f, 0.75f, 1.0f}) {
        CAPTURE(grey);
        for (int k = 0; k < 4; k++) in.scene[k] = grey;
        float got[3];
        CompositeCpu(PassFor(in, flags), in.scene, in.dof, 0, in.l0, in.l1, in.l2, in.vol, 0,
                     in.soft, in.n0, in.n1, got);
        const float w = 6.75f * 2.65f * grey * (1 - grey) * (1 - grey);
        const float ov = grey <= 0.5f ? 2 * n * grey : 1 - 2 * (1 - n) * (1 - grey);
        CHECK(Near(got[0], Sat(grey + w * (ov - grey)), 1e-5f));
    }
    // at n 0.5 the overlay of a grey at or under 0.5 is the grey itself: no
    // change, whatever the weight
    for (int k = 0; k < 3; k++) in.n0[k] = in.n1[k] = 0.5f;
    for (int k = 0; k < 4; k++) in.scene[k] = 0.3f;
    float got[3];
    CompositeCpu(PassFor(in, flags), in.scene, in.dof, 0, in.l0, in.l1, in.l2, in.vol, 0, in.soft,
                 in.n0, in.n1, got);
    CHECK(Near(got[1], 0.3f, 1e-6f));
}

TEST_CASE("PlanPost turns the noise on where the game's composite had it and the map was kept") {
    // render_screens_song.b3t's song-video (out/n1/c): c112, c113 and the
    // flags +0x2D, +0x2E as the composite drew with them
    FrameCapture f;
    f.post.valid = 1;
    f.post.proc = 0x1000;
    f.post_boundary = 0;
    f.proc_cmds = 7;
    f.post_consts.valid = 1;
    const float c112[4] = {0.7767f, 0.8988f, 0.7839f, 0.8558f};
    const float c113[4] = {2.5f, 2.5f, 1.35914f, 2.65f};
    std::copy(c112, c112 + 4, f.post_consts.c112);
    std::copy(c113, c113 + 4, f.post_consts.c113);
    f.post_consts.flags[kPostFlagNoise] = 1;
    f.post_consts.flags[kPostFlagNoiseMidtone] = 1;
    PostPlan plan{};
    // no map kept (captures from before): no grain, and nothing else on
    CHECK_FALSE(PlanPost(f, 0, plan));
    auto map = std::make_shared<Texture>();
    map->width = map->height = 4;
    map->rgba.assign(16, 0xff808080u);
    map->mips = {std::vector<uint32_t>(4, 0xff808080u), std::vector<uint32_t>(1, 0xff808080u)};
    f.noise_map = map;
    f.noise_sampler.filtered = 1;
    f.noise_sampler.mag_linear = f.noise_sampler.min_linear = 1;
    f.noise_sampler.mip = 1;
    f.noise_sampler.mip_max = 2;
    REQUIRE(PlanPost(f, 0, plan));
    CHECK(plan.composite.flags.x == (kPostNoise | kPostNoiseMidtone));
    CHECK(plan.noise == map.get());
    CHECK(Near(plan.composite.noise_seeds.x, c112[0]));
    CHECK(Near(plan.composite.noise_seeds.w, c112[3]));
    CHECK(Near(plan.composite.noise.z, c113[2]));
    CHECK(plan.composite.noise_tex.x == 4u);
    uint32_t packed[4];
    PackSampler(f.noise_sampler, 3, packed);
    CHECK(plan.composite.noise_sampler.x == packed[0]);
    CHECK(plan.composite.noise_sampler.z == packed[2]);
    // left out when asked, or by another effect alone; --post-only noise
    // keeps its midtone weight
    CHECK_FALSE(PlanPost(f, 0, plan, false));
    CHECK_FALSE(PlanPost(f, kPostXfm, plan));
    REQUIRE(PlanPost(f, kPostNoise, plan));
    CHECK(plan.composite.flags.x == (kPostNoise | kPostNoiseMidtone));
    // without the composite's flag, none
    f.post_consts.flags[kPostFlagNoise] = 0;
    CHECK_FALSE(PlanPost(f, 0, plan));

    // a world frame: on by the proc's fields, c113 from them, seeds of its
    // own that differ from frame to frame (or the stationary two)
    f.proc_cmds = 1;
    f.post.noise_base[0] = f.post.noise_base[1] = 2.5f;
    f.post.noise_top = 1.35914f;
    f.post.noise_intensity = 2.65f;
    f.post.noise_midtone = 1;
    CHECK_FALSE(PlanPost(f, 0, plan));  // no map
    f.post.noise_map = 0x2000;
    f.game_frame = 100;
    REQUIRE(PlanPost(f, 0, plan));
    CHECK(plan.composite.flags.x == (kPostNoise | kPostNoiseMidtone));
    CHECK(Near(plan.composite.noise.x, 2.5f));
    CHECK(Near(plan.composite.noise.z, 1.35914f));
    CHECK(Near(plan.composite.noise.w, 2.65f));
    const float4 seeds = plan.composite.noise_seeds;
    for (float v : {seeds.x, seeds.y, seeds.z, seeds.w}) {
        CHECK(v >= 0);
        CHECK(v < 1);
    }
    f.game_frame = 101;
    REQUIRE(PlanPost(f, 0, plan));
    CHECK(plan.composite.noise_seeds.x != seeds.x);
    f.post.noise_stationary = 1;
    f.post.noise_seeds[0] = 0.25f;
    f.post.noise_seeds[1] = 0.75f;
    REQUIRE(PlanPost(f, 0, plan));
    CHECK(plan.composite.noise_seeds.x == 0.25f);
    CHECK(plan.composite.noise_seeds.w == 0.75f);
    CHECK(plan.composite.noise.z == 1.0f);
}

TEST_CASE("the grain reads the noise map by its sampler at both taps") {
    // a flat scene at a midtone and a noise map with a known texel pattern,
    // read nearest at level 0 (so every tap is one texel): each pixel is the
    // model's noise term of its two texels
    const uint32_t w = 32, h = 18;
    const uint32_t grey = 85;  // L = 1/3
    std::vector<uint32_t> scene(size_t(w) * h, grey | grey << 8 | grey << 16);
    std::vector<float> depth(scene.size(), 0.0f);
    Texture map;
    map.width = map.height = 8;
    map.rgba.resize(64);
    for (uint32_t i = 0; i < 64; i++) {
        const uint32_t v = (i * 37 + 11) % 256;
        map.rgba[i] = v | v << 8 | v << 16 | 0xffu << 24;
    }
    TexSampler ts;
    ts.filtered = 1;
    ts.mip = 2;  // level 0 alone
    PostPlan plan{};
    plan.composite.flags = {kPostNoise | kPostNoiseMidtone, 0, 0, 0};
    plan.composite.noise_seeds = {0.1f, 0.2f, 0.3f, 0.4f};
    plan.composite.noise = {2.5f, 2.5f, 1.35914f, 2.65f};
    uint32_t packed[4];
    PackSampler(ts, 1, packed);
    plan.composite.noise_sampler = {packed[0], packed[1], packed[2], packed[3]};
    plan.composite.noise_tex = {8, 8, 0, 0};
    plan.noise = &map;
    std::vector<uint32_t> out;
    RunPost(plan, scene, depth, w, h, {}, {}, {}, out);
    REQUIRE(out.size() == scene.size());
    auto texel = [&](float u, float v) {
        const int x = int(std::floor((u - std::floor(u)) * 8));
        const int y = int(std::floor((v - std::floor(v)) * 8));
        return float(map.rgba[size_t(y) * 8 + x] & 0xff) / 255;
    };
    int changed = 0;
    for (uint32_t y = 0; y < h; y += 5) {
        for (uint32_t x = 0; x < w; x += 3) {
            const float u = (float(x) + 0.5f) / float(w), v = (float(y) + 0.5f) / float(h);
            const float t0 = texel((u + 0.1f) * 2.5f, (v + 0.2f) * 2.5f);
            const float t1 = texel((u + 0.3f) * 2.5f * 1.35914f, (v + 0.4f) * 2.5f * 1.35914f);
            Inputs in{};
            in.c113[3] = 2.65f;
            for (int k = 0; k < 3; k++) {
                in.n0[k] = t0;
                in.n1[k] = t1;
            }
            float rgb[3] = {float(grey) / 255, float(grey) / 255, float(grey) / 255};
            NoiseModel(in, true, rgb);
            const float got = float(out[size_t(y) * w + x] & 0xff) / 255;
            CHECK(Near(got, Sat(rgb[0]), 0.6f / 255));
            changed += (out[size_t(y) * w + x] & 0xff) != grey;
        }
    }
    CHECK(changed > 0);
}

// check_trails.py's model of the game's BLENDPREVIOUS term (variants 30,
// 200030 and 802000200024 of its shader cache, run in xsim): the previous
// post frame faded by c125.y, its mean scored against the threshold c125.x
// (or gated by the previous alpha), kept where it beats the colour's mean
void ModelTrails(const float c125[4], const float cur[3], const float prev[4], float out[4]) {
    float d[3];
    for (int k = 0; k < 3; k++) d[k] = Sat(prev[k] - c125[1]);
    const float m = (d[0] + d[1] + d[2]) * c125[2];
    const float score = m > c125[0] ? m : prev[3] * m;
    const bool win = score > (cur[0] + cur[1] + cur[2]) * c125[2];
    for (int k = 0; k < 3; k++) out[k] = Sat(win ? d[k] : cur[k]);
    out[3] = win ? 1.0f : 0.0f;
}

TEST_CASE("the trails keep the faded previous frame where it's brighter, as the game's shader") {
    uint32_t s = 7;
    auto next = [&](float lo, float hi) {
        s = s * 1664525u + 1013904223u;
        return lo + (hi - lo) * float(s >> 8) / float(1u << 24);
    };
    for (int trial = 0; trial < 300; trial++) {
        CAPTURE(trial);
        // check_trails.py's ranges; the colour unsaturated, as after the
        // colour matrix
        const float c125[4] = {next(0, 1), next(0, 0.3f), trial % 2 ? 1.0f / 3 : next(0.1f, 1), 0};
        const float cur[3] = {next(-0.5f, 1.5f), next(-0.5f, 1.5f), next(-0.5f, 1.5f)};
        const float prev[4] = {next(0, 1), next(0, 1), next(0, 1), trial % 3 ? next(0, 1) : 1.0f};
        PostPass p{};
        p.trails = {c125[0], c125[1], c125[2], c125[3]};
        float want[4], got[4];
        ModelTrails(c125, cur, prev, want);
        TrailsCpu(p, cur, prev, got);
        for (int k = 0; k < 4; k++) CHECK(Near(got[k], want[k], 1e-6f));
    }
    // the music-video venues' most common (0.9999, a sixth or more a
    // frame): no trail ever starts, even from white
    PostPass p{};
    p.trails = {0.9999f, 1.0f / 6, 1.0f / 3, 0};
    const float white[4] = {1, 1, 1, 0}, dark[3] = {0.1f, 0.1f, 0.1f};
    float got[4];
    TrailsCpu(p, dark, white, got);
    CHECK(got[3] == 0.0f);
    CHECK(Near(got[0], 0.1f));
    // but one that was kept (alpha 1) goes on until it fades under the colour
    const float kept[4] = {1, 1, 1, 1};
    TrailsCpu(p, dark, kept, got);
    CHECK(got[3] == 1.0f);
    CHECK(Near(got[0], 1 - 1.0f / 6));
}

TEST_CASE("PlanPost turns the trails on where the game's composite had them") {
    FrameCapture f;
    f.post.valid = 1;
    f.post.proc = 0x1000;
    f.post_boundary = 0;
    f.proc_cmds = 7;
    f.post_consts.valid = 1;
    // song-video (render_screens_song.b3t): c125 and +0x2F as captured
    const float c125[4] = {0.9999f, 0.382057f, 0.333333f, 0};
    std::copy(c125, c125 + 4, f.post_consts.c125);
    PostPlan plan{};
    CHECK_FALSE(PlanPost(f, 0, plan));
    f.post_consts.flags[kPostFlagBlendPrevious] = 1;
    REQUIRE(PlanPost(f, 0, plan));
    CHECK(plan.composite.flags.x == kPostTrails);
    CHECK(plan.trails_update);
    CHECK(Near(plan.composite.trails.y, c125[1]));
    // a world frame: by the proc's threshold and duration, a post frame's
    // time at its emulated rate, and not one the game keeps
    f.proc_cmds = 1;
    f.post.emulate_fps = 30;
    CHECK_FALSE(PlanPost(f, 0, plan));
    f.post.trail_threshold = 0;
    f.post.trail_duration = 0.4f;
    REQUIRE(PlanPost(f, 0, plan));
    CHECK(plan.composite.flags.x == kPostTrails);
    CHECK_FALSE(plan.trails_update);
    CHECK(Near(plan.composite.trails.y, 1.0f / 30 / 0.4f));
    CHECK(Near(plan.composite.trails.z, 1.0f / 3));
    f.post.trail_threshold = 1;
    CHECK_FALSE(PlanPost(f, 0, plan));
}

TEST_CASE("the trails read the post frame before, which a frame drawn alone hasn't") {
    // video_trails' (0, fade 0.1 a frame): a white frame, then a dark one,
    // which keeps the white faded; without the history (a capture) it's
    // the dark frame alone
    const uint32_t w = 8, h = 4;
    std::vector<uint32_t> white(size_t(w) * h, 0xffffffffu), dark(size_t(w) * h, 0x00202020u);
    std::vector<float> depth(white.size(), 0.0f);
    PostPlan plan{};
    plan.composite.flags = {kPostTrails, 0, 0, 0};
    plan.composite.trails = {0, 0.1f, 1.0f / 3, 0};
    plan.trails_update = true;
    PostHistory history;
    std::vector<uint32_t> out;
    RunPost(plan, white, depth, w, h, {}, {}, {}, out, nullptr, &history, 10);
    CHECK((out[0] & 0xffffffu) == 0xffffffu);
    REQUIRE(history.game_frame == 10u);
    CHECK(history.rgba[0] == 0xffffffffu);  // the scene's alpha, 1
    RunPost(plan, dark, depth, w, h, {}, {}, {}, out, nullptr, &history, 11);
    const uint32_t faded = uint32_t(0.9f * 255 + 0.5f);
    CHECK((out[0] & 0xff) == faded);
    CHECK((history.rgba[0] >> 24) == 0xffu);  // kept: alpha 1
    CHECK(history.game_frame == 11u);
    // drawn again, the same frame isn't kept twice, and has no earlier one
    // to read (the history is its own): the dark frame
    std::vector<uint32_t> again;
    RunPost(plan, dark, depth, w, h, {}, {}, {}, again, nullptr, &history, 11);
    CHECK((again[0] & 0xff) == 0x20u);
    CHECK(history.game_frame == 11u);
    // no history: the dark frame
    RunPost(plan, dark, depth, w, h, {}, {}, {}, out);
    CHECK((out[0] & 0xffffffu) == 0x202020u);
}

namespace {

// A camera for the motion blur's tests: at the origin, looking down +z (the
// view depth is z), its picture kTx wide and kTy tall per unit of depth
// either side of the centre, near 10 and far 10000 in the z range 0.1..1 (c89
// as captured: 10, 10000, 1/0.9, 0.1/0.9); the previous frame's the same
// moved by (sx, sy, 0). As RndVelocityBuffer keeps them: Milo's row-vector
// matrices (clip = [x y z 1] M) and the frustum's eye and corner rays to the
// far plane, in DrawRectDepth's order (top left, bottom left, top right,
// bottom right).
constexpr float kTx = 0.8f, kTy = 0.45f, kNear = 10, kFar = 10000;

FrameCapture MovingCamera(float sx, float sy) {
    FrameCapture f;
    f.post.valid = 1;
    f.post.proc = 0x1000;
    f.post_boundary = 0;
    f.proc_cmds = 7;
    f.post_consts.valid = 1;
    f.post_consts.flags[kPostFlagVelocity] = 1;
    for (float& v : f.post_consts.c122) v = 1.25f;
    PostParams& p = f.post;
    p.vel_read = p.vel_on = p.vel_pre_depth = p.vel_same_cam = 1;
    p.vel_frame = 5;
    p.vel_scale = 0.75f;
    const float a = kFar / (kFar - kNear), b = -kNear * kFar / (kFar - kNear);
    float m[4][4] = {{1 / kTx, 0, 0, 0}, {0, 1 / kTy, 0, 0}, {0, 0, a, 1}, {0, 0, b, 0}};
    std::memcpy(p.vel_view_proj, m, sizeof(m));
    // the previous camera at (sx, sy): x - sx, y - sy before the projection
    m[3][0] = -sx / kTx;
    m[3][1] = -sy / kTy;
    std::memcpy(p.vel_prev_view_proj, m, sizeof(m));
    const float d[4] = {kNear, kFar, 1 / 0.9f, 0.1f / 0.9f};
    std::copy(d, d + 4, p.vel_depth_range);
    const float corners[4][2] = {{-1, 1}, {-1, -1}, {1, 1}, {1, -1}};
    for (int i = 0; i < 4; i++) {
        p.vel_corners[i][0] = corners[i][0] * kTx * kFar;
        p.vel_corners[i][1] = corners[i][1] * kTy * kFar;
        p.vel_corners[i][2] = kFar;
    }
    return f;
}

// what the picture moved at view depth w: prev uv - uv, from the geometry
// (the eye moved by s: a point at depth w is s / (t w) the other way in NDC,
// half that in uv; uv's y is down)
float MovedU(float sx, float w) { return -0.5f * sx / (kTx * w); }
float MovedV(float sy, float w) { return 0.5f * sy / (kTy * w); }

// a scene that's a function of uv, so each tap's place shows
void UvScene(const float at[2], float out[4]) {
    out[0] = at[0];
    out[1] = at[1];
    out[2] = at[0] * at[1];
    out[3] = 0.25f + at[0] * at[0];
}

}  // namespace

TEST_CASE("the velocity pass is the game's 8CDB397D: how far the picture moved, clamped") {
    // check_velocity.py checks the shader against its model in xsim; this
    // checks that model here against the geometry it stands for
    PostPlan plan{};
    for (const float s : {0.0f, 3.0f, -40.0f, 400.0f}) {
        const FrameCapture f = MovingCamera(s, s / 2);
        REQUIRE(PlanPost(f, 0, plan));
        CHECK(plan.composite.flags.x == kPostVelocity);
        CHECK(Near(plan.composite.vel_depth.y, 1.25f));  // c122.x
        // where nothing drew, the depth texture's 0: the far plane
        CHECK(Near(plan.composite.vel_depth.x, 1.0f));
        for (const float w : {50.0f, 300.0f, 2500.0f, 0.0f}) {
            for (const float u : {0.1f, 0.5f, 0.83f}) {
                const float uv[2] = {u, 1 - u * 0.7f};
                float t[4];
                VelocityTexelCpu(plan.composite, uv, w > 0 ? 1 / w : 0.0f, t);
                const float at = w > 0 ? w : kFar;
                const float du = std::clamp(MovedU(s, at), -0.02f, 0.02f);
                const float dv = std::clamp(MovedV(s / 2, at), -0.02f, 0.02f);
                CHECK(Near(t[0], du * 25 + 0.5f, 2e-5f));
                CHECK(Near(t[1], dv * 25 + 0.5f, 2e-5f));
                CHECK(Near(t[2], std::sqrt(du * du + dv * dv) * 1.7677668f, 2e-5f));
                CHECK(t[3] == 0.0f);
            }
        }
    }
}

TEST_CASE("the composite's velocity blur is the game's 140762E9: 11 taps along the motion") {
    // check_velocity.py's blurred_scene, which matches 140762E9 and every
    // dumped composite with c122 in xsim
    const float g0 = 0.1994711458683014f;
    const float gk[10] = {0.00876415055245161f, 0.02699548378586769f, 0.06475879997015f,
                          0.12098535895347595f, 0.1760326623916626f,  0.1760326623916626f,
                          0.12098535895347595f, 0.06475879997015f,    0.02699548378586769f,
                          0.00876415055245161f};
    const float norm = 1.0054858922958374f;
    float sum = g0;
    for (float g : gk) sum += g;
    CHECK(Near(sum * norm, 1.0f, 1e-5f));  // a flat picture stays flat

    PostPass pass{};
    pass.flags.x = kPostVelocity;
    pass.vel_depth = {0, 1.4f, 0, 0};
    const float uv[2] = {0.4f, 0.6f};
    const float centre[4] = {0.9f, 0.8f, 0.7f, 0.6f};
    for (const float d : {0.0005f, 0.0019f, 0.012f, -0.02f}) {
        const float vel[4] = {d * 25 + 0.5f, -d * 12.5f + 0.5f,
                              std::sqrt(d * d * 1.25f) * 1.7677668f, 0};
        float got[4];
        VelocityBlurCpu(pass, uv, vel, centre, UvScene, got);
        if (!(vel[2] >= 0.003f)) {
            for (int c = 0; c < 4; c++) CHECK(got[c] == centre[c]);  // under the mask
            continue;
        }
        const float step[2] = {0.0046f * (vel[0] - 0.5f) * 1.4f,
                               0.0046f * (vel[1] - 0.5f) * 1.4f};
        float want[4];
        for (int c = 0; c < 4; c++) want[c] = centre[c] * g0;
        for (int k = -5; k < 5; k++) {
            const float at[2] = {uv[0] + k * step[0], uv[1] + k * step[1]};
            float t[4];
            UvScene(at, t);
            for (int c = 0; c < 4; c++) want[c] += t[c] * gk[k + 5];
        }
        for (int c = 0; c < 4; c++) CHECK(Near(got[c], want[c] * norm, 1e-5f));
    }
    // off, it's the scene
    pass.flags.x = 0;
    const float vel[4] = {0.9f, 0.5f, 0.5f, 0};
    float got[4];
    VelocityBlurCpu(pass, uv, vel, centre, UvScene, got);
    CHECK(got[0] == centre[0]);
}

TEST_CASE("PlanPost turns the motion blur on where the game's had it and the capture has its cameras") {
    FrameCapture f = MovingCamera(5, 0);
    PostPlan plan{};
    REQUIRE(PlanPost(f, 0, plan));
    CHECK(plan.composite.flags.x == kPostVelocity);
    // c134..c137 as Draw uploads the previous matrix: its columns
    CHECK(Near(plan.composite.vel_prev[0].x, 1 / kTx));
    CHECK(Near(plan.composite.vel_prev[0].w, -5 / kTx));
    CHECK(Near(plan.composite.vel_prev[3].z, 1));
    CHECK(Near(plan.composite.vel_near.w, kFar));
    // the corner rays: the top left's, then across and down
    CHECK(Near(plan.composite.vel_corner[0].x, -kTx * kFar, 1e-2f));
    CHECK(Near(plan.composite.vel_corner[1].x, 2 * kTx * kFar, 1e-2f));
    CHECK(Near(plan.composite.vel_corner[2].y, -2 * kTy * kFar, 1e-2f));
    CHECK_FALSE(PlanPost(f, 0, plan, true, false));  // left off
    REQUIRE(PlanPost(f, kPostVelocity, plan));
    CHECK(plan.composite.flags.x == kPostVelocity);
    // the composite didn't have it
    f.post_consts.flags[kPostFlagVelocity] = 0;
    CHECK_FALSE(PlanPost(f, 0, plan));
    f.post_consts.flags[kPostFlagVelocity] = 1;
    // a capture from before has no velocity buffer: none, as before
    f.post.vel_read = 0;
    CHECK_FALSE(PlanPost(f, 0, plan));
    f.post.vel_read = 1;
    // a world frame: where DoVelocity would turn it on, by the buffer's last
    // c122
    f.proc_cmds = 1;
    REQUIRE(PlanPost(f, 0, plan));
    CHECK(plan.composite.flags.x == kPostVelocity);
    CHECK(Near(plan.composite.vel_depth.y, 0.75f));
    f.post.vel_frame = 0;  // a shot's first frame: AdvanceFrame says not yet
    CHECK_FALSE(PlanPost(f, 0, plan));
    f.post.vel_frame = 5;
    f.post.vel_on = 0;  // the proc's motion_blur_velocity off
    CHECK_FALSE(PlanPost(f, 0, plan));
}

TEST_CASE("the motion blur leaves a still camera's picture be and smears a moving one's along it") {
    // a white column on black, everything at depth 100
    const uint32_t w = 128, h = 72;
    std::vector<uint32_t> scene(size_t(w) * h, 0xff000000u);
    for (uint32_t y = 0; y < h; y++) scene[size_t(y) * w + 64] = 0xffffffffu;
    std::vector<float> depth(scene.size(), 1.0f / 100);
    PostPlan plan{};
    std::vector<uint32_t> still, moved;
    REQUIRE(PlanPost(MovingCamera(0, 0), 0, plan));
    RunPost(plan, scene, depth, w, h, {}, {}, {}, still);
    CHECK(still == scene);  // no motion: under the mask, the scene as it was
    // the eye moved left by 4 since: the picture moved right by 0.025 uv,
    // clamped to 0.02 (2.6 px here); the taps reach 5 steps of 0.115 * 0.02
    // * c122 uv (1.8 px) either way
    REQUIRE(PlanPost(MovingCamera(-4, 0), 0, plan));
    RunPost(plan, scene, depth, w, h, {}, {}, {}, moved);
    const uint32_t row = 36 * w;
    CHECK((moved[row + 64] & 0xff) < 0xff);  // the column spread out
    CHECK((moved[row + 64] & 0xff) > 0x20);
    CHECK((moved[row + 63] & 0xff) > 0);
    CHECK((moved[row + 65] & 0xff) > 0);
    CHECK((moved[row + 60] & 0xff) == 0);  // no further than the taps reach
    CHECK((moved[row + 68] & 0xff) == 0);
    // along the motion only: every row the same
    for (uint32_t y = 1; y < h - 1; y++) CHECK(moved[size_t(y) * w + 63] == moved[row + 63]);
}

namespace {

// check_velocity.py's object_ps_model: the texel 39DE58D4 writes
void ModelObjectTexel(const float c8[4], const float cur[4], const float prev[4], float s9,
                      float out[4]) {
    const float cu = 0.5f + 0.5f * cur[0] / cur[3], cv = 0.5f - 0.5f * cur[1] / cur[3];
    const float pu = 0.5f + 0.5f * prev[0] / prev[3], pv = 0.5f - 0.5f * prev[1] / prev[3];
    const float z = (1 - s9) * c8[2] - c8[3];
    const float w = c8[0] * c8[1] / (c8[1] - z * (c8[1] - c8[0]));
    const float dx = std::clamp(pu - cu, -0.02f, 0.02f), dy = std::clamp(pv - cv, -0.02f, 0.02f);
    out[0] = dx * 25 + 0.5f;
    out[1] = dy * 25 + 0.5f;
    out[2] = std::sqrt(dx * dx + dy * dy) * 1.7677668f;
    out[3] = cur[3] > w + 1 ? 0.0f : 1.0f;
}

// what the game's depth texture holds at view depth w, through c8 (s9 = 1 - z)
float GameS9(const float c8[4], float w) {
    const float z = (c8[1] - c8[1] * c8[0] / w) / (c8[1] - c8[0]);
    return 1 - (z + c8[3]) / c8[2];
}

// a quad facing the camera at depth z, x0..x1 across and y0..y1 up
std::shared_ptr<Geometry> Quad(float x0, float x1, float y0, float y1, float z) {
    auto g = std::make_shared<Geometry>();
    g->verts.resize(4);
    const float corners[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    for (int i = 0; i < 4; i++) {
        g->verts[i].pos[0] = corners[i][0];
        g->verts[i].pos[1] = corners[i][1];
        g->verts[i].pos[2] = z;
    }
    g->indices = {0, 1, 2, 0, 2, 3};
    return g;
}

// MovingCamera's frame with one rigid object: `geom` placed as it is this
// frame and moved by (shift, 0, 0) the last (palette rows of a translation),
// through the frame's two view-projections (VS c0..c7: the matrices'
// columns) and its depth range
VelocityObject RigidObject(const FrameCapture& f, std::shared_ptr<const Geometry> geom,
                           float shift) {
    VelocityObject o;
    o.geom = std::move(geom);
    o.mesh = 0x5000;
    o.bones = 1;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            o.view_proj[r][c] = f.post.vel_view_proj[c][r];
            o.view_proj[4 + r][c] = f.post.vel_prev_view_proj[c][r];
        }
        o.depth_range[r] = f.post.vel_depth_range[r];
    }
    o.rows = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, shift, 0, 1, 0, 0, 0, 0, 1, 0};
    return o;
}

}  // namespace

TEST_CASE("the object pass's texel is the game's 39DE58D4: its motion, where it's in front") {
    const float c8[4] = {kNear, kFar, 1 / 0.9f, 0.1f / 0.9f};
    VelocityObjectPass pass{};
    pass.depth_range = {c8[0], c8[1], c8[2], c8[3]};
    const float cur[4] = {100, 50, 20, 200};
    const float prevs[3][4] = {{100, 50, 20, 200}, {110, 40, 25, 210}, {-400, 300, 30, 190}};
    for (const auto& prev : prevs) {
        // the scene in front (150), just behind (199.5: within the shader's
        // 1), behind, and nothing drawn (the far plane)
        for (const float w : {150.0f, 199.5f, 400.0f, 0.0f}) {
            float got[4], want[4];
            VelocityObjectTexelCpu(pass, cur, prev, w > 0 ? 1 / w : 0.0f, got);
            ModelObjectTexel(c8, cur, prev, w > 0 ? GameS9(c8, w) : 0.0f, want);
            for (int c = 0; c < 4; c++) CHECK(Near(got[c], want[c], 2e-4f));
            CHECK(got[3] == (w == 150.0f ? 0.0f : 1.0f));
        }
    }
}

TEST_CASE("the object pass places a vertex as the game's 21A0C657 and F922317D") {
    // two palette entries, this frame's translating by (10 b + 10, 0, 0) and
    // the last frame's by (0, 5 b + 5, 0); weights x, y, z and 1 - their sum
    VelocityObject o;
    o.bones = 2;
    o.skinned = 1;
    auto translate = [](float x, float y) {
        return std::vector<float>{1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, 0};
    };
    for (int last = 0; last < 2; last++)
        for (int b = 0; b < 2; b++) {
            const auto r = last ? translate(0, 5.0f * b + 5) : translate(10.0f * b + 10, 0);
            o.rows.insert(o.rows.end(), r.begin(), r.end());
        }
    VelocityObjectPass pass{};
    // c0..c3 the identity, c4..c7 doubling x
    for (int r = 0; r < 4; r++) {
        pass.view_proj[r] = {r == 0 ? 1.0f : 0, r == 1 ? 1.0f : 0, r == 2 ? 1.0f : 0,
                             r == 3 ? 1.0f : 0};
        pass.view_proj[4 + r] = pass.view_proj[r];
    }
    pass.view_proj[4].x = 2;
    Vertex v{};
    v.pos[0] = 1;
    v.pos[1] = 2;
    v.pos[2] = 3;
    const uint8_t bones[4] = {1, 0, 1, 0};
    const float weights[4] = {0.25f, 0.5f, 0.1f, 0.9f};  // the fourth isn't read
    std::copy(bones, bones + 4, v.bone);
    std::copy(weights, weights + 4, v.weight);
    float cur[4], prev[4];
    VelocityObjectVertexCpu(o, pass, v, cur, prev);
    // weights by bone: 1 has 0.25 + 0.1, 0 has 0.5 + 0.15
    CHECK(Near(cur[0], 1 + 0.35f * 20 + 0.65f * 10));
    CHECK(Near(cur[1], 2));
    CHECK(Near(cur[3], 1));
    CHECK(Near(prev[0], 2));
    CHECK(Near(prev[1], 2 + 0.35f * 10 + 0.65f * 5));
    // unskinned: the palette's first entry, whatever the weights
    o.skinned = 0;
    o.bones = 1;
    o.rows = translate(7, 0);
    const auto last = translate(0, 3);
    o.rows.insert(o.rows.end(), last.begin(), last.end());
    VelocityObjectVertexCpu(o, pass, v, cur, prev);
    CHECK(Near(cur[0], 8));
    CHECK(Near(prev[0], 2));
    CHECK(Near(prev[1], 5));
}

TEST_CASE("the object pass keeps what moves with the camera sharp, where it's in front") {
    // white columns on black: one at x 32, where a quad at depth 50 covers
    // the left half, one at x 96, over the background at depth 100
    const uint32_t w = 128, h = 72;
    std::vector<uint32_t> scene(size_t(w) * h, 0xff000000u);
    std::vector<float> depth(scene.size(), 1.0f / 100);
    for (uint32_t y = 0; y < h; y++) {
        scene[size_t(y) * w + 32] = scene[size_t(y) * w + 96] = 0xffffffffu;
        for (uint32_t x = 0; x < 64; x++) depth[size_t(y) * w + x] = 1.0f / 50;
    }
    const uint32_t row = 36 * w;
    auto run = [&](const FrameCapture& f) {
        PostPlan plan{};
        REQUIRE(PlanPost(f, 0, plan));
        std::vector<uint32_t> out;
        RunPost(plan, scene, depth, w, h, {}, {}, {}, out);
        return out;
    };
    // the eye moved left by 4: without the object both columns smear
    FrameCapture f = MovingCamera(-4, 0);
    std::vector<uint32_t> out = run(f);
    CHECK((out[row + 31] & 0xff) > 0);
    CHECK((out[row + 97] & 0xff) > 0);
    // the quad moved with the eye (it was 4 left of here too): its pixels
    // keep still, the background still smears
    const auto quad = Quad(-45, 0.2f, -30, 30, 50);
    f.velocity_objects.push_back(RigidObject(f, quad, -4));
    out = run(f);
    CHECK((out[row + 31] & 0xff) == 0);
    CHECK((out[row + 32] & 0xff) == 0xff);
    CHECK((out[row + 33] & 0xff) == 0);
    CHECK((out[row + 97] & 0xff) > 0);
    // behind the scene (the shader's own test against the pre-pass depth),
    // it leaves the camera's motion
    f.velocity_objects[0] = RigidObject(f, Quad(-120, 0.4f, -60, 60, 120), -4);
    out = run(f);
    CHECK((out[row + 31] & 0xff) > 0);
    // a capture from before has none: the camera's alone
    f.velocity_objects.clear();
    CHECK(run(f) == run(MovingCamera(-4, 0)));
}
