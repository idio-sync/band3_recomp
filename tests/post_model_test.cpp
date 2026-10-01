// Checks src/Render/post_model.cpp (which runs src/Render/shaders/
// post_model.hlsli on the CPU, as post.hlsl does on the GPU): the blurs' taps
// against what the game gave its shaders, the native depth read as the game's
// depth texture against the c24 the game's composite drew with, the
// composite against the models of the game's composite shaders that
// out/research/m4_shaders/check_post.py checked in an interpreter, and the
// passes together on a plain picture. Captured numbers are from
// render_song.b3t's 10s and render_song_evenodd.b3t's 25s (kept in out/m4).

#include <doctest/doctest.h>
#include <cmath>
#include <cstdint>
#include <vector>
#include "src/Render/post_model.h"

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
// native composite stands for (without soft particles and the overlay, which
// it leaves out: their samplers read 0 here)
struct Inputs {
    float scene[4], dof[4], depth, l0[3], l1[3], l2[3], c6[3], c24[4], rows[3][4];
};

// 63306D35: DOF, bloom (screen), colour matrix
void Model63306D35(const Inputs& in, float out[3]) {
    const float a = ModelDofAmount(in.c24, in.depth);
    float rgb[3];
    for (int k = 0; k < 3; k++) {
        rgb[k] = in.scene[k] + (in.dof[k] - in.scene[k]) * a;
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

// deterministic inputs in check_post.py's ranges
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
    return in;
}

PostPass PassFor(const Inputs& in, uint32_t flags) {
    PostPass p{};
    p.flags = {flags, 0, 0, 0};
    p.c6 = {in.c6[0], in.c6[1], in.c6[2], 0};
    p.c24 = {in.c24[0], in.c24[1], in.c24[2], in.c24[3]};
    for (int j = 0; j < 3; j++)
        p.xfm[j] = {in.rows[j][0], in.rows[j][1], in.rows[j][2], in.rows[j][3]};
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
        {"63306D35 DOF bloom xfm", kPostDof | kPostBloom | kPostXfm, Model63306D35},
        {"C91275BB DOF", kPostDof, ModelC91275BB},
        {"2F002AB2 bloom", kPostBloom, Model2F002AB2},
        {"C6A009EA glare", kPostGlare, ModelC6A009EA},
        {"0F105E2D DOF glare xfm", kPostDof | kPostGlare | kPostXfm, Model0F105E2D},
    };
    for (const Variant& v : variants) {
        CAPTURE(v.name);
        for (uint32_t seed = 0; seed < 30; seed++) {
            const Inputs in = MakeInputs(seed);
            float want[3], got[3];
            v.model(in, want);
            CompositeCpu(PassFor(in, v.flags), in.scene, in.dof, in.depth, in.l0, in.l1, in.l2,
                         got);
            for (int k = 0; k < 3; k++) CHECK(Near(got[k], want[k], 1e-5f));
        }
    }
}

TEST_CASE("PlanPost takes the composite's constants, or works them out") {
    FrameCapture f;
    PostPlan plan{};
    CHECK_FALSE(PlanPost(f, 0, plan));  // no DoPostProcess read

    f.post.valid = 1;
    f.post.proc = 0x1000;
    f.post.cam_near = 10;
    f.post.cam_far = 10000;
    f.post.cam_zrange[0] = 0.1f;
    f.post.cam_zrange[1] = 1;
    // nothing on: the composite would be a copy
    CHECK_FALSE(PlanPost(f, 0, plan));

    // worked out from the parameters: glare bloom, DOF, flicker
    f.post.bloom_intensity = 2;
    f.post.bloom_glare = 1;
    f.post.dof = 0x2000;
    f.post.dof_enabled = 1;
    f.post.dof_scale = 0.9f;
    f.post.dof_bias = 0.8f;
    f.post.dof_max_blur = 1;
    f.post.color_mod = 0.5f;
    REQUIRE(PlanPost(f, 0, plan));
    CHECK(plan.composite.flags.x == (kPostGlare | kPostDof | kPostXfm));
    CHECK(Near(plan.composite.c6.x, 2));
    CHECK(Near(plan.composite.c24.x, 10, 1e-4f));
    CHECK(Near(plan.composite.xfm[0].x, 0.5f));
    CHECK(Near(plan.composite.camera.y, 10000));
    CHECK(Near(plan.bloom_taps[2][0][0].x, -6.5f / 20, 1e-7f));  // level 2, 20 across
    CHECK(Near(plan.bloom_taps[2][1][0].y, -6.5f / 11, 1e-7f));  // 11 down
    REQUIRE(PlanPost(f, kPostDof, plan));
    CHECK(plan.composite.flags.x == kPostDof);

    // the composite's own, when the capture has them: its flags win
    f.post_consts.valid = 1;
    f.post_consts.flags[kPostFlagBloom] = 1;
    f.post_consts.c6[0] = 0.25f;
    REQUIRE(PlanPost(f, 0, plan));
    CHECK(plan.composite.flags.x == kPostBloom);
    CHECK(Near(plan.composite.c6.x, 0.25f));

    // DOF without the camera's planes can't read the depth
    f.post_consts.flags[kPostFlagDof] = 1;
    f.post.cam_far = 0;
    REQUIRE(PlanPost(f, 0, plan));
    CHECK(plan.composite.flags.x == kPostBloom);

    f.post.disabled = 1;
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
    RunPost(plan, scene, depth, w, h, out);
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
