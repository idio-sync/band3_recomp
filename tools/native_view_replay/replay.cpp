// Experimental: draws a native view capture (BAND3_NATIVE_VIEW_DUMP) offline.
//
//   replay <file.cap> <out.png> [--size WxH] [--cam N] [--per-cam] [--list]
//                               [--compare <screenshot.png> [--image <native.png>]]
//                               [--diff <native.png>] [--crop x,y,w,h] [--mesh <hex>]
//                               [--dump-tex <draw>[:<map>][@<level>]] [--shade <draw>]
//                               [--dump-rt <hex>[:<version>]]
//                               [--rt-none | --rt-guest]
//                               [--no-tex] [--no-blend] [--no-cull] [--no-shadow] [--no-normal]
//                               [--no-default-mat]
//                               [--no-depth-clear]
//                               [--nearest]
//                               [--transpose]
//                               [--no-skinned | --only-skinned] [--unskinned]
//                               [--legacy-light | --no-light] [--pick X,Y]
//                               [--dump-alpha <png>] [--dump-depth <png>]
//                               [--dump-bloom <png>] [--dump-noise <png>]
//                               [--view alpha|depth]
//                               [--no-post | --post-only xfm|dof|bloom|spot|soft|noise]
//                               [--no-grain]
//                               [--no-gamma | --gamma-from <other.cap>]
//                               [--scale <f>] [--shadow-scale <f>] [--msaa 1|2|4]
//
// Prints, for each camera, how many of its vertices land in front of the camera
// and inside the frustum with the matrix as captured and transposed (the back
// buffer's mesh draws, which are what's drawn). --list prints the passes (the
// back buffer's and those into textures: target, name, size, clear, draws,
// version, the frame it was drawn in) with where post-processing starts and
// whether the world before it is another frame's (even/odd rendering), then
// every draw: its mesh, sizes, material and where it lands on screen, and what
// its shader was given (option word, shade, ambient c1, the box map's sum,
// point lights in the option word / with a colour); a draw into a texture or a
// DrawRect quad says so (with its corners' uv, where the material's texture
// transform moved them off 0..1). --shade prints all of one draw's ShadeState. A
// capture with shades also gets a summary of them.
// The texture passes the frame samples are drawn as the native view draws
// them (soft_raster.h), each into a target of its own; a render target no
// pass drew samples what guest memory held, where the capture kept it, else
// transparent black. --rt-none never uses guest memory's pixels (a capture
// with readback drawn the native way only), --rt-guest draws no texture
// passes and samples guest memory's pixels alone (as before them).
// --dump-rt writes what the texture pass target with that DxTex holds (alpha
// shown as black) after the pass making that version of it (its last without
// one), drawn on the CPU, to out.png, and its alpha as grey to out.alpha.png.
// --dump-tex writes a draw's diffuse texture as the capture kept it (guest
// memory's pixels, for a render target), or with :<map> one of its shade's
// maps (normal, specular, glow, projected, gobo...: --shade's names), and its
// alpha likewise: --dump-tex <draw>:projected of a projected light's draw is
// guest memory's copy of NgLight's shadow (right with --readback_resolve=full),
// to set against --dump-rt of it, which the CPU draws. With @<level> it
// writes that mip level of it instead (Texture::mips, guest memory's chain).
// --compare draws the frame at the size of a harness `capture` screenshot and
// writes the two side by side (game left, native right), with their mean
// difference; with --image the native side is that PNG instead (a harness
// capture's <name>.gpu.png, the GPU backend's picture). --diff draws the frame
// on the CPU at that PNG's size into out.png and prints their mean difference,
// to check the GPU backend against the CPU's reference. Both follow it with a
// "metrics:" line (out/research/parity_plan.md; tools/parity.py reads it): the
// mean to three places, the % of pixels whose largest channel is off by more
// than 8, 32 and 64, that difference's p50/p90/p99, the worst mean of a 4x4
// grid's cells and the signed mean (native minus the reference, over RGB).
// --crop x,y,w,h (in the compared PNG's pixels) measures that rectangle alone,
// the mean too: a HUD element, say. --legacy-light draws with the placeholder
// lighting from before the game's shading, --no-light with none (every
// material unlit). --no-cull draws both sides of every
// triangle, as the native view did before it culled as the game does (the
// cull mode --list prints, scene_capture.h's DrawItem::cull). --no-depth-clear
// never clears the back buffer's depth between cameras in a capture from
// before its cameras were kept (RasterOptions::clear_depth_per_camera; one
// with them never does, and layers them by their z ranges). --list prints the
// clear colour the back buffer starts as ("clear:") and the cameras ("camera"
// lines: viewport, z range, the depth the renderers give each one's draws:
// soft_raster.h's LayoutBackBuffer). --no-shadow draws
// the characters without their self-shadows (RasterOptions::self_shadow): no
// shadow map's pass, every SHADOW_BUFFER draw lit. --list's "shadow:" lines
// check the captured shadow maps: each SHADOW_BUFFER draw's s5 is the version
// of the shadow map whose pass came last before it, and its VS c40..c43 are
// that pass's view-projection times the texture's (u = .5x + .5009765625w, v =
// -.5y + .5009765625w), as RB3's CheckShadow makes them, with its draw modes,
// cull modes and options. --no-normal shades every normal-mapped material
// with its vertex normal, leaving its normal map and detail map out
// (RasterOptions::normal_maps), as captures from before the tangents were
// kept are drawn. --no-default-mat leaves out the passes RB3 draws without a
// material, with its default one (scene_capture.h's NoMaterial;
// RasterOptions::default_material), as captures from before kept none. --nearest reads every texture nearest at level 0, as the
// native view did before it sampled them as the game's samplers do
// (RasterOptions::filtering; captures from before the samplers were kept
// draw so either way); --shade prints each texture's sampler and its levels.
// --pick draws the frame at --size and prints the
// draw that last wrote pixel X,Y, its colour and its shade.
// Every capture prints a "post:" line, what post-processing was set to do at
// DxRnd::DoPostProcess (post_params.h: boundary, colour matrix, bloom, DOF,
// the world camera), and a "check:" line setting it against the constants
// RB3's composite drew with (the colour matrix times the modulation against
// c92..c94, c24 recomputed from the DOF fields, c6 from the bloom colour, the
// matrix rebuilt from hue..levels; each with the post flag that says whether
// the composite used it), then the DOF and bloom blurs' taps if captured.
// --dump-alpha and --dump-depth draw the frame on the CPU at --size and write
// the scene target's alpha (the bloom weight) or depth as grey (soft_raster.h's
// RasterView), where the world's draws left them; --view alpha|depth does the
// same for the picture the other options draw (--diff against a capture's
// <name>.gpu.alpha.png or .gpu.depth.png, say).
// --dump-bloom draws the frame on the CPU at --size and writes bloom's level
// 0 (a quarter of it each way) as the composite read it, after glare's pass on
// a glare frame: at the game's 1280x720, against --dump-tex of the glare
// pass's draw (--list's "rect shader 25"), whose guest pixels are what that
// pass left in the level (right in captures taken with --readback_resolve=full).
// RB3's post-processing (post_model.h: depth of field, bloom or glare, the
// spotlights' depth volume, the noise, the colour matrix) is applied as the
// frame set it; --no-post leaves the scene as it is, --post-only applies one
// effect alone (bloom covers glare), to see what each contributes, and
// --no-grain leaves the noise (film grain) out. The "noise:" line under
// "check:" says what the noise drew with: the flags (+0x2D, midtone +0x2E),
// c112 and c113 against the proc's fields (c113 = base scale, top scale or 1
// if stationary, intensity; c112 the stationary seeds when it's stationary),
// and the map the capture kept, its size, format, levels and sampler, and
// sampler 13's base against the proc's map's. --dump-noise writes that map
// to a PNG (level 0; its mips beside it, <png>.1.png and on).
// The spotlights' passes (spot_model.h) show in --list as passes into the
// depth-volume and density textures, the cones as "cone" draws; --dump-rt
// of the depth volume draws them on the CPU. The soft particles' show as
// "soft" draws (scene_capture.h's IsSoftParticle) in the pass into the
// soft-particle buffer's first surface, before its two blurs; --dump-rt of
// that surface draws them, and --post-only soft adds the buffer alone.
// The display's gamma ramp (gamma_ramp.h), which the presenter applies to the
// game's picture and so to a harness screenshot, goes over the native picture
// last, as captured; every capture prints a "gamma:" line, which ramp and what
// it shows a few values as, and --list the whole ramp. --no-gamma leaves it
// off, --gamma-from draws with another capture's (one from before captures
// kept it has none: it's drawn as RB3 drew it).
// --scale draws the frame at f times the game's 1280x720 (or at --size, or
// the PNG's size with --compare and --diff), with the passes that are
// pictures of the screen (the spotlights' depth volume and density map, the
// soft-particle surfaces) f times their game size, as the native renderer
// draws them at a window f times 720 lines tall (RasterOptions::
// target_scale); --shadow-scale draws the characters' shadow maps f times
// theirs (RasterOptions::shadow_scale). --dump-rt writes a scaled pass at
// its scaled size.
// --msaa draws the overlay (the draws from post-processing on: the track, the
// HUD) with that many samples a pixel, averaged (RasterOptions::msaa,
// native_view_msaa): 2, the default, as RB3 does, 4 smoother, 1 none.
//
// Build (from the repository root):
//   clang++ -std=c++20 -O2 -I. tools/native_view_replay/replay.cpp
//     src/Render/soft_raster.cpp src/Render/shade_model.cpp src/Render/post_model.cpp
//     src/Render/spot_model.cpp src/Render/capture_file.cpp src/Render/png_writer.cpp
//     -o out/native_view_replay.exe

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "src/Render/capture_file.h"
#include "src/Render/png_writer.h"
#include "src/Render/post_model.h"
#include "src/Render/post_params.h"
#include "src/Render/shade_model.h"
#include "src/Render/soft_raster.h"

using namespace band3::render;

namespace {

Mat4 Transpose(const Mat4& m) {
    Mat4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r.m[i][j] = m.m[j][i];
    return r;
}

void Clip(const float p[3], const Mat4& world, const Mat4& vp, float out[4]) {
    float w[3];
    for (int c = 0; c < 3; c++)
        w[c] = p[0] * world.m[0][c] + p[1] * world.m[1][c] + p[2] * world.m[2][c] + world.m[3][c];
    for (int c = 0; c < 4; c++)
        out[c] = w[0] * vp.m[0][c] + w[1] * vp.m[1][c] + w[2] * vp.m[2][c] + vp.m[3][c];
}

struct CamStats {
    int draws = 0;
    int verts = 0;
    int front[2] = {0, 0};
    int inside[2] = {0, 0};
    Mat4 vp;
};

// a rectangle of the compared pictures, in their pixels (--crop); w 0 is all
struct Crop {
    uint32_t x = 0, y = 0, w = 0, h = 0;
};

// How far a native picture is from a reference, over the pixels of `crop`
// both have, every `step`th in x and y (--compare halves the screenshot):
// the mean of |native - reference| over RGB, which the pictures' "mean
// difference" lines print, and out/research/parity_plan.md's metrics
struct DiffStats {
    size_t pixels = 0;
    double mean = 0;
    double signed_mean = 0;            // native - reference, over RGB
    double over8 = 0, over32 = 0, over64 = 0;  // % of pixels, by their largest channel
    int p50 = 0, p90 = 0, p99 = 0;     // of the largest channel's difference
    double worst_cell = 0;             // the worst of a 4x4 grid's cells' means
    int worst_cx = 0, worst_cy = 0;
};

DiffStats Measure(const std::vector<uint32_t>& ref, const std::vector<uint32_t>& native,
                  uint32_t width, uint32_t height, uint32_t step, Crop crop) {
    if (!crop.w || !crop.h) crop = {0, 0, width, height};
    // only the sampled pixels inside both the crop and the picture
    const uint32_t x_end = std::min(crop.x + crop.w, width / step * step);
    const uint32_t y_end = std::min(crop.y + crop.h, height / step * step);
    const uint32_t x0 = (crop.x + step - 1) / step * step, y0 = (crop.y + step - 1) / step * step;
    DiffStats s;
    double sum = 0, signed_sum = 0;
    size_t over[3] = {0, 0, 0};
    size_t hist[256] = {};
    double cell_sum[4][4] = {};
    size_t cell_n[4][4] = {};
    for (uint32_t y = y0; y < y_end; y += step) {
        const uint32_t cy = std::min(3u, (y - crop.y) * 4 / crop.h);
        for (uint32_t x = x0; x < x_end; x += step) {
            const uint32_t cx = std::min(3u, (x - crop.x) * 4 / crop.w);
            const uint32_t a = ref[size_t(y) * width + x], b = native[size_t(y) * width + x];
            int most = 0, abs_sum = 0;
            for (int c = 0; c < 3; c++) {
                const int d = int((b >> (8 * c)) & 0xff) - int((a >> (8 * c)) & 0xff);
                signed_sum += d;
                abs_sum += std::abs(d);
                most = std::max(most, std::abs(d));
            }
            sum += abs_sum;
            cell_sum[cy][cx] += abs_sum;
            cell_n[cy][cx]++;
            hist[most]++;
            if (most > 8) over[0]++;
            if (most > 32) over[1]++;
            if (most > 64) over[2]++;
            s.pixels++;
        }
    }
    if (!s.pixels) return s;
    const double n = double(s.pixels);
    s.mean = sum / (n * 3);
    s.signed_mean = signed_sum / (n * 3);
    s.over8 = 100.0 * double(over[0]) / n;
    s.over32 = 100.0 * double(over[1]) / n;
    s.over64 = 100.0 * double(over[2]) / n;
    // the smallest difference that many of the pixels are within
    auto percentile = [&](double p) {
        size_t seen = 0;
        for (int v = 0; v < 256; v++) {
            seen += hist[v];
            if (double(seen) >= p * n) return v;
        }
        return 255;
    };
    s.p50 = percentile(0.5);
    s.p90 = percentile(0.9);
    s.p99 = percentile(0.99);
    for (int cy = 0; cy < 4; cy++) {
        for (int cx = 0; cx < 4; cx++) {
            if (!cell_n[cy][cx]) continue;
            const double m = cell_sum[cy][cx] / (double(cell_n[cy][cx]) * 3);
            if (m > s.worst_cell) {
                s.worst_cell = m;
                s.worst_cx = cx;
                s.worst_cy = cy;
            }
        }
    }
    return s;
}

// the line after a "mean difference" one (tools/parity.py reads both)
void PrintDiffStats(const DiffStats& s, const Crop& crop) {
    std::printf("  metrics: %zu pixels", s.pixels);
    if (crop.w && crop.h)
        std::printf(" in crop %u,%u %ux%u", crop.x, crop.y, crop.w, crop.h);
    std::printf("; mean %.3f; >8 %.2f%% >32 %.2f%% >64 %.2f%%; p50 %d p90 %d p99 %d; "
                "worst cell %.2f (%d,%d of 4x4); signed %+.2f\n",
                s.mean, s.over8, s.over32, s.over64, s.p50, s.p90, s.p99, s.worst_cell, s.worst_cx,
                s.worst_cy, s.signed_mean);
}

void PrintMat(const Mat4& m) {
    for (int i = 0; i < 4; i++)
        std::printf("      [%10.4f %10.4f %10.4f %10.4f]\n", m.m[i][0], m.m[i][1], m.m[i][2],
                    m.m[i][3]);
}

// ShaderOptions bit names (rb3-xenon rndobj/ShaderOptions.cpp's macros)
struct OptionName {
    int bit, width;
    const char* name;
};
constexpr OptionName kOptionNames[] = {
    {0, 1, "PER_PIXEL"},      {1, 1, "SPECULAR_MAP"},    {2, 1, "SPECULAR"},
    {3, 1, "ENVIRON_MAP"},    {4, 1, "DIFFUSE_MAP"},     {5, 1, "NORMAL_MAP"},
    {6, 1, "COPY_PREVIOUS"},  {7, 1, "GLOW_MAP"},        {8, 1, "PRELIT"},
    {10, 2, "TEX_GEN"},       {12, 1, "SKINNED"},        {13, 1, "SCREEN_ALIGNED"},
    {14, 1, "RIM_UNDER"},     {15, 1, "RIM_MAP"},        {16, 1, "REAL_LIGHTS"},
    {17, 1, "APPROX_LIGHTS"}, {18, 1, "FOG"},            {19, 1, "SHADOW_BUFFER"},
    {20, 1, "ANISOTROPIC"},   {21, 1, "COLOR_XFM"},      {22, 1, "PSEUDO_HDR"},
    {23, 1, "EXTRUDE"},       {24, 1, "NORM_DETAIL"},    {25, 1, "BILLBOARD"},
    {26, 2, "FADE_OUT"},      {28, 2, "NUM_PROJ"},       {30, 2, "VARIATION"},
    {32, 2, "COLOR_MOD"},     {34, 1, "FUR_DETAIL"},     {37, 1, "RIM_LIGHT"},
    {38, 1, "AO"},            {39, 1, "TONE_MAPPING"},   {40, 2, "NUM_POINT"},
    {42, 1, "VELOCITY"},      {43, 1, "ENVIRON_FALLOFF"}, {44, 1, "PROJ_MULTIPLY"},
    {45, 1, "SOFT_DEPTH"},    {46, 1, "REFRACT_WORLD"},  {48, 1, "POINT_CUBE_TEX"},
    {49, 1, "ENVIRON_SPECMASK"}, {51, 1, "SPOTLIGHT"},   {53, 1, "INTENSIFY"},
};
constexpr const char* kMapNames[kNumShadeMaps] = {"normal", "specular", "glow",   "environ",
                                                  "projected", "gobo", "detail", "rim"};

std::string OptionString(uint64_t options) {
    std::string out;
    for (const OptionName& o : kOptionNames) {
        const uint32_t v = uint32_t(options >> o.bit) & ((1u << o.width) - 1);
        if (!v) continue;
        if (!out.empty()) out += ' ';
        out += o.name;
        if (o.width > 1) out += "=" + std::to_string(v);
    }
    return out;
}

const ShadeState* ShadeOf(const FrameCapture& fc, const DrawItem& d) {
    return d.shade >= 0 && size_t(d.shade) < fc.shades.size() ? &fc.shades[d.shade] : nullptr;
}

// the box map's six colours added up, rgb
float BoxSum(const ShadeInputs& s) {
    float sum = 0;
    for (int reg = 80; reg <= 85; reg++)
        for (int c = 0; c < 3; c++) sum += s.Vs(reg)[c];
    return sum;
}

// point lights with a colour (c67, c68)
int PointsLit(const ShadeInputs& s) {
    int n = 0;
    for (int reg = 67; reg <= 68; reg++) {
        const float* c = s.Vs(reg);
        if (c[0] != 0 || c[1] != 0 || c[2] != 0) n++;
    }
    return n;
}

bool SameColor(const float* a, const float* b) {
    for (int c = 0; c < 4; c++)
        if (std::fabs(a[c] - b[c]) > 1e-5f) return false;
    return true;
}

// what a texture fetch constant says: format, dimension (1 2D, 3 cube),
// size and base address
std::string FetchString(const uint32_t f[6]) {
    if (!f[1]) return "-";
    char buf[128];
    // the clamp modes (0 wrap, 2 the edge, 6 the border...) are dword 0's
    // bits 10-12 and 13-15
    std::snprintf(buf, sizeof(buf), "fmt %u dim %u %ux%u base %08X clamp %u,%u", f[1] & 0x3f,
                  (f[5] >> 9) & 3, (f[2] & 0x1fff) + 1, ((f[2] >> 13) & 0x1fff) + 1,
                  f[1] & 0xfffff000u, (f[0] >> 10) & 7, (f[0] >> 13) & 7);
    return buf;
}

// what a sampler does (scene_capture.h's TexSampler)
std::string SamplerString(const TexSampler& t) {
    if (!t.filtered) return "sampler: none kept (nearest, level 0)";
    static const char* kMip[] = {"nearest", "linear", "base", "?"};
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "sampler: clamp %u,%u mag %s min %s mip %s aniso %u levels %u-%u bias %.2f "
                  "border %s",
                  t.clamp_x, t.clamp_y, t.mag_linear ? "linear" : "point",
                  t.min_linear ? "linear" : "point", kMip[t.mip & 3], t.aniso, t.mip_min,
                  t.mip_max, t.lod_bias, t.border_white ? "white" : "black");
    return buf;
}

// a texture's size and how many levels the capture kept of it
std::string LevelsString(const Texture* t) {
    if (!t) return "none";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%ux%u, %zu level%s%s", t->width, t->height,
                  1 + t->mips.size(), t->mips.empty() ? "" : "s",
                  t->rgba.empty() ? " (no pixels)" : "");
    return buf;
}

// the shades' numbers over the frame: how well the constants and option word
// agree with the material, and how often a draw has a second pass
void PrintShadeSummary(const FrameCapture& fc) {
    using namespace shader_opt;
    size_t with = 0, vs_c0 = 0, ps_c0 = 0, premult = 0, next_pass = 0, lit = 0, real = 0,
           approx = 0, box = 0, diffuse_bit = 0, prelit_bit = 0, s0_same = 0, s0_differ = 0;
    float eye_off = 0;
    std::map<int, size_t> types;
    size_t sampled[kNumShadeMaps] = {}, same_base[kNumShadeMaps] = {},
           other_base[kNumShadeMaps] = {}, decoded[kNumShadeMaps] = {};
    std::map<std::string, size_t> formats;
    // NORMAL_MAP draws, those whose geometry kept its tangents (the
    // renderers' normal maps need them), their skin and detail ones, and
    // the texgen's third row, VS c22, other than (0, 0, 1): the frame turned
    size_t nmap = 0, nmap_tangents = 0, nmap_skin = 0, nmap_detail = 0, nmap_c22 = 0,
           nmap_rt = 0;
    for (const DrawItem& d : fc.draws) {
        const ShadeState* s = ShadeOf(fc, d);
        if (!s) continue;
        with++;
        if (s->Option(kNormalMap) && d.rect_shader < 0) {
            nmap++;
            if (d.geom && d.geom->tangents) nmap_tangents++;
            if (s->OptionBits(kCustomVariation, 2) == 1) nmap_skin++;
            if (s->Option(kNormDetail)) nmap_detail++;
            if (MapTargetOf(s, kMapNormal)) nmap_rt++;
            const float* c22 = s->Vs(22);
            if (c22[0] != 0 || c22[1] != 0 || c22[2] != 1) nmap_c22++;
        }
        types[s->shader_type]++;
        if (!SameColor(s->Vs(0), d.color)) {
            vs_c0++;
            if (d.blend == 7) premult++;
        }
        if (!SameColor(s->Ps(0), d.color)) ps_c0++;
        if (s->next_pass) next_pass++;
        if (s->use_environ) lit++;
        if (s->Option(kRealLights)) real++;
        if (s->Option(kApproxLights)) approx++;
        if (BoxSum(*s) != 0) box++;
        if (s->Option(kDiffuseMap) != (s->mat_diffuse_base != 0)) diffuse_bit++;
        // particles' draws say prelit for their vertex colour, the
        // spotlights' cones for having no lighting
        if (s->shader_type != 14 && !IsSpotCone(d, s) && s->Option(kPrelit) != d.prelit)
            prelit_bit++;
        if (s->fetch_diffuse[1] && s->mat_diffuse_base) {
            if ((s->fetch_diffuse[1] & 0xfffff000u) == s->mat_diffuse_base) s0_same++;
            else s0_differ++;
        }
        for (int c = 0; c < 3; c++) {
            const float* row = s->Vs(16 + c);
            eye_off = std::max(eye_off, std::fabs(row[3] - s->eye[c]));
        }
        for (int m = 0; m < kNumShadeMaps; m++) {
            if (!s->fetch[m][1]) continue;
            sampled[m]++;
            if (s->maps[m]) decoded[m]++;
            if (s->mat_map_base[m]) {
                if ((s->fetch[m][1] & 0xfffff000u) == s->mat_map_base[m]) same_base[m]++;
                else other_base[m]++;
            }
            char key[64];
            std::snprintf(key, sizeof(key), "%s fmt %u dim %u", kMapNames[m],
                          s->fetch[m][1] & 0x3f, (s->fetch[m][5] >> 9) & 3);
            formats[key]++;
        }
    }
    if (!with) return;
    const double n = double(with);
    std::printf("shades: %zu distinct for %zu of %zu draws; types", fc.shades.size(), with,
                fc.draws.size());
    for (auto [t, k] : types) std::printf(" %d:%zu", t, k);
    std::printf("\n  c0 not the material colour: VS %zu (%zu premultiplied blend), PS %zu\n",
                vs_c0, premult, ps_c0);
    std::printf("  next_pass: %zu draws (%.1f%%)\n", next_pass, 100.0 * double(next_pass) / n);
    std::printf("  use_environ %zu, REAL_LIGHTS %zu, APPROX_LIGHTS %zu, box map nonzero %zu\n",
                lit, real, approx, box);
    std::printf("  option word against the material: DIFFUSE_MAP differs %zu, PRELIT differs "
                "%zu; s0 bound = material's diffuse %zu, other %zu\n",
                diffuse_bit, prelit_bit, s0_same, s0_differ);
    std::printf("  eye (camera translation) against VS c16..c18.w: off by up to %g\n", eye_off);
    std::printf("  NORMAL_MAP draws %zu, their geometry with tangents %zu; skin %zu, "
                "NORM_DETAIL %zu, VS c22 not (0,0,1) %zu, the map a render target %zu\n",
                nmap, nmap_tangents, nmap_skin, nmap_detail, nmap_c22, nmap_rt);
    for (int m = 0; m < kNumShadeMaps; m++) {
        if (!sampled[m]) continue;
        std::printf("  %-9s sampled by %zu draws, %zu decoded; bound = material's %zu, other %zu\n",
                    kMapNames[m], sampled[m], decoded[m], same_base[m], other_base[m]);
    }
    for (auto& [k, c] : formats) std::printf("    %s: %zu draws\n", k.c_str(), c);
}

void PrintShade(const FrameCapture& fc, size_t draw) {
    const DrawItem& d = fc.draws[draw];
    const ShadeState* s = ShadeOf(fc, d);
    if (!s) {
        std::printf("draw %zu has no shade\n", draw);
        return;
    }
    std::printf("draw %zu: shade %d, mesh %08X, material %08X, env %08X\n", draw, d.shade,
                d.mesh, s->mat, s->env);
    std::printf("  options %016llX type %d: %s\n", (unsigned long long)s->options,
                s->shader_type, OptionString(s->options).c_str());
    std::printf("  eye %.3f %.3f %.3f\n", s->eye[0], s->eye[1], s->eye[2]);
    std::printf("  use_environ %u intensify %u per_pixel_lit %u shader_variation %d "
                "next_pass %08X\n",
                s->use_environ, s->intensify, s->per_pixel_lit, s->shader_variation,
                s->next_pass);
    std::printf("  material colour %.4f %.4f %.4f %.4f\n", d.color[0], d.color[1], d.color[2],
                d.color[3]);
    for (int i = 0; i < kNumShadeRegs; i++) {
        const float* v = s->vs[i];
        const float* p = s->ps[i];
        std::printf("  c%-3u VS %10.4f %10.4f %10.4f %10.4f | PS %10.4f %10.4f %10.4f %10.4f\n",
                    kShadeRegs[i], v[0], v[1], v[2], v[3], p[0], p[1], p[2], p[3]);
    }
    std::printf("  s0  diffuse: bound %s, material's base %08X\n",
                FetchString(s->fetch_diffuse).c_str(), s->mat_diffuse_base);
    std::printf("      %s; texture %s\n", SamplerString(s->diffuse_sampler).c_str(),
                LevelsString(d.tex.get()).c_str());
    for (int m = 0; m < kNumShadeMaps; m++) {
        std::printf("  s%-2u %s: bound %s, material %08X (base %08X)", kShadeMapSampler[m],
                    kMapNames[m], FetchString(s->fetch[m]).c_str(), s->mat_maps[m],
                    s->mat_map_base[m]);
        if (const Texture* t = s->maps[m].get(); t && t->tex_obj)
            std::printf(", render target %08X type 0x%X version %u%s", t->tex_obj, t->tex_type,
                        t->version, t->rgba.empty() ? "" : " (guest pixels)");
        else if (t)
            std::printf(", decoded %ux%u", t->width, t->height);
        std::printf("\n");
        if (s->fetch[m][1])
            std::printf("      %s; map %s\n", SamplerString(s->samplers[m]).c_str(),
                        LevelsString(s->maps[m].get()).c_str());
    }
}

const char* TexTypeName(uint32_t type) {
    switch (type) {
        case 0x2: return "rendered";
        case 0x22: return "rendered-noz";
        case 0x42: return "shadow-map";
        case 0xA2: return "depth-volume";
        case 0x122: return "density";
        default: return "other";
    }
}

// whether a pass was carried in from a frame before the capture's: a
// composed capture's own are its world frame's as well as its frame's
bool Carried(const FrameCapture& fc, const Pass& p) {
    return p.from_frame < (fc.composed ? fc.world_frame : fc.game_frame);
}

// the passes' numbers in a line, and where post-processing starts
void PrintPassSummary(const FrameCapture& fc) {
    if (fc.passes.empty()) return;
    size_t textures = 0, carried = 0;
    for (const Pass& p : fc.passes) {
        if (!p.tex_obj) continue;
        textures++;
        if (Carried(fc, p)) carried++;
    }
    std::printf("passes: %zu (%zu into textures, %zu of them carried from earlier frames); game "
                "frame %llu; render targets sampled %u, missing %u, their pass's draws all left "
                "out %u; snapshots %u; empty passes left out %u, unbalanced %u\n",
                fc.passes.size(), textures, carried, (unsigned long long)fc.game_frame,
                fc.rt_sampled, fc.rt_missing, fc.rt_filtered, fc.rt_snapshots, fc.passes_empty,
                fc.passes_unbalanced);
    std::printf("draws left out: skipped_shadow %u (shadow draw modes outside their passes), "
                "skipped_velocity %u, skipped_draw_mode %u, skipped_no_geom %u, "
                "skipped_target %u\n",
                fc.skipped_shadow, fc.skipped_velocity, fc.skipped_draw_mode, fc.skipped_no_geom,
                fc.skipped_target);
    uint32_t no_mat = 0;
    for (const DrawItem& d : fc.draws)
        no_mat += NoMaterial(d, d.shade >= 0 ? &fc.shades[size_t(d.shade)] : nullptr);
    std::printf("material passes: after a mesh's first %u, without a material %u (%u of them "
                "kept, with the default material; the rest in skipped_no_geom too); DrawFaces "
                "outside a DrawShowing %u\n",
                fc.later_passes, fc.skipped_no_mat, no_mat, fc.faces_elsewhere);
    // the render targets draws sample that no pass here made
    std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, size_t>> missing;
    for (const DrawItem& d : fc.draws) {
        if (!d.tex || !d.tex->tex_obj || !IsPassTargetType(d.tex->tex_type)) continue;
        bool made = false;
        for (const Pass& p : fc.passes)
            made |= p.tex_obj == d.tex->tex_obj && p.version == d.tex->version;
        if (!made) {
            auto& m = missing[{d.tex->tex_obj, d.tex->version}];
            m.first = d.tex->tex_type;
            m.second++;
        }
    }
    for (const auto& [key, m] : missing) {
        const uint64_t k = uint64_t(key.first) << 32 | key.second;
        const bool left_out = std::find(fc.rt_filtered_keys.begin(), fc.rt_filtered_keys.end(),
                                        k) != fc.rt_filtered_keys.end();
        std::printf("  no pass for %08X version %u (%s), sampled by %zu draws%s\n", key.first,
                    key.second, TexTypeName(m.first), m.second,
                    left_out ? ": its pass's draws were all left out (draw mode, no geometry)"
                             : "");
    }
    if (fc.post_boundary == FrameCapture::kNoPost)
        std::printf("post-processing: none this frame\n");
    else
        std::printf("post-processing: from draw %u, proc_cmds %u\n", fc.post_boundary,
                    fc.proc_cmds);
    // even/odd rendering: a post frame's capture with the world frame's world
    if (fc.composed)
        std::printf("composed: the world (draws before %u) is game frame %llu's, the rest "
                    "game frame %llu's\n",
                    fc.post_boundary, (unsigned long long)fc.world_frame,
                    (unsigned long long)fc.game_frame);
    else
        std::printf("composed: no, the world is the frame's own\n");
}

// the largest difference between two runs of floats
float MaxDiff(const float* a, const float* b, int n) {
    float most = 0;
    for (int i = 0; i < n; i++) most = std::max(most, std::fabs(a[i] - b[i]));
    return most;
}

// The display gamma ramp (the "gamma:" line): which, and what a few 8-bit
// values show as through it; with `all`, every entry, 10-bit
void PrintGamma(const FrameCapture& fc, bool all) {
    const GammaRamp& g = fc.gamma;
    if (g.mode == GammaRamp::kNone) {
        std::printf("gamma: none (a capture from before the ramp was kept, or it wasn't read): "
                    "drawn as RB3 drew it\n");
        return;
    }
    uint8_t lut[3][256];
    GammaLut(g, lut);
    std::printf("gamma: %s; value: shown r/g/b", g.mode == GammaRamp::kTable ? "table" : "pwl");
    for (int v : {0, 1, 2, 4, 8, 16, 32, 64, 96, 128, 192, 255})
        std::printf(" %d:%u/%u/%u", v, lut[0][v], lut[1][v], lut[2][v]);
    int differ = 0;
    for (int c = 0; c < 3; c++)
        for (int v = 0; v < 256; v++) differ += lut[c][v] != v;
    std::printf("; %d of 768 values change\n", differ);
    if (!all) return;
    if (g.mode == GammaRamp::kTable) {
        std::printf("  table (10-bit r/g/b by 8-bit value):\n");
        for (int v = 0; v < 256; v++) {
            if (v % 8 == 0) std::printf("   %3d:", v);
            std::printf(" %4u/%4u/%4u", TableChannel(g.table[v], 0), TableChannel(g.table[v], 1),
                        TableChannel(g.table[v], 2));
            if (v % 8 == 7) std::printf("\n");
        }
    } else {
        std::printf("  pwl (base+delta, 10.6 fixed point, r g b by step of 8 10-bit values):\n");
        for (int i = 0; i < 128; i++) {
            std::printf("   %3d:", i);
            for (int c = 0; c < 3; c++)
                std::printf(" %04X+%04X", g.pwl[i][c] & 0xffff, g.pwl[i][c] >> 16);
            std::printf("\n");
        }
    }
}

// What post-processing was set to do (the "post:" line), against what RB3's
// composite drew with (the "check:" line), and the blurs' taps
void PrintPost(const FrameCapture& fc) {
    const PostParams& p = fc.post;
    if (!p.valid) {
        std::printf("post: none read (no DoPostProcess this frame, or a capture from before)\n");
        return;
    }
    float rows[3][4];
    ModulatedXfm(p, rows);
    std::printf("post: boundary %u, proc_cmds %u, proc %08X%s%s; xfm %s, mod %.4f, rows "
                "[%.4f %.4f %.4f | %.4f] [%.4f %.4f %.4f | %.4f] [%.4f %.4f %.4f | %.4f] "
                "(hue %.1f sat %.1f light %.1f contrast %.1f bright %.1f); bloom colour %.3f %.3f "
                "%.3f %.3f threshold %.3f intensity %.3f%s%s; DOF %s focal %.2f blurDepth %.4f "
                "min %.4f max %.4f (z scale %.6f bias %.6f, width scale %.3f); cam %08X near %.3f "
                "far %.1f zrange %.3f..%.3f; emulate_fps %.1f\n",
                fc.post_boundary, fc.proc_cmds, p.proc, p.overridden ? " (override)" : "",
                p.disabled ? " disabled" : "", ColorXfmEnabled(p) ? "enabled" : "off",
                p.color_mod, rows[0][0], rows[0][1], rows[0][2], rows[0][3], rows[1][0],
                rows[1][1], rows[1][2], rows[1][3], rows[2][0], rows[2][1], rows[2][2], rows[2][3],
                p.hue, p.saturation, p.lightness, p.contrast, p.brightness, p.bloom_color[0],
                p.bloom_color[1], p.bloom_color[2], p.bloom_color[3], p.bloom_threshold,
                p.bloom_intensity, p.bloom_glare ? " glare" : "", p.bloom_streak ? " streak" : "",
                p.dof_enabled ? "on" : "off", p.dof_focal, p.dof_blur_depth, p.dof_min_blur,
                p.dof_max_blur, p.dof_scale, p.dof_bias, p.blur_width_scale, p.cam, p.cam_near,
                p.cam_far, p.cam_zrange[0], p.cam_zrange[1], p.emulate_fps);

    const PostConsts& c = fc.post_consts;
    float rebuilt[3][3], rebuilt_v[3];
    AdjustColorXfm(p, rebuilt, rebuilt_v);
    const float rebuilt_diff = std::max(MaxDiff(&rebuilt[0][0], &p.xfm[0][0], 9),
                                        MaxDiff(rebuilt_v, p.xfm_offset, 3));
    if (!c.valid) {
        std::printf("check: no composite constants (FinishPostProcess didn't run this frame); "
                    "matrix rebuilt from hue..levels off by %.6f\n",
                    rebuilt_diff);
        return;
    }
    float dof[4], bloom[4];
    DofConstants(p, dof);
    BloomConstant(p, bloom);
    std::printf("check: flags DOF %u bloom %u glare %u xfm %u (ColorXfmEnabled %u); matrix*mod "
                "vs c92..c94 off by %.6f; c24 recomputed %.6f %.6f %.4f %.4f vs captured %.6f "
                "%.6f %.4f %.4f, off by %.6f; c6 bloom colour*intensity %.4f %.4f %.4f vs "
                "captured %.4f %.4f %.4f, off by %.6f; matrix rebuilt from hue..levels off by "
                "%.6f\n",
                c.flags[kPostFlagDof], c.flags[kPostFlagBloom], c.flags[kPostFlagGlare],
                c.flags[kPostFlagColorXfm], unsigned(ColorXfmEnabled(p)),
                MaxDiff(&rows[0][0], &c.c92[0][0], 12), dof[0], dof[1], dof[2], dof[3],
                c.c24[0], c.c24[1], c.c24[2], c.c24[3], MaxDiff(dof, c.c24, 4), bloom[0],
                bloom[1], bloom[2], c.c6[0], c.c6[1], c.c6[2], MaxDiff(bloom, c.c6, 3),
                rebuilt_diff);
    std::printf("  c92..c94 captured [%.4f %.4f %.4f | %.4f] [%.4f %.4f %.4f | %.4f] [%.4f %.4f "
                "%.4f | %.4f]\n",
                c.c92[0][0], c.c92[0][1], c.c92[0][2], c.c92[0][3], c.c92[1][0], c.c92[1][1],
                c.c92[1][2], c.c92[1][3], c.c92[2][0], c.c92[2][1], c.c92[2][2], c.c92[2][3]);
    std::printf("  flags 0x26..0x3F:");
    for (uint8_t f : c.flags) std::printf(" %02X", f);
    std::printf("; c15 %.6f %.6f %.6f %.6f\n", c.c15[0], c.c15[1], c.c15[2], c.c15[3]);
    // the spotlights' term (s12 * (c127.x + c127.y * s5.x) * c91.x,
    // TheShaderMgr +0x25: out/research/spotlight_survey.md 2), the soft
    // particles' (s4, +0x3F, the buffer's first surface: softparticle_survey.md
    // 1), the noise's (c112, c113, +0x2D/+0x2E: out/research/
    // n1_post_noise.md), and what the native composite leaves out: velocity
    // blur (s10 * c122) (out/research/m4_design.md)
    const auto flag = [&](int offset) { return unsigned(c.flags[offset - kPostFlagBase]); };
    std::printf("  spotlights: +0x25 %02X c127 %.4f %.4f %.4f %.4f c91 %.4f %.4f %.4f %.4f\n",
                c.spot_flag, c.c127[0], c.c127[1], c.c127[2], c.c127[3], c.c91[0], c.c91[1],
                c.c91[2], c.c91[3]);
    std::printf("  soft particles: +0x3F %02X, surfaces %08X %08X\n", flag(0x3F),
                c.soft_surface[0], c.soft_surface[1]);
    {
        float want[4];
        NoiseConstant(p, want);
        const float seeds[4] = {p.noise_seeds[0], p.noise_seeds[1], p.noise_seeds[0],
                                p.noise_seeds[1]};
        const uint32_t base = c.noise_fetch[1] & 0xfffff000u;
        std::printf("  noise: +0x2D %02X +0x2E %02X (proc: intensity %.4f map %08X midtone %u "
                    "stationary %u, so %s); c112 %.4f %.4f %.4f %.4f%s; c113 %.4f %.4f %.4f %.4f "
                    "vs proc %.4f %.4f %.4f %.4f, off by %.6f\n",
                    flag(0x2D), flag(0x2E), p.noise_intensity, p.noise_map, p.noise_midtone,
                    p.noise_stationary, NoiseEnabled(p) ? "on" : "off", c.c112[0], c.c112[1],
                    c.c112[2], c.c112[3],
                    !p.noise_stationary                ? ""
                    : MaxDiff(seeds, c.c112, 4) == 0 ? " (the stationary seeds)"
                                                       : " (NOT the stationary seeds)",
                    c.c113[0], c.c113[1], c.c113[2], c.c113[3], want[0], want[1], want[2], want[3],
                    MaxDiff(want, c.c113, 4));
        if (const Texture* t = fc.noise_map.get()) {
            const TexSampler& ns = fc.noise_sampler;
            std::printf("    map %ux%u format %u, %zu mips; sampler clamp %u/%u mag %u min %u mip "
                        "%u (%u..%u) aniso %u bias %.3f; s13 base %08X vs the proc's map %08X "
                        "(%s)\n",
                        t->width, t->height, t->format, t->mips.size(), ns.clamp_x, ns.clamp_y,
                        ns.mag_linear, ns.min_linear, ns.mip, ns.mip_min, ns.mip_max, ns.aniso,
                        ns.lod_bias, base, p.noise_map_base,
                        base == p.noise_map_base ? "same" : "DIFFERENT");
        } else if (flag(0x2D)) {
            std::printf("    no map kept (a capture from before, or a format not decoded): no "
                        "grain\n");
        }
    }
    // and the trails (blend previous, +0x2F: the previous post frame faded
    // by c125.y, kept where its mean is over the threshold c125.x and over
    // the colour's), which a capture can't draw: it has no previous frame
    std::printf("  left out: velocity +0x38 %02X +0x39 %02X c122 %.4f %.4f %.4f %.4f; trails "
                "+0x2F %02X c125 %.4f %.4f %.4f %.4f (proc threshold %.4f duration %.4f)\n",
                flag(0x38), flag(0x39), c.c122[0], c.c122[1], c.c122[2], c.c122[3], flag(0x2F),
                c.c125[0], c.c125[1], c.c125[2], c.c125[3], p.trail_threshold, p.trail_duration);
    if (c.dof_survey) {
        std::printf("  DOF blur taps (c31..c38 xy, weight c47..c54 x):");
        for (int i = 0; i < 8; i++)
            std::printf(" (%.5f %.5f %.4f)", c.dof_offsets[i][0], c.dof_offsets[i][1],
                        c.dof_weights[i][0]);
        std::printf("\n");
    }
    if (c.bloom_survey) {
        std::printf("  bloom blur taps (c31..c45 xy, weight c47..c61 x):");
        for (int i = 0; i < 15; i++)
            std::printf(" (%.5f %.5f %.5f)", c.bloom_offsets[i][0], c.bloom_offsets[i][1],
                        c.bloom_weights[i][0]);
        std::printf("\n");
    }
}

// a pass's draws by draw mode (DrawItem::draw_mode) other than the colour
// pass's, and their cull modes and shaders' options: " mode 1 x12 (cull 6,
// SKINNED)"
std::string DrawModes(const FrameCapture& fc, const Pass& p) {
    std::map<int, std::pair<size_t, std::map<std::string, size_t>>> modes;
    const uint32_t end = std::min<uint32_t>(p.first_draw + p.draw_count, uint32_t(fc.draws.size()));
    for (uint32_t d = p.first_draw; d < end; d++) {
        const DrawItem& it = fc.draws[d];
        if (!it.draw_mode) continue;
        auto& m = modes[it.draw_mode];
        m.first++;
        const ShadeState* s = ShadeOf(fc, it);
        m.second["cull " + std::to_string(it.cull) + ", " +
                 (s ? OptionString(s->options) : std::string("no shade"))]++;
    }
    std::string out;
    for (const auto& [mode, m] : modes) {
        out += " mode " + std::to_string(mode) + " x" + std::to_string(m.first) + " (";
        bool first = true;
        for (const auto& [what, n] : m.second) {
            if (!first) out += "; ";
            first = false;
            out += what + (m.second.size() > 1 ? " x" + std::to_string(n) : "");
        }
        out += ")";
    }
    return out;
}

void PrintPasses(const FrameCapture& fc) {
    for (size_t i = 0; i < fc.passes.size(); i++) {
        const Pass& p = fc.passes[i];
        const uint32_t end = p.first_draw + p.draw_count;
        if (!p.tex_obj) {
            std::printf("pass %3zu back buffer draws %u..%u%s\n", i, p.first_draw, end,
                        DrawModes(fc, p).c_str());
            continue;
        }
        uint32_t rects = 0, mips = 0;
        for (uint32_t d = p.first_draw; d < end && d < fc.draws.size(); d++) {
            if (fc.draws[d].rect_shader >= 0) rects++;
            if (fc.draws[d].mip_level) mips++;
        }
        char clear[64] = "no clear";
        if (p.clear_flags)
            std::snprintf(clear, sizeof(clear), "clear %02X to %08X z %.0f", p.clear_flags,
                          p.clear_color, p.clear_z);
        std::printf("pass %3zu texture %08X %s %ux%u mips %u, draws %u..%u (%u rects, %u mip), "
                    "%s, viewport %.0f,%.0f %.0fx%.0f, cam %08X, version %u, frame %llu%s: %s%s\n",
                    i, p.tex_obj, TexTypeName(p.tex_type), p.width, p.height, p.num_mips,
                    p.first_draw, end, rects, mips, clear, p.viewport[0], p.viewport[1],
                    p.viewport[2], p.viewport[3], p.cam, p.version,
                    (unsigned long long)p.from_frame,
                    Carried(fc, p) ? " (carried)" : "",
                    p.name.empty() ? "-" : p.name.c_str(), DrawModes(fc, p).c_str());
    }
}

// The "shadow:" lines: each SHADOW_BUFFER draw's s5 against the shadow maps'
// passes, and its VS c40..c43 against the view-projection the pass that made
// its version drew with, times the texture's matrix (RB3's CheckShadow); and
// the shadow maps' passes' draws (their draw modes, cull modes, options).
void PrintShadowCheck(const FrameCapture& fc) {
    using namespace shader_opt;
    // the texture's: u = .5x + .5009765625w, v = -.5y + .5009765625w, z, w
    Mat4 t{};
    t.m[0][0] = 0.5f;
    t.m[1][1] = -0.5f;
    t.m[2][2] = 1.0f;
    t.m[3][0] = t.m[3][1] = 0.5009765625f;
    t.m[3][3] = 1.0f;
    size_t buffer_draws = 0, as_map = 0, other_s5 = 0, no_s5 = 0, just_before = 0, made = 0;
    float worst = 0, worst_rel = 0;
    float worst_col[4] = {};  // by column: u, v, z, w
    size_t checked = 0;
    size_t worst_draw = 0;
    for (size_t d = 0; d < fc.draws.size(); d++) {
        const ShadeState* s = ShadeOf(fc, fc.draws[d]);
        if (!s || !s->Option(kShadowBuffer)) continue;
        buffer_draws++;
        const Texture* map = ShadowMapOf(s);
        if (!map) {
            if (s->maps[kMapProjected]) other_s5++;
            else no_s5++;
            continue;
        }
        as_map++;
        // the pass that made its version, and the last shadow map pass before the draw
        const Pass* maker = nullptr;
        const Pass* last = nullptr;
        for (const Pass& p : fc.passes) {
            if (p.tex_obj == map->tex_obj && p.version == map->version) maker = &p;
            if (p.tex_type == kTexTypeShadowMap && p.first_draw + p.draw_count <= d) last = &p;
        }
        if (!maker) continue;
        made++;
        if (maker == last) just_before++;
        if (!maker->draw_count) continue;
        const Mat4& vp = fc.draws[maker->first_draw].view_proj;
        for (int i = 0; i < 4; i++) {
            const float* c = s->Vs(40 + i);
            for (int r = 0; r < 4; r++) {
                float want = 0;
                for (int k = 0; k < 4; k++) want += vp.m[r][k] * t.m[k][i];
                const float diff = std::fabs(c[r] - want);
                if (diff > worst) worst_draw = d;
                worst = std::max(worst, diff);
                worst_col[i] = std::max(worst_col[i], diff);
                worst_rel = std::max(worst_rel, diff / std::max(1.0f, std::fabs(want)));
            }
        }
        checked++;
    }
    size_t map_passes = 0, map_draws = 0;
    for (const Pass& p : fc.passes) {
        if (p.tex_type != kTexTypeShadowMap) continue;
        map_passes++;
        map_draws += p.draw_count;
    }
    if (!buffer_draws && !map_passes) return;
    std::printf("shadow: %zu shadow map passes, %zu draws; %zu SHADOW_BUFFER draws: s5 the shadow "
                "map %zu (of those, made by a pass here %zu, by the shadow map pass just before "
                "it %zu), s5 something else %zu, none %zu\n",
                map_passes, map_draws, buffer_draws, as_map, made, just_before, other_s5, no_s5);
    if (checked)
        std::printf("shadow: c40..c43 against the maker pass's view_proj x T over %zu draws: off "
                    "by up to %.6f (%.2e relative; u %.6f v %.6f z %.6f w %.6f), the most at "
                    "draw %zu\n",
                    checked, worst, worst_rel, worst_col[0], worst_col[1], worst_col[2],
                    worst_col[3], worst_draw);
}

// The "clear:" line, TheRnd's clear colour the back buffer starts as, and
// the "camera:" lines: the back buffer's cameras (FrameCapture::cameras),
// each one's viewport, z range and draws, and how the renderers layer them
// (soft_raster.h's LayoutBackBuffer): the reference camera's d = B + A/w,
// and each camera's first draw's DepthMap
void PrintCameras(const FrameCapture& fc) {
    if (fc.has_clear_color)
        std::printf("clear: %.3f %.3f %.3f %.3f (RGBA8 %08X)\n", fc.clear_color[0],
                    fc.clear_color[1], fc.clear_color[2], fc.clear_color[3], ClearRgba(fc));
    else
        std::printf("clear: none kept (a capture from before it): 0xff202020\n");
    if (fc.cameras.empty()) {
        std::printf("cameras: none kept (a capture from before them): depth cleared per camera\n");
        return;
    }
    const BackBufferLayout l = LayoutBackBuffer(fc);
    std::printf("cameras: %zu; depth %s", fc.cameras.size(),
                l.mapped ? "layered by z range" : "1/w (no perspective reference camera)");
    if (l.mapped)
        std::printf(", reference z = %.6f w + %.6f, zrange %.4f..%.4f: d = %.6f + %.6f / w",
                    l.ref_proj[0], l.ref_proj[1], l.ref_zrange[0], l.ref_zrange[1], l.ref_b,
                    l.ref_a);
    std::printf("\n");
    for (const CameraView& c : fc.cameras) {
        size_t draws = 0, first = fc.draws.size();
        for (size_t i = 0; i < fc.draws.size(); i++) {
            const DrawItem& d = fc.draws[i];
            if (d.cam != c.cam || d.target || d.rect_shader >= 0) continue;
            if (!draws++) first = i;
        }
        std::printf("  camera %08X viewport %.0f,%.0f %.0fx%.0f of %ux%u, zrange %.4f..%.4f, %zu "
                    "mesh draws",
                    c.cam, c.viewport[0], c.viewport[1], c.viewport[2], c.viewport[3], c.target_w,
                    c.target_h, c.zrange[0], c.zrange[1], draws);
        if (draws) {
            float vp[4];
            DepthMap dm;
            PlaceBackBufferDraw(l, fc, fc.draws[first], c.target_w, c.target_h, vp, dm);
            const Mat4& m = fc.draws[first].view_proj;
            std::printf(" from #%zu, depth (%.6f w + %.6f + %.6f z) / w; z column %.4f %.4f %.4f "
                        "%.4f, w column %.4f %.4f %.4f %.4f",
                        first, dm.p, dm.q, dm.r, m.m[0][2], m.m[1][2], m.m[2][2], m.m[3][2],
                        m.m[0][3], m.m[1][3], m.m[2][3], m.m[3][3]);
        }
        std::printf("\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    // an option where the output belongs would be taken for its name
    if (argc < 3 || std::strncmp(argv[2], "--", 2) == 0) {
        std::fprintf(stderr, "usage: replay <file.cap> <out.png> [options]\n");
        return 2;
    }
    auto fc = LoadCapture(argv[1]);
    if (!fc) {
        std::fprintf(stderr, "can't load %s\n", argv[1]);
        return 1;
    }
    RasterOptions o;
    bool transpose = false, per_cam = false, list = false, no_skinned = false, only_skinned = false;
    std::string compare, image, diff_with, dump_alpha, dump_depth, dump_bloom, dump_noise;
    Crop crop;
    long mesh_filter = -1, dump_tex = -1, shade_draw = -1;
    int dump_map = -1;  // --dump-tex's :<map>, -1 the diffuse texture
    long dump_level = 0;  // and its @<level>
    uint32_t dump_rt = 0, dump_rt_version = 0;
    int pick_x = -1, pick_y = -1;
    long cam_filter = -1;
    bool sized = false;
    float scale = 0;  // --scale's, 0 none
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--transpose") transpose = true;
        else if (a == "--per-cam") per_cam = true;
        else if (a == "--list") list = true;
        else if (a == "--compare" && i + 1 < argc) compare = argv[++i];
        else if (a == "--image" && i + 1 < argc) image = argv[++i];
        else if (a == "--diff" && i + 1 < argc) diff_with = argv[++i];
        else if (a == "--crop" && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%u,%u,%u,%u", &crop.x, &crop.y, &crop.w, &crop.h) != 4 ||
                !crop.w || !crop.h) {
                std::fprintf(stderr, "--crop takes x,y,w,h\n");
                return 2;
            }
        }
        else if (a == "--mesh" && i + 1 < argc) mesh_filter = std::strtol(argv[++i], nullptr, 16);
        else if (a == "--dump-tex" && i + 1 < argc) {
            char* end = nullptr;
            dump_tex = std::strtol(argv[++i], &end, 0);
            if (const char* at = std::strchr(argv[i], '@')) dump_level = std::strtol(at + 1, nullptr, 0);
            if (end && *end == ':') {
                const std::string name(end + 1, std::strcspn(end + 1, "@"));
                for (int m = 0; m < kNumShadeMaps; m++)
                    if (name == kMapNames[m]) dump_map = m;
                if (dump_map < 0) {
                    std::fprintf(stderr, "--dump-tex's map is one of --shade's names\n");
                    return 2;
                }
            }
        }
        else if (a == "--shade" && i + 1 < argc) shade_draw = std::strtol(argv[++i], nullptr, 0);
        else if (a == "--dump-rt" && i + 1 < argc) {
            char* end = nullptr;
            dump_rt = uint32_t(std::strtoul(argv[++i], &end, 16));
            if (end && *end == ':') dump_rt_version = uint32_t(std::strtoul(end + 1, nullptr, 0));
        }
        else if (a == "--rt-none") o.rt_guest_pixels = false;
        else if (a == "--rt-guest") o.texture_passes = false;
        else if (a == "--pick" && i + 1 < argc) std::sscanf(argv[++i], "%d,%d", &pick_x, &pick_y);
        else if (a == "--no-blend") o.blending = false;
        else if (a == "--no-cull") o.culling = false;
        else if (a == "--no-depth-clear") o.clear_depth_per_camera = false;
        else if (a == "--no-shadow") o.self_shadow = false;
        else if (a == "--no-normal") o.normal_maps = false;
        else if (a == "--no-default-mat") o.default_material = false;
        else if (a == "--nearest") o.filtering = false;
        else if (a == "--no-tex") o.textures = false;
        else if (a == "--legacy-light") o.legacy_light = true;
        else if (a == "--no-light") o.lighting = false;
        else if (a == "--no-skinned") no_skinned = true;
        else if (a == "--only-skinned") only_skinned = true;
        else if (a == "--unskinned") o.skinning = false;
        else if (a == "--cam" && i + 1 < argc) cam_filter = std::strtol(argv[++i], nullptr, 0);
        else if (a == "--size" && i + 1 < argc) {
            std::sscanf(argv[++i], "%ux%u", &o.width, &o.height);
            sized = true;
        }
        else if (a == "--scale" && i + 1 < argc) scale = std::strtof(argv[++i], nullptr);
        else if (a == "--shadow-scale" && i + 1 < argc)
            o.shadow_scale = std::strtof(argv[++i], nullptr);
        else if (a == "--msaa" && i + 1 < argc) {
            o.msaa = uint32_t(std::strtoul(argv[++i], nullptr, 0));
            if (o.msaa != 1 && o.msaa != 2 && o.msaa != 4) {
                std::fprintf(stderr, "--msaa takes 1, 2 or 4\n");
                return 2;
            }
        }
        else if (a == "--dump-alpha" && i + 1 < argc) dump_alpha = argv[++i];
        else if (a == "--dump-depth" && i + 1 < argc) dump_depth = argv[++i];
        else if (a == "--dump-bloom" && i + 1 < argc) dump_bloom = argv[++i];
        else if (a == "--dump-noise" && i + 1 < argc) dump_noise = argv[++i];
        else if (a == "--no-post") o.post = false;
        else if (a == "--no-grain") o.grain = false;
        else if (a == "--no-gamma") o.gamma = false;
        else if (a == "--gamma-from" && i + 1 < argc) {
            const auto other = LoadCapture(argv[++i]);
            if (!other) {
                std::fprintf(stderr, "can't load %s\n", argv[i]);
                return 1;
            }
            fc->gamma = other->gamma;
        }
        else if (a == "--post-only" && i + 1 < argc) {
            const std::string e = argv[++i];
            o.post_only = e == "xfm"     ? post::kPostXfm
                          : e == "dof"   ? post::kPostDof
                          : e == "bloom" ? post::kPostBloom | post::kPostGlare
                          : e == "spot"  ? post::kPostSpot
                          : e == "soft"  ? post::kPostSoft
                          : e == "noise" ? post::kPostNoise
                                         : 0;
            if (!o.post_only) {
                std::fprintf(stderr, "--post-only takes xfm, dof, bloom, spot, soft or noise\n");
                return 2;
            }
        }
        else if (a == "--view" && i + 1 < argc) {
            const std::string v = argv[++i];
            o.view = v == "alpha"   ? RasterView::kSceneAlpha
                     : v == "depth" ? RasterView::kSceneDepth
                                    : RasterView::kFinal;
        }
    }

    if (scale) {
        if (!(scale > 0) || scale > 8) {
            std::fprintf(stderr, "--scale takes a number above 0, at most 8\n");
            return 2;
        }
        o.target_scale = scale;
        if (!sized) {
            o.width = uint32_t(std::lround(post::kGameWidth * double(scale)));
            o.height = uint32_t(std::lround(post::kGameHeight * double(scale)));
        }
    }
    if (!(o.shadow_scale > 0) || o.shadow_scale > 8) {
        std::fprintf(stderr, "--shadow-scale takes a number above 0, at most 8\n");
        return 2;
    }

    // per-camera diagnostics, both matrix orders
    std::map<uint32_t, CamStats> cams;
    std::vector<uint32_t> order;
    size_t drawn = 0;
    for (const DrawItem& d : fc->draws) {
        if (!DrawnToBackBuffer(d) || d.rect_shader >= 0) continue;
        drawn++;
        auto [it, fresh] = cams.try_emplace(d.cam);
        if (fresh) order.push_back(d.cam);
        CamStats& cs = it->second;
        cs.draws++;
        cs.vp = d.view_proj;
        const Mat4 vps[2] = {d.view_proj, Transpose(d.view_proj)};
        const size_t step = std::max<size_t>(1, d.geom->verts.size() / 64);
        for (size_t v = 0; v < d.geom->verts.size(); v += step) {
            cs.verts++;
            for (int k = 0; k < 2; k++) {
                float c[4];
                Clip(d.geom->verts[v].pos, d.bones.empty() ? d.world : d.bones[0], vps[k], c);
                if (c[3] > 0) {
                    cs.front[k]++;
                    if (std::fabs(c[0]) <= c[3] && std::fabs(c[1]) <= c[3]) cs.inside[k]++;
                }
            }
        }
    }
    std::printf("frame %llu, %zu draws (%zu of them meshes to the back buffer), %zu cameras\n",
                (unsigned long long)fc->frame, fc->draws.size(), drawn, cams.size());
    PrintPassSummary(*fc);
    PrintPost(*fc);
    PrintGamma(*fc, list);
    if (list) {
        PrintCameras(*fc);
        PrintPasses(*fc);
        PrintShadowCheck(*fc);
    }
    for (uint32_t cam : order) {
        const CamStats& cs = cams[cam];
        std::printf("  cam 0x%08X: %d draws, %d sampled verts | as captured: %d front %d inside"
                    " | transposed: %d front %d inside\n",
                    cam, cs.draws, cs.verts, cs.front[0], cs.inside[0], cs.front[1],
                    cs.inside[1]);
        PrintMat(cs.vp);
    }
    PrintShadeSummary(*fc);

    if (pick_x >= 0 && pick_y >= 0) {
        if (uint32_t(pick_x) >= o.width || uint32_t(pick_y) >= o.height) {
            std::fprintf(stderr, "%d,%d is outside %ux%u\n", pick_x, pick_y, o.width, o.height);
            return 1;
        }
        // the last draw to write it, then while that one blends, the one
        // before it (drawn again without it), down to an opaque one
        FrameCapture f = *fc;
        std::vector<uint32_t> rgba;
        std::vector<int32_t> ids;
        const size_t at = size_t(pick_y) * o.width + pick_x;
        for (int layer = 0; layer < 8; layer++) {
            Rasterize(f, o, rgba, &ids);
            if (layer == 0) WritePng(argv[2], rgba, o.width, o.height);
            const uint32_t c = rgba[at];
            const int32_t id = ids[at];
            std::printf("pixel %d,%d: draw %d, colour %u %u %u\n", pick_x, pick_y, id, c & 0xff,
                        (c >> 8) & 0xff, (c >> 16) & 0xff);
            if (id < 0) break;
            PrintShade(f, size_t(id));
            if (f.draws[id].blend == 1 || !o.blending) break;
            // keep the indices the original frame's: an empty draw draws nothing
            f.draws[id].geom = std::make_shared<Geometry>();
        }
        return 0;
    }

    if (shade_draw >= 0) {
        if (size_t(shade_draw) >= fc->draws.size()) {
            std::fprintf(stderr, "there are %zu draws\n", fc->draws.size());
            return 1;
        }
        PrintShade(*fc, size_t(shade_draw));
        return 0;
    }

    if (list) {
        for (size_t i = 0; i < fc->draws.size(); i++) {
            const DrawItem& d = fc->draws[i];
            if (cam_filter >= 0 && d.cam != uint32_t(cam_filter)) continue;
            float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
            float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
            int front = 0;
            // a BILLBOARD draw lands turned to the camera, as the renderers
            // draw it (shade.hlsli's Billboard)
            shade::ShadeParams sp;
            shade::PackShade(d, ShadeOf(*fc, d), RasterOptions{}, false, sp);
            const bool billboard = (sp.flags.x & shade::kShadeBillboard) != 0;
            for (const Vertex& v : d.geom->verts) {
                for (int c = 0; c < 3; c++) { mn[c] = std::min(mn[c], v.pos[c]); mx[c] = std::max(mx[c], v.pos[c]); }
                float c4[4];
                if (billboard) {
                    float wp[3];
                    shade::BillboardCpu(sp, v.pos, d.world.m[3], wp);
                    const Mat4 identity{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}}};
                    Clip(wp, identity, d.view_proj, c4);
                } else {
                    Clip(v.pos, d.bones.empty() ? d.world : d.bones[v.bone[0] < d.bones.size() ? v.bone[0] : 0], d.view_proj, c4);
                }
                if (c4[3] > 0) {
                    front++;
                    for (int k = 0; k < 2; k++) { lo[k] = std::min(lo[k], c4[k] / c4[3]); hi[k] = std::max(hi[k], c4[k] / c4[3]); }
                }
            }
            const ShadeState* shade = ShadeOf(*fc, d);
            if (d.target || d.rect_shader >= 0) {
                std::printf("      ");
                if (d.target) std::printf("into %08X ", d.target);
                if (IsSpotCone(d, shade)) std::printf("cone ");
                if (IsSoftParticle(d, shade)) std::printf("soft ");
                if (d.rect_shader >= 0)
                    std::printf("rect shader %d [%.1f %.1f %.1f %.1f] ", d.rect_shader, d.rect[0],
                                d.rect[1], d.rect[2], d.rect[3]);
                // its corners' uv where the material's texture transform
                // moved them off 0..1 (top left, then bottom right)
                if (d.rect_shader >= 0 && d.geom && d.geom->verts.size() == 4) {
                    const float* a = d.geom->verts[0].uv;
                    const float* b = d.geom->verts[2].uv;
                    if (a[0] != 0 || a[1] != 0 || b[0] != 1 || b[1] != 1)
                        std::printf("uv %.3f,%.3f..%.3f,%.3f ", a[0], a[1], b[0], b[1]);
                }
                if (d.mip_level) std::printf("mip %d ", d.mip_level);
                if (d.draw_mode) std::printf("draw mode %u ", unsigned(d.draw_mode));
                std::printf("\n");
            }
            if (d.tex && d.tex->tex_obj)
                std::printf("      samples render target %08X type 0x%X version %u%s\n",
                            d.tex->tex_obj, d.tex->tex_type, d.tex->version,
                            d.tex->rgba.empty() ? " (no pixels)" : " (guest pixels)");
            std::printf("#%3zu mesh %08X v%5zu t%5zu bones %2zu blend %d z %d cull %u cut %d/%d prelit %d tex %s%ux%u fmt %u | local [%.1f %.1f %.1f]..[%.1f %.1f %.1f] | ndc x %.2f..%.2f y %.2f..%.2f front %d | col %.2f %.2f %.2f %.2f\n",
                i, d.mesh, d.geom->verts.size(), d.geom->indices.size() / 3, d.bones.size(), d.blend, d.z_mode,
                unsigned(d.cull), int(d.alpha_cut), d.alpha_threshold, int(d.prelit),
                d.tex ? "" : "-", d.tex ? d.tex->width : 0, d.tex ? d.tex->height : 0, d.tex ? d.tex->format : 0,
                mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], lo[0], hi[0], lo[1], hi[1], front,
                d.color[0], d.color[1], d.color[2], d.color[3]);
            if (const ShadeState* s = shade) {
                const float* c1 = s->Vs(1);
                // the pass's material and the next pass's (a draw of its
                // own in captures since passes were: the same mesh after)
                std::printf("      opt %016llX type %d shade %d env %u | c1 %.2f %.2f %.2f %.2f "
                            "| box %.2f | points %u/%d | mat %08X next_pass %08X\n",
                            (unsigned long long)s->options, s->shader_type, d.shade,
                            unsigned(s->use_environ), c1[0], c1[1], c1[2], c1[3], BoxSum(*s),
                            s->OptionBits(shader_opt::kNumPoint, 2), PointsLit(*s), s->mat,
                            s->next_pass);
                // particles: the quad axes' lengths (VS c47, c48) and the
                // first quad's sides along them (corners 0 to 3, 0 to 1)
                if (s->shader_type == 14 && d.geom->verts.size() >= 4) {
                    auto len = [](const float* a, const float* b) {
                        float sum = 0;
                        for (int c = 0; c < 3; c++) sum += (a[c] - b[c]) * (a[c] - b[c]);
                        return std::sqrt(sum);
                    };
                    const float zero[3] = {};
                    const auto& q = d.geom->verts;
                    std::printf("      particles: |c47| %.3f |c48| %.3f c49 %.2f %.2f %.2f %.2f "
                                "| first quad %.2f x %.2f\n",
                                len(s->Vs(47), zero), len(s->Vs(48), zero), s->Vs(49)[0],
                                s->Vs(49)[1], s->Vs(49)[2], s->Vs(49)[3],
                                len(q[3].pos, q[0].pos), len(q[1].pos, q[0].pos));
                }
            }
        }
    }

    // the scene target's alpha or depth as grey, and how much of it is lit
    auto dump_view = [&](const std::string& path, RasterView view) {
        RasterOptions vo = o;
        vo.view = view;
        std::vector<uint32_t> px;
        const RasterStats rs = Rasterize(*fc, vo, px);
        WritePng(path, px, vo.width, vo.height);
        size_t lit = 0, bright = 0;
        double sum = 0;
        for (uint32_t c : px) {
            const uint32_t g = c & 0xff;
            sum += g;
            if (g) lit++;
            if (g >= 0x80) bright++;
        }
        const double n = double(px.size());
        std::printf("%s: scene %s, %ux%u: %.1f%% of pixels above 0, %.1f%% at 0.5 or more, mean "
                    "%.1f of 255; %u draws, %.1f ms\n",
                    path.c_str(), view == RasterView::kSceneAlpha ? "alpha" : "depth", vo.width,
                    vo.height, 100.0 * double(lit) / n, 100.0 * double(bright) / n, sum / n,
                    rs.draws, rs.ms);
    };
    if (!dump_bloom.empty()) {
        std::vector<uint32_t> px, level;
        RasterOptions bo = o;
        bo.post_bloom0 = &level;
        Rasterize(*fc, bo, px);
        if (level.empty()) {
            std::fprintf(stderr, "the frame has no bloom or glare\n");
            return 1;
        }
        for (uint32_t& c : level) c |= 0xff000000u;  // alpha off, as --dump-tex
        WritePng(dump_bloom, level, post::Quarter(bo.width), post::Quarter(bo.height));
        std::printf("%s: bloom level 0, %ux%u\n", dump_bloom.c_str(), post::Quarter(bo.width),
                    post::Quarter(bo.height));
        return 0;
    }
    if (!dump_noise.empty()) {
        const Texture* t = fc->noise_map.get();
        if (!t || t->rgba.empty()) {
            std::fprintf(stderr, "the capture kept no noise map\n");
            return 1;
        }
        // level 0 there, and each mip beside it as <png>.<level>.png
        const std::string stem = dump_noise.substr(0, dump_noise.size() - 4);
        for (size_t l = 0; l <= t->mips.size(); l++) {
            std::vector<uint32_t> px = l ? t->mips[l - 1] : t->rgba;
            for (uint32_t& c : px) c |= 0xff000000u;  // alpha off, as --dump-tex
            const uint32_t w = std::max(t->width >> l, 1u), h = std::max(t->height >> l, 1u);
            if (px.size() != size_t(w) * h) break;
            WritePng(l ? stem + "." + std::to_string(l) + ".png" : dump_noise, px, w, h);
        }
        std::printf("%s: the noise map, %ux%u format %u, %zu mips (beside it)\n",
                    dump_noise.c_str(), t->width, t->height, t->format, t->mips.size());
        return 0;
    }
    if (!dump_alpha.empty() || !dump_depth.empty()) {
        if (!dump_alpha.empty()) dump_view(dump_alpha, RasterView::kSceneAlpha);
        if (!dump_depth.empty()) dump_view(dump_depth, RasterView::kSceneDepth);
        return 0;
    }

    if (dump_rt) {
        std::vector<uint32_t> px;
        uint32_t w = 0, h = 0;
        RasterStats rs;
        if (!RasterizeTarget(*fc, o, dump_rt, dump_rt_version, px, w, h, &rs)) {
            std::fprintf(stderr, "no pass in the capture draws %08X\n", dump_rt);
            return 1;
        }
        // colour over black where it's transparent, as the texture's alpha
        // says, and the alpha on its own
        std::vector<uint32_t> alpha(px.size());
        size_t opaque = 0;
        for (size_t i = 0; i < px.size(); i++) {
            const uint32_t a = px[i] >> 24;
            alpha[i] = a | a << 8 | a << 16 | 0xff000000u;
            if (a >= 0x80) opaque++;
        }
        for (uint32_t& p : px) p |= 0xff000000u;
        std::string alpha_path = argv[2];
        alpha_path = alpha_path.substr(0, alpha_path.size() - 4) + ".alpha.png";
        WritePng(argv[2], px, w, h);
        WritePng(alpha_path, alpha, w, h);
        std::printf("%s (and %s): %08X, %ux%u, %.1f%% of it alpha >= 0.5; %u texture passes, %u "
                    "draws, %u sampled a render target nothing drew, %.1f ms\n",
                    argv[2], alpha_path.c_str(), dump_rt, w, h,
                    100.0 * double(opaque) / double(px.size()), rs.passes, rs.draws,
                    rs.rt_missing, rs.ms);
        return 0;
    }

    if (dump_tex >= 0) {
        const DrawItem* d = size_t(dump_tex) < fc->draws.size() ? &fc->draws[dump_tex] : nullptr;
        const ShadeState* s = d ? ShadeOf(*fc, *d) : nullptr;
        const Texture* tp = !d              ? nullptr
                            : dump_map >= 0 ? (s ? s->maps[dump_map].get() : nullptr)
                                            : d->tex.get();
        if (!tp) {
            std::fprintf(stderr, "draw %ld has no %s\n", dump_tex,
                         dump_map >= 0 ? kMapNames[dump_map] : "texture");
            return 1;
        }
        Texture t = *tp;
        if (t.rgba.empty()) {
            std::fprintf(stderr, "draw %ld samples render target %08X version %u, kept without "
                         "pixels\n", dump_tex, t.tex_obj, t.version);
            return 1;
        }
        // a mip level instead of the base
        if (dump_level > 0) {
            if (size_t(dump_level) > t.mips.size()) {
                std::fprintf(stderr, "it has %zu mip levels\n", t.mips.size());
                return 1;
            }
            t.rgba = t.mips[dump_level - 1];
            t.width = std::max(1u, t.width >> dump_level);
            t.height = std::max(1u, t.height >> dump_level);
        }
        // alpha off, to see the colour, and on its own as grey
        std::vector<uint32_t> px = t.rgba, alpha(t.rgba.size());
        for (size_t i = 0; i < px.size(); i++) {
            const uint32_t a = px[i] >> 24;
            alpha[i] = a | a << 8 | a << 16 | 0xff000000u;
            px[i] |= 0xff000000u;
        }
        std::string alpha_path = argv[2];
        alpha_path = alpha_path.substr(0, alpha_path.size() - 4) + ".alpha.png";
        WritePng(argv[2], px, t.width, t.height);
        WritePng(alpha_path, alpha, t.width, t.height);
        std::printf("%s (and %s): %ux%u format %u", argv[2], alpha_path.c_str(), t.width,
                    t.height, t.format);
        if (t.tex_obj) std::printf(", render target %08X version %u", t.tex_obj, t.version);
        std::printf("\n");
        return 0;
    }

    auto render = [&](const std::string& path, long only_cam) {
        // the back buffer's draws filtered out are left empty, so the passes'
        // draw numbers stay right; the texture passes' are all kept
        FrameCapture f = *fc;
        for (DrawItem& c : f.draws) {
            if (!DrawnToBackBuffer(c)) continue;
            if ((only_cam >= 0 && c.cam != uint32_t(only_cam)) ||
                (no_skinned && !c.bones.empty()) ||
                (mesh_filter >= 0 && c.mesh != uint32_t(mesh_filter)) ||
                (only_skinned && c.bones.empty())) {
                c.geom = std::make_shared<Geometry>();
                continue;
            }
            if (transpose) c.view_proj = Transpose(c.view_proj);
        }
        std::vector<uint32_t> rgba;
        const RasterStats rs = Rasterize(f, o, rgba);
        WritePng(path, rgba, o.width, o.height);
        std::printf("%s: %u draws (%u texture passes, %u sampled a render target nothing drew), "
                    "%u tris, %u pixels, %.1f ms\n",
                    path.c_str(), rs.draws, rs.passes, rs.rt_missing, rs.triangles, rs.pixels,
                    rs.ms);
    };
    // the frame drawn on the CPU at a size, with the options above
    auto rasterize_at = [&](uint32_t w, uint32_t h, std::vector<uint32_t>& out) {
        o.width = w;
        o.height = h;
        FrameCapture f = *fc;
        if (transpose)
            for (DrawItem& d : f.draws) d.view_proj = Transpose(d.view_proj);
        return Rasterize(f, o, out);
    };
    if (!compare.empty()) {
        // the game's own frame (a harness `capture`) left, the native one right,
        // both at half the screenshot's size
        std::vector<uint32_t> shot;
        uint32_t sw = 0, sh = 0;
        if (!ReadPng(compare, shot, sw, sh)) {
            std::fprintf(stderr, "can't read %s (only PNGs band3 wrote)\n", compare.c_str());
            return 1;
        }
        std::vector<uint32_t> native;
        std::string drawn;
        if (!image.empty()) {
            uint32_t iw = 0, ih = 0;
            if (!ReadPng(image, native, iw, ih)) {
                std::fprintf(stderr, "can't read %s (only PNGs band3 wrote)\n", image.c_str());
                return 1;
            }
            if (iw != sw || ih != sh) {
                std::fprintf(stderr, "%s is %ux%u, the screenshot %ux%u\n", image.c_str(), iw,
                             ih, sw, sh);
                return 1;
            }
            drawn = image;
        } else {
            const RasterStats rs = rasterize_at(sw, sh, native);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%u draws, %u texture passes, %u rt missing, %.1f ms",
                          rs.draws, rs.passes, rs.rt_missing, rs.ms);
            drawn = buf;
        }
        const uint32_t hw = sw / 2, hh = sh / 2;
        std::vector<uint32_t> side(size_t(hw) * 2 * hh);
        for (uint32_t y = 0; y < hh; y++) {
            for (uint32_t x = 0; x < hw; x++) {
                side[size_t(y) * hw * 2 + x] = shot[size_t(y * 2) * sw + x * 2] | 0xff000000u;
                side[size_t(y) * hw * 2 + hw + x] =
                    native[size_t(y * 2) * sw + x * 2] | 0xff000000u;
            }
        }
        WritePng(argv[2], side, hw * 2, hh);
        // over the pixels the halved picture shows
        const DiffStats ds = Measure(shot, native, sw, sh, 2, crop);
        std::printf("%s: game | native, %s; mean difference %.1f of 255\n", argv[2],
                    drawn.c_str(), ds.mean);
        PrintDiffStats(ds, crop);
        return 0;
    }
    if (!diff_with.empty()) {
        // another picture of this capture (the GPU backend's) against the
        // CPU's, every pixel
        std::vector<uint32_t> other;
        uint32_t w = 0, h = 0;
        if (!ReadPng(diff_with, other, w, h)) {
            std::fprintf(stderr, "can't read %s (only PNGs band3 wrote)\n", diff_with.c_str());
            return 1;
        }
        std::vector<uint32_t> cpu;
        const RasterStats rs = rasterize_at(w, h, cpu);
        WritePng(argv[2], cpu, w, h);
        // the CPU's picture is the native one here: signed is it minus the PNG
        const DiffStats ds = Measure(other, cpu, w, h, 1, crop);
        std::printf("%s: cpu, %u draws, %u texture passes, %u rt missing, %.1f ms; against %s: "
                    "mean difference %.2f of 255, %.2f%% of pixels off by more than 8\n",
                    argv[2], rs.draws, rs.passes, rs.rt_missing, rs.ms, diff_with.c_str(),
                    ds.mean, ds.over8);
        PrintDiffStats(ds, crop);
        return 0;
    }
    render(argv[2], cam_filter);
    if (per_cam) {
        for (size_t i = 0; i < order.size(); i++) {
            std::string p = argv[2];
            p = p.substr(0, p.size() - 4) + ".cam" + std::to_string(i) + ".png";
            render(p, long(order[i]));
        }
    }
    return 0;
}
