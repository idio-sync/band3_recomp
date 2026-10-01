// Checks src/Render/spot_model.cpp (shaders/spot_model.hlsli on the CPU), the
// spotlight cone's maths: each of the shader's cases for the view ray against
// the cone, against the stretch inside the cone found by stepping along the
// ray; the falloff against numeric integration; the cone's colour against
// its parts; the cross-section's coordinate and the scene depth; and
// PackSpot's registers. Then src/Render/soft_raster.cpp's spotlight passes:
// a cone into the depth volume as SpotCone says, 8-bit, added to the clear;
// the blur in place from its taps; and that they're drawn for --dump-rt
// after post_boundary but leave the picture alone.

#include <doctest/doctest.h>
#include <cmath>
#include <memory>
#include <vector>
#include "src/Render/soft_raster.h"
#include "src/Render/spot_model.h"

using namespace band3::render;
using namespace band3::render::spot;

namespace {

// A cone with its apex at (0, 0, 10) pointing down -z, 10 long, half angle
// 30 degrees; the camera at `eye` looking along +x
constexpr float kApex[3] = {0, 0, 10};
constexpr float kAxis[3] = {0, 0, -1};
constexpr float kLength = 10;
constexpr float kCos2 = 0.75f;  // cos^2 30
constexpr float kFar = 10000;

SpotParams Cone(const float eye[3], const float forward[3] = nullptr) {
    static constexpr float kX[3] = {1, 0, 0};
    const float* f = forward ? forward : kX;
    SpotParams sp{};
    sp.eye = {eye[0], eye[1], eye[2], 1};
    sp.apex = {kApex[0], kApex[1], kApex[2], 1.0f / kLength};
    sp.axis = {kAxis[0], kAxis[1], kAxis[2], kLength};
    sp.eye_apex = {eye[0] - kApex[0], eye[1] - kApex[1], eye[2] - kApex[2], 0};
    sp.cone = {0, 0, 0, kCos2};
    sp.forward = {f[0], f[1], f[2], -(f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2])};
    sp.depth_range = {10, kFar, 1.0f / (1 - 0), 0};
    sp.color = {8, 4, 2, 0};
    sp.fog = {0, 1 / kFar, 0, 0};
    return sp;
}

// whether x is inside the (real, not mirror) cone
bool Inside(const float x[3]) {
    float d[3], h = 0, len2 = 0;
    for (int i = 0; i < 3; i++) {
        d[i] = x[i] - kApex[i];
        h += d[i] * kAxis[i];
        len2 += d[i] * d[i];
    }
    return h > 0 && h * h >= kCos2 * len2;
}

// the stretch of eye + t dir, t in 0..tmax, inside the cone, by stepping
// along it; false if none is
bool Stepped(const float eye[3], const float dir[3], float tmax, float& tn, float& tf) {
    constexpr int kSteps = 200000;
    bool any = false;
    for (int k = 0; k <= kSteps; k++) {
        const float t = tmax * float(k) / kSteps;
        const float x[3] = {eye[0] + t * dir[0], eye[1] + t * dir[1], eye[2] + t * dir[2]};
        if (!Inside(x)) continue;
        if (!any) tn = t;
        tf = t;
        any = true;
    }
    return any;
}

struct RayCase {
    const char* name;
    float eye[3];
    float dir[3];
    float scene_depth;
    uint32_t kind;
};

// how far along the ray the proxy is: past everything that matters
constexpr float kProxy = 40;

}  // namespace

TEST_CASE("the cone's cases each find the stretch of the view ray inside the cone") {
    const RayCase cases[] = {
        // inside the cone (below its end: the formula's cone goes on), looking
        // up past the apex: from the eye to where it leaves by the side
        {"from the eye", {1, 0, -5}, {0, 0, 1}, kFar, kSpotRayFromEye},
        // across it, below the apex: in by one side, out by the other
        {"through", {-20, 0, 4}, {1, 0, 0.05f}, kFar, kSpotRayThrough},
        // from above the apex down into it: in, then on to the proxy
        {"to the proxy", {1, 0, 15}, {0, 0, -1}, kFar, kSpotRayToScene},
        // the same, stopped by the scene in front of the proxy
        {"to the scene", {1, 0, 15}, {0, 0, -1}, 20, kSpotRayToScene},
        // across the mirror cone above the apex: nothing
        {"mirror", {-20, 0, 15}, {1, 0, 0}, kFar, kSpotRayMirror},
        // past it: nothing
        {"miss", {-20, 50, 5}, {1, 0, 0}, kFar, kSpotRayMiss},
    };
    for (const RayCase& c : cases) {
        CAPTURE(c.name);
        float n = 0;
        for (float v : c.dir) n += v * v;
        n = std::sqrt(n);
        const float dir[3] = {c.dir[0] / n, c.dir[1] / n, c.dir[2] / n};
        const float p[3] = {c.eye[0] + kProxy * dir[0], c.eye[1] + kProxy * dir[1],
                            c.eye[2] + kProxy * dir[2]};
        const SpotParams sp = Cone(c.eye);
        const SpotRayCpu r = SpotRayOnCpu(sp, p, c.scene_depth);
        CHECK(r.kind == c.kind);
        for (int i = 0; i < 3; i++) CHECK(r.dir[i] == doctest::Approx(dir[i]).epsilon(1e-5));
        float tn = 0, tf = 0;
        const float tmax = std::min(c.scene_depth, kProxy);
        if (Stepped(c.eye, dir, tmax, tn, tf)) {
            CHECK(r.tn == doctest::Approx(tn).epsilon(1e-3));
            CHECK(r.tf == doctest::Approx(tf).epsilon(1e-3));
        } else {
            // empty: the cone adds nothing
            CHECK(r.tn == r.tf);
            float rgb[3];
            SpotConeCpu(sp, p, kProxy, c.scene_depth, 1, 0, rgb);
            for (float v : rgb) CHECK(v == 0);
        }
    }
}

TEST_CASE("the falloff is the mean of (1 - u)^2 over the stretch") {
    const float us[] = {0.0f, 0.1f, 0.35f, 0.5f, 0.77f, 0.999f, 1.0f};
    for (float un : us) {
        for (float uf : us) {
            CAPTURE(un);
            CAPTURE(uf);
            if (un == uf) {
                // the 360's 0 * 1/0
                CHECK(SpotFalloffCpu(un, uf) == 0);
                continue;
            }
            constexpr int kSteps = 100000;
            double sum = 0;
            for (int k = 0; k < kSteps; k++) {
                const double u = un + (uf - un) * (k + 0.5) / kSteps;
                sum += (1 - u) * (1 - u);
            }
            CHECK(SpotFalloffCpu(un, uf) == doctest::Approx(sum / kSteps).epsilon(1e-4));
        }
    }
    // nearly the same height: no cancellation
    CHECK(SpotFalloffCpu(0.5f, 0.5000001f) == doctest::Approx(0.25f).epsilon(1e-5));
}

TEST_CASE("the cone's colour is the stretch's depth times its falloff, 0.004 and the colour") {
    const float eye[3] = {-20, 0, 4};
    const float dir[3] = {0.9987523f, 0, 0.0499376f};  // (1, 0, 0.05) made unit
    const float p[3] = {eye[0] + kProxy * dir[0], eye[1], eye[2] + kProxy * dir[2]};
    SpotParams sp = Cone(eye);
    const SpotRayCpu r = SpotRayOnCpu(sp, p, kFar);
    REQUIRE(r.kind == kSpotRayThrough);
    // the mean of (1 - u)^2 along the stretch, by steps; its length in view
    // depth along x
    constexpr int kSteps = 100000;
    double sum = 0;
    for (int k = 0; k < kSteps; k++) {
        const double t = r.tn + (r.tf - r.tn) * (k + 0.5) / kSteps;
        const double h = (eye[2] + t * dir[2] - kApex[2]) * kAxis[2];
        const double u = std::clamp(h / kLength, 0.0, 1.0);
        sum += (1 - u) * (1 - u);
    }
    const double depth = (r.tf - r.tn) * dir[0];
    const double i = 0.004 * depth * sum / kSteps;
    float rgb[3];
    SpotConeCpu(sp, p, kProxy, kFar, 1, 0, rgb);
    CHECK(rgb[0] == doctest::Approx(8 * i).epsilon(1e-3));
    CHECK(rgb[1] == doctest::Approx(4 * i).epsilon(1e-3));
    CHECK(rgb[2] == doctest::Approx(2 * i).epsilon(1e-3));

    // the cross-section's profile scales it when c86.x says so
    sp.xsec = {1, 0, 0, 0};
    float half[3];
    SpotConeCpu(sp, p, kProxy, kFar, 0.5f, 0, half);
    CHECK(half[0] == doctest::Approx(rgb[0] * 0.5f).epsilon(1e-5));
    sp.xsec = {0, 0, 0, 0};
    SpotConeCpu(sp, p, kProxy, kFar, 0.5f, 0, half);
    CHECK(half[0] == doctest::Approx(rgb[0]).epsilon(1e-5));

    // the fog: with c127.zw 0, as while the cones draw, none; with them, the
    // density dims it by sat((w - x) y) (z + w dens / (0.125 + dens))
    sp.fog = {0, 1 / kProxy, 0.5f, 0.5f};
    float fogged[3];
    SpotConeCpu(sp, p, kProxy, kFar, 1, 0.125f, fogged);
    CHECK(fogged[0] == doctest::Approx(rgb[0] * (1 - (0.5f + 0.5f * 0.5f))).epsilon(1e-5));

    // a horizontal ray keeps one height: 0, as on the 360
    const float flat_eye[3] = {-20, 0, 5};
    const float flat_p[3] = {20, 0, 5};
    SpotConeCpu(Cone(flat_eye), flat_p, kProxy, kFar, 1, 0, rgb);
    CHECK(rgb[0] == 0);
}

TEST_CASE("the cross-section's coordinate and the scene depth") {
    const float eye[3] = {0, 0, 0};
    SpotParams sp = Cone(eye);
    // planes at x = -2 and x = 2: 0 halfway, 1 on either
    sp.plane_a = {1, 0, 0, -2};
    sp.plane_b = {1, 0, 0, 2};
    const float mid[3] = {0, 5, 5}, edge[3] = {2, 0, 0}, other[3] = {-2, 0, 0},
                quarter[3] = {-1, 0, 0};
    CHECK(SpotGoboCoordCpu(sp, mid) == doctest::Approx(0));
    CHECK(SpotGoboCoordCpu(sp, edge) == doctest::Approx(1));
    CHECK(SpotGoboCoordCpu(sp, other) == doctest::Approx(1));
    CHECK(SpotGoboCoordCpu(sp, quarter) == doctest::Approx(0.5f));
    // the native depth is 1/w; where nothing drew, the far plane
    CHECK(SpotSceneDepthCpu(sp, 0) == kFar);
    CHECK(SpotSceneDepthCpu(sp, 0.01f) == doctest::Approx(100));
}

TEST_CASE("PackSpot takes a cone's registers") {
    ShadeInputs in{};
    in.shader_type = kDepthVolumeShader;
    in.ps[ShadeRegIndex(10)][0] = 1;
    in.ps[ShadeRegIndex(25)][3] = 0.1f;
    in.ps[ShadeRegIndex(26)][3] = 10;
    in.ps[ShadeRegIndex(27)][2] = -5;
    in.ps[ShadeRegIndex(28)][3] = 0.75f;
    in.ps[ShadeRegIndex(30)][3] = -3;
    in.ps[ShadeRegIndex(86)][0] = 1;
    in.ps[ShadeRegIndex(87)][3] = 2;
    in.ps[ShadeRegIndex(88)][3] = 4;
    in.ps[ShadeRegIndex(89)][1] = kFar;
    in.ps[ShadeRegIndex(90)][1] = 8;
    in.ps[ShadeRegIndex(127)][1] = 1 / kFar;
    SpotParams sp;
    REQUIRE(PackSpot(in, 640, 360, sp));
    CHECK(sp.eye.x == 1);
    CHECK(sp.apex.w == 0.1f);
    CHECK(sp.axis.w == 10);
    CHECK(sp.eye_apex.z == -5);
    CHECK(sp.cone.w == 0.75f);
    CHECK(sp.forward.w == -3);
    CHECK(sp.xsec.x == 1);
    CHECK(sp.plane_a.w == 2);
    CHECK(sp.plane_b.w == 4);
    CHECK(sp.depth_range.y == kFar);
    CHECK(sp.color.y == 8);
    CHECK(sp.fog.y == 1 / kFar);
    CHECK(sp.target.x == 640);
    CHECK(sp.target.w == doctest::Approx(1.0f / 360));
    in.shader_type = 18;
    CHECK_FALSE(PackSpot(in, 640, 360, sp));
}

namespace {

constexpr uint32_t kVolume = 0x241B6958;  // a DxTex, as the capture keeps it
constexpr uint32_t kSize = 16;

Mat4 Identity() {
    Mat4 m{};
    for (int i = 0; i < 4; i++) m.m[i][i] = 1.0f;
    return m;
}

// a quad from x0 to x1 in clip space (world space too: view_proj is
// identity), full height, z 0
std::shared_ptr<const Geometry> Quad(float x0, float x1, uint32_t color) {
    auto g = std::make_shared<Geometry>();
    const float corner[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (const auto& c : corner) {
        Vertex v{};
        v.pos[0] = x0 + c[0] * (x1 - x0);
        v.pos[1] = 1.0f - c[1] * 2.0f;
        v.uv[0] = c[0];
        v.uv[1] = c[1];
        v.color = color;
        g->verts.push_back(v);
    }
    g->indices = {0, 1, 2, 0, 2, 3};
    return g;
}

DrawItem Item(std::shared_ptr<const Geometry> g) {
    DrawItem d;
    d.geom = std::move(g);
    d.world = d.view_proj = Identity();
    for (float& c : d.color) c = 1.0f;
    d.blend = 1;
    d.z_mode = 0;
    d.prelit = true;
    d.alpha_cut = false;
    d.alpha_threshold = 0;
    d.cam = 0;
    d.mesh = 0;
    d.target = kVolume;
    return d;
}

Pass VolumePass(uint32_t first, uint32_t count, uint32_t version, bool clear) {
    Pass p;
    p.tex_obj = kVolume;
    p.first_draw = first;
    p.draw_count = count;
    p.width = p.height = kSize;
    p.tex_type = kTexTypeDepthVolume;
    p.version = version;
    if (clear) {
        p.clear_flags = 0x3f;
        p.clear_color = 0xFF000000u;
    }
    return p;
}

// the depth volume as a blur samples it
std::shared_ptr<const Texture> Volume(uint32_t version) {
    auto t = std::make_shared<Texture>();
    t->width = t->height = kSize;
    t->tex_obj = kVolume;
    t->tex_type = kTexTypeDepthVolume;
    t->version = version;
    return t;
}

// a DrawRect blur across (or down), 5 taps a texel apart weighing 0.1 0.25
// 0.3 0.25 0.1
ShadeState BlurShade(bool down) {
    ShadeState s;
    static_cast<ShadeInputs&>(s) = ShadeInputs{};
    s.shader_type = 1;
    const float weights[5] = {0.1f, 0.25f, 0.3f, 0.25f, 0.1f};
    for (int i = 0; i < 5; i++) {
        float* off = s.ps[ShadeRegIndex(31 + i)];
        off[down ? 1 : 0] = float(i - 2) / kSize;
        off[2] = off[3] = 1;
        for (int c = 0; c < 4; c++) s.ps[ShadeRegIndex(47 + i)][c] = weights[i];
    }
    return s;
}

uint8_t Channel(uint32_t c, int i) { return uint8_t(c >> (8 * i)); }

}  // namespace

TEST_CASE("a cone adds SpotCone's colour into the depth volume, 8-bit, and the blur blurs it") {
    // the camera below the cone's end looking up into it (from the eye, its
    // case 1); the proxy a quad over the volume's left half, at z 0 in
    // world space
    FrameCapture f;
    const float eye[3] = {0, 0, -10};
    const float up[3] = {0, 0, 1};
    SpotParams sp = Cone(eye, up);
    // 40 long, so the volume's half of the ray is within its length
    sp.apex.w = 1.0f / 40;
    sp.axis.w = 40;
    sp.color = {40, 20, 0, 0};
    ShadeState cone;
    static_cast<ShadeInputs&>(cone) = ShadeInputs{};
    cone.shader_type = kDepthVolumeShader;
    const float4* fields[] = {&sp.eye,   &sp.apex,    &sp.axis,    &sp.eye_apex, &sp.cone,
                              &sp.forward, &sp.xsec,  &sp.plane_a, &sp.plane_b,
                              &sp.depth_range, &sp.color, &sp.fog};
    const int regs[] = {10, 25, 26, 27, 28, 30, 86, 87, 88, 89, 90, 127};
    for (int k = 0; k < 12; k++) {
        float* to = cone.ps[ShadeRegIndex(regs[k])];
        to[0] = fields[k]->x;
        to[1] = fields[k]->y;
        to[2] = fields[k]->z;
        to[3] = fields[k]->w;
    }
    f.shades.push_back(cone);
    f.shades.push_back(BlurShade(false));
    f.shades.push_back(BlurShade(true));

    DrawItem c = Item(Quad(-1, 0, 0xffffffffu));
    c.blend = 2;  // Add
    c.shade = 0;
    f.draws.push_back(c);
    // two blur passes, as the capture records them: in place, no clear
    for (uint32_t v = 0; v < 2; v++) {
        DrawItem b = Item(Quad(-1, 1, 0xffffffffu));
        b.rect_shader = 1;
        b.rect[2] = b.rect[3] = float(kSize);
        b.tex = Volume(10 + v);
        b.shade = 1 + int32_t(v);
        f.draws.push_back(b);
    }
    f.passes = {VolumePass(0, 1, 10, true), VolumePass(1, 1, 11, false),
                VolumePass(2, 1, 12, false)};
    // all of it after post-processing starts, as the game draws it
    f.post_boundary = 0;
    f.proc_cmds = 7;

    RasterOptions o;
    o.width = o.height = 32;
    std::vector<uint32_t> px;
    uint32_t w = 0, h = 0;
    REQUIRE(RasterizeTarget(f, o, kVolume, 10, px, w, h));
    REQUIRE(w == kSize);
    // each pixel of the left half: SpotCone at the proxy's position there,
    // with no scene in front (the far plane), added to the clear's black
    uint32_t lit = 0;
    for (uint32_t y = 0; y < kSize; y++) {
        for (uint32_t x = 0; x < kSize; x++) {
            const uint32_t got = px[y * kSize + x];
            CHECK(Channel(got, 3) == 255);
            if (x >= kSize / 2) {
                CHECK((got & 0xffffffu) == 0);
                continue;
            }
            const float p[3] = {(float(x) + 0.5f) / kSize * 2 - 1, 1 - (float(y) + 0.5f) / kSize * 2,
                                0};
            float rgb[3];
            SpotConeCpu(sp, p, 1, kFar, 1, 0, rgb);
            for (int i = 0; i < 3; i++) {
                const int want = int(std::clamp(rgb[i], 0.0f, 1.0f) * 255 + 0.5f);
                CHECK(std::abs(int(Channel(got, i)) - want) <= 1);
            }
            if (Channel(got, 0)) lit++;
        }
    }
    CHECK(lit > kSize * kSize / 4);

    // blurred across (version 11) then down (12, the last): column 8, the
    // first unlit one, takes 0.25 and 0.1 of the two lit ones left of it
    std::vector<uint32_t> blurred;
    REQUIRE(RasterizeTarget(f, o, kVolume, 11, blurred, w, h));
    const uint32_t row = 3;
    const float want = (0.25f * Channel(px[row * kSize + 7], 0) + 0.1f * Channel(px[row * kSize + 6], 0));
    CHECK(std::abs(int(Channel(blurred[row * kSize + 8], 0)) - int(want + 0.5f)) <= 1);
    // at the edge the taps clamp: column 0 takes 0.35 of itself
    const float edge = 0.35f * Channel(px[row * kSize], 0) + 0.3f * Channel(px[row * kSize], 0) +
                       0.25f * Channel(px[row * kSize + 1], 0) + 0.1f * Channel(px[row * kSize + 2], 0);
    CHECK(std::abs(int(Channel(blurred[row * kSize], 0)) - int(edge + 0.5f)) <= 1);
    CHECK(Channel(blurred[row * kSize + 8], 3) == 255);
    // the cone is the same all the way down, so the blur down keeps it
    std::vector<uint32_t> last;
    REQUIRE(RasterizeTarget(f, o, kVolume, 0, last, w, h));
    CHECK(last[row * kSize + 8] == blurred[row * kSize + 8]);

    // nothing samples the depth volume yet (the composite's term is to
    // come), so the frame draws none of it and the picture is the clear
    CHECK(PlanPasses(f, o).size() <= 1);
    for (const PassRun& run : PlanPasses(f, o)) CHECK(run.pass == nullptr);
    std::vector<uint32_t> picture, without;
    Rasterize(f, o, picture);
    FrameCapture none;
    none.post_boundary = 0;
    none.proc_cmds = 7;
    Rasterize(none, o, without);
    CHECK(picture == without);
}
