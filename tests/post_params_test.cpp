// Checks src/Render/post_params.h: what the native view works out from RB3's
// post-processing parameters, against what the game's code does with them:
// whether the colour matrix is on (RndPostProc::ColorXfmEnabled), the matrix
// as the composite reads it, c24 as NgDOFProc::DoPost sets it, c6 as DoBloom
// does, and the matrix rebuilt from hue..levels (RndColorXfm::AdjustColorXfm).
// The last cases are numbers captured from the game (render_song_evenodd.b3t's
// 25s and render_song.b3t's 10s, kept in out/m4), which they have to give back.

#include <doctest/doctest.h>
#include <cmath>
#include <cstring>
#include "src/Render/post_params.h"

using namespace band3::render;

namespace {

bool Near(float a, float b, float tolerance = 1e-5f) { return std::fabs(a - b) <= tolerance; }

}  // namespace

TEST_CASE("a post proc at its defaults leaves the colour alone") {
    PostParams p;
    CHECK_FALSE(ColorXfmEnabled(p));
    float m[3][3], v[3];
    AdjustColorXfm(p, m, v);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) CHECK(Near(m[i][j], i == j ? 1.0f : 0.0f));
        CHECK(Near(v[i], 0));
    }
}

TEST_CASE("flicker turns the colour matrix on and scales its 3x3 only") {
    PostParams p;
    p.xfm[0][1] = 0.25f;  // red into green
    p.xfm_offset[2] = 0.1f;
    p.color_mod = 0.5f;
    CHECK(ColorXfmEnabled(p));
    float rows[3][4];
    ModulatedXfm(p, rows);
    // green's row reads red: M's column 1, times the modulation
    CHECK(Near(rows[1][0], 0.125f));
    CHECK(Near(rows[1][1], 0.5f));
    CHECK(Near(rows[0][0], 0.5f));
    CHECK(Near(rows[0][1], 0));
    // the translation isn't scaled
    CHECK(Near(rows[2][3], 0.1f));
    CHECK(Near(rows[0][3], 0));
}

TEST_CASE("levels count as changed by their 8-bit colours") {
    PostParams p;
    p.level_in_lo[0] = 0.003f;  // 0.77 of 255: still 0
    p.level_in_hi[2] = 0.999f;  // 254.7: 254
    CHECK(PackColor(p.level_in_lo) == 0);
    CHECK(PackColor(p.level_in_hi) == 0xfeffff);
    CHECK(ColorXfmEnabled(p));
    p.level_in_hi[2] = 1;
    CHECK_FALSE(ColorXfmEnabled(p));
    // the alpha doesn't count
    p.level_out_hi[3] = 0;
    CHECK_FALSE(ColorXfmEnabled(p));
    p.level_out_lo[1] = 0.5f;
    CHECK(ColorXfmEnabled(p));
}

TEST_CASE("the colour matrix is rebuilt as AdjustColorXfm builds it") {
    PostParams p;
    // full desaturation: every channel the mean of the three
    p.saturation = -100;
    float m[3][3], v[3];
    AdjustColorXfm(p, m, v);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) CHECK(Near(m[i][j], 1.0f / 3.0f, 1e-6f));

    // hue 60: each channel cos 45 of itself, sin 45 of the next (red's
    // output takes green), then brightness 50 adds a quarter
    p = PostParams{};
    p.hue = 60;
    p.brightness = 50;
    AdjustColorXfm(p, m, v);
    const float h = std::sqrt(0.5f);
    CHECK(Near(m[0][0], h));
    CHECK(Near(m[1][0], h));
    CHECK(Near(m[2][0], 0));
    CHECK(Near(v[0], 0.25f));

    // levels, after the rest: in 0.2..0.6 to out 0.1..0.9
    p = PostParams{};
    for (int i = 0; i < 3; i++) {
        p.level_in_lo[i] = 0.2f;
        p.level_in_hi[i] = 0.6f;
        p.level_out_lo[i] = 0.1f;
        p.level_out_hi[i] = 0.9f;
    }
    p.lightness = -50;  // halves the colour first
    AdjustColorXfm(p, m, v);
    CHECK(Near(m[1][1], 0.5f * 2.0f));
    CHECK(Near(v[1], 0.1f - 0.2f * 2.0f));
}

TEST_CASE("c24 and c6 are recomputed as the game sets them") {
    PostParams p;
    p.dof_scale = 0.9f;
    p.dof_bias = 0.8f;
    p.dof_min_blur = 0.3f;
    p.dof_max_blur = 0.2f;
    float c24[4];
    DofConstants(p, c24);
    CHECK(Near(c24[0], 10.0f, 1e-4f));
    CHECK(Near(c24[1], -9.0f, 1e-4f));
    CHECK(Near(c24[2], 0.2f));  // min(max, min)
    CHECK(Near(c24[3], 0.2f));
    p.dof_max_blur = -1;
    DofConstants(p, c24);
    CHECK(Near(c24[3], 1.0f));

    p.bloom_color[0] = 0.5f;
    p.bloom_color[1] = 1.0f;
    p.bloom_color[2] = 0.25f;
    p.bloom_intensity = 2.0f;
    float c6[4];
    BloomConstant(p, c6);
    CHECK(Near(c6[0], 1.0f));
    CHECK(Near(c6[1], 2.0f));
    CHECK(Near(c6[2], 0.5f));
    CHECK(Near(c6[3], 0));
}

namespace {

// a capture's numbers, as the game had them: its proc's parameters and the
// matrix it built, and the composite's c92..c94 and c24
struct CapturedPost {
    float hsl[5];  // hue, saturation, lightness, contrast, brightness
    float levels[4][3];  // in lo, in hi, out lo, out hi (rgb)
    float xfm[3][3], offset[3], mod;
    float dof[6];  // scale, bias, focal, blur depth, min, max
    float c92[3][4];
    float c24[4];
};

void Check(const CapturedPost& c) {
    PostParams p;
    p.hue = c.hsl[0];
    p.saturation = c.hsl[1];
    p.lightness = c.hsl[2];
    p.contrast = c.hsl[3];
    p.brightness = c.hsl[4];
    float* levels[] = {p.level_in_lo, p.level_in_hi, p.level_out_lo, p.level_out_hi};
    for (int l = 0; l < 4; l++)
        for (int i = 0; i < 3; i++) levels[l][i] = c.levels[l][i];
    float m[3][3], v[3];
    AdjustColorXfm(p, m, v);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) CHECK(Near(m[i][j], c.xfm[i][j], 2e-6f));
        CHECK(Near(v[i], c.offset[i], 2e-6f));
    }
    std::memcpy(p.xfm, c.xfm, sizeof(p.xfm));
    std::memcpy(p.xfm_offset, c.offset, sizeof(p.xfm_offset));
    p.color_mod = c.mod;
    CHECK(ColorXfmEnabled(p));
    float rows[3][4];
    ModulatedXfm(p, rows);
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 4; k++) CHECK(Near(rows[r][k], c.c92[r][k], 1e-6f));
    p.dof_scale = c.dof[0];
    p.dof_bias = c.dof[1];
    p.dof_focal = c.dof[2];
    p.dof_blur_depth = c.dof[3];
    p.dof_min_blur = c.dof[4];
    p.dof_max_blur = c.dof[5];
    float c24[4];
    DofConstants(p, c24);
    for (int k = 0; k < 4; k++) CHECK(Near(c24[k], c.c24[k], 1e-4f));
}

}  // namespace

TEST_CASE("the game's own numbers come back: the B&W shot at 25s with even/odd rendering") {
    // out/m4/evenon/render-song-eo-25s.cap: desaturated, lifted and
    // contrasted, levels tinting; flicker barely on
    Check({{0, -100, 9.69859886f, 37.1560783f, 14.5478983f},
           {{0.102691047f, 0.102691047f, 0.102691047f},
            {0.581629097f, 0.581629097f, 0.581629097f},
            {0.185516775f, 0.185516775f, 0.185516775f},
            {0.99881804f, 0.99881804f, 1}},
           {{0.80961895f, 0.809618831f, 0.810795426f},
            {0.809618831f, 0.80961895f, 0.810795426f},
            {0.809618831f, 0.809618831f, 0.810795605f}},
           {-0.100273341f, -0.100273341f, -0.100688681f},
           0.999721408f,
           {0.920664608f, 0.87746048f, 112.280983f, 0.349999994f, 0, 1},
           {{0.809393406f, 0.809393287f, 0.809393287f, -0.100273341f},
            {0.809393287f, 0.809393406f, 0.809393287f, -0.100273341f},
            {0.810569525f, 0.810569525f, 0.810569704f, -0.100688681f}},
           {23.145937f, -21.3096447f, 0, 1}});
}

TEST_CASE("the game's own numbers come back: the flickering grey shot at 10s") {
    // out/m4/evenoff/render-song-10s.cap: fully desaturated, a little
    // contrast, levels tinting, and the flicker's modulation (0.91) on the
    // matrix but not its offset
    Check({{0, -100, 0, 10, 0},
           {{0, 0, 0}, {1, 1, 1}, {0.0980392173f, 0.0980392173f, 0.0980392173f},
            {0.960784316f, 0.960784316f, 1}},
           {{0.319258094f, 0.319258034f, 0.333769768f},
            {0.319258034f, 0.319258094f, 0.333769768f},
            {0.319258034f, 0.319258034f, 0.333769828f}},
           {0.0505246595f, 0.0505246595f, 0.0483649075f},
           0.907554626f,
           {0.825270951f, 0.730701029f, 51.295414f, 0.349999994f, 0, 1},
           {{0.289744169f, 0.289744109f, 0.289744109f, 0.0505246595f},
            {0.289744109f, 0.289744169f, 0.289744109f, 0.0505246595f},
            {0.302914292f, 0.302914292f, 0.302914351f, 0.0483649075f}},
           {10.5741863f, -8.72656918f, 0, 1}});
}
