// Experimental: draws a native view capture (BAND3_NATIVE_VIEW_DUMP) offline.
//
//   replay <file.cap> <out.png> [--size WxH] [--cam N] [--per-cam] [--list]
//                               [--compare <screenshot.png> [--image <native.png>]]
//                               [--diff <native.png>] [--mesh <hex>]
//                               [--dump-tex <draw>] [--shade <draw>]
//                               [--dump-rt <hex>[:<version>]]
//                               [--rt-none | --rt-guest]
//                               [--no-tex] [--no-blend] [--no-cull] [--transpose]
//                               [--no-skinned | --only-skinned] [--unskinned]
//                               [--legacy-light | --no-light] [--pick X,Y]
//                               [--dump-alpha <png>] [--dump-depth <png>]
//                               [--view alpha|depth]
//                               [--no-post | --post-only xfm|dof|bloom]
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
// DrawRect quad says so. --shade prints all of one draw's ShadeState. A
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
// --compare draws the frame at the size of a harness `capture` screenshot and
// writes the two side by side (game left, native right), with their mean
// difference; with --image the native side is that PNG instead (a harness
// capture's <name>.gpu.png, the GPU backend's picture). --diff draws the frame
// on the CPU at that PNG's size into out.png and prints their mean difference,
// to check the GPU backend against the CPU's reference. --legacy-light draws
// with the placeholder lighting from before the game's shading, --no-light
// with none (every material unlit). --no-cull draws both sides of every
// triangle, as the native view did before it culled as the game does (the
// cull mode --list prints, scene_capture.h's DrawItem::cull). --pick draws the frame at --size and
// prints the draw that last wrote pixel X,Y, its colour and its shade.
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
// RB3's post-processing (post_model.h: depth of field, bloom or glare, the
// colour matrix) is applied as the frame set it; --no-post leaves the scene
// as it is, --post-only applies one effect alone (bloom covers glare), to
// see what each contributes.
//
// Build (from the repository root):
//   clang++ -std=c++20 -O2 -I. tools/native_view_replay/replay.cpp
//     src/Render/soft_raster.cpp src/Render/shade_model.cpp src/Render/post_model.cpp
//     src/Render/capture_file.cpp src/Render/png_writer.cpp -o out/native_view_replay.exe

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
    char buf[96];
    std::snprintf(buf, sizeof(buf), "fmt %u dim %u %ux%u base %08X", f[1] & 0x3f,
                  (f[5] >> 9) & 3, (f[2] & 0x1fff) + 1, ((f[2] >> 13) & 0x1fff) + 1,
                  f[1] & 0xfffff000u);
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
    for (const DrawItem& d : fc.draws) {
        const ShadeState* s = ShadeOf(fc, d);
        if (!s) continue;
        with++;
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
        // particles' draws say prelit for their vertex colour
        if (s->shader_type != 14 && s->Option(kPrelit) != d.prelit) prelit_bit++;
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
    for (int m = 0; m < kNumShadeMaps; m++) {
        std::printf("  s%-2u %s: bound %s, material %08X (base %08X)", kShadeMapSampler[m],
                    kMapNames[m], FetchString(s->fetch[m]).c_str(), s->mat_maps[m],
                    s->mat_map_base[m]);
        if (s->maps[m]) std::printf(", decoded %ux%u", s->maps[m]->width, s->maps[m]->height);
        std::printf("\n");
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
    // what the native composite leaves out: velocity blur (s10 * c122), the
    // overlay (s12 * (c127.x + c127.y * s5.x) * c91.x) and noise (c112, c113)
    // (out/research/m4_design.md)
    const auto flag = [&](int offset) { return unsigned(c.flags[offset - kPostFlagBase]); };
    std::printf("  left out: velocity +0x38 %02X +0x39 %02X c122 %.4f %.4f %.4f %.4f; overlay "
                "+0x3F %02X c127 %.4f %.4f %.4f %.4f c91 %.4f %.4f %.4f %.4f; noise +0x2D %02X "
                "c112 %.4f %.4f %.4f %.4f c113 %.4f %.4f %.4f %.4f\n",
                flag(0x38), flag(0x39), c.c122[0], c.c122[1], c.c122[2], c.c122[3], flag(0x3F),
                c.c127[0], c.c127[1], c.c127[2], c.c127[3], c.c91[0], c.c91[1], c.c91[2],
                c.c91[3], flag(0x2D), c.c112[0], c.c112[1], c.c112[2], c.c112[3], c.c113[0],
                c.c113[1], c.c113[2], c.c113[3]);
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

void PrintPasses(const FrameCapture& fc) {
    for (size_t i = 0; i < fc.passes.size(); i++) {
        const Pass& p = fc.passes[i];
        const uint32_t end = p.first_draw + p.draw_count;
        if (!p.tex_obj) {
            std::printf("pass %3zu back buffer draws %u..%u\n", i, p.first_draw, end);
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
                    "%s, viewport %.0f,%.0f %.0fx%.0f, cam %08X, version %u, frame %llu%s: %s\n",
                    i, p.tex_obj, TexTypeName(p.tex_type), p.width, p.height, p.num_mips,
                    p.first_draw, end, rects, mips, clear, p.viewport[0], p.viewport[1],
                    p.viewport[2], p.viewport[3], p.cam, p.version,
                    (unsigned long long)p.from_frame,
                    Carried(fc, p) ? " (carried)" : "",
                    p.name.empty() ? "-" : p.name.c_str());
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
    std::string compare, image, diff_with, dump_alpha, dump_depth;
    long mesh_filter = -1, dump_tex = -1, shade_draw = -1;
    uint32_t dump_rt = 0, dump_rt_version = 0;
    int pick_x = -1, pick_y = -1;
    long cam_filter = -1;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--transpose") transpose = true;
        else if (a == "--per-cam") per_cam = true;
        else if (a == "--list") list = true;
        else if (a == "--compare" && i + 1 < argc) compare = argv[++i];
        else if (a == "--image" && i + 1 < argc) image = argv[++i];
        else if (a == "--diff" && i + 1 < argc) diff_with = argv[++i];
        else if (a == "--mesh" && i + 1 < argc) mesh_filter = std::strtol(argv[++i], nullptr, 16);
        else if (a == "--dump-tex" && i + 1 < argc) dump_tex = std::strtol(argv[++i], nullptr, 0);
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
        else if (a == "--no-tex") o.textures = false;
        else if (a == "--legacy-light") o.legacy_light = true;
        else if (a == "--no-light") o.lighting = false;
        else if (a == "--no-skinned") no_skinned = true;
        else if (a == "--only-skinned") only_skinned = true;
        else if (a == "--unskinned") o.skinning = false;
        else if (a == "--cam" && i + 1 < argc) cam_filter = std::strtol(argv[++i], nullptr, 0);
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%ux%u", &o.width, &o.height);
        else if (a == "--dump-alpha" && i + 1 < argc) dump_alpha = argv[++i];
        else if (a == "--dump-depth" && i + 1 < argc) dump_depth = argv[++i];
        else if (a == "--no-post") o.post = false;
        else if (a == "--post-only" && i + 1 < argc) {
            const std::string e = argv[++i];
            o.post_only = e == "xfm"   ? post::kPostXfm
                          : e == "dof" ? post::kPostDof
                          : e == "bloom" ? post::kPostBloom | post::kPostGlare
                                         : 0;
            if (!o.post_only) {
                std::fprintf(stderr, "--post-only takes xfm, dof or bloom\n");
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

    // per-camera diagnostics, both matrix orders
    std::map<uint32_t, CamStats> cams;
    std::vector<uint32_t> order;
    size_t drawn = 0;
    for (const DrawItem& d : fc->draws) {
        if (!DrawnToBackBuffer(d)) continue;
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
    if (list) PrintPasses(*fc);
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
            for (const Vertex& v : d.geom->verts) {
                for (int c = 0; c < 3; c++) { mn[c] = std::min(mn[c], v.pos[c]); mx[c] = std::max(mx[c], v.pos[c]); }
                float c4[4];
                Clip(v.pos, d.bones.empty() ? d.world : d.bones[v.bone[0] < d.bones.size() ? v.bone[0] : 0], d.view_proj, c4);
                if (c4[3] > 0) {
                    front++;
                    for (int k = 0; k < 2; k++) { lo[k] = std::min(lo[k], c4[k] / c4[3]); hi[k] = std::max(hi[k], c4[k] / c4[3]); }
                }
            }
            if (d.target || d.rect_shader >= 0) {
                std::printf("      ");
                if (d.target) std::printf("into %08X ", d.target);
                if (d.rect_shader >= 0)
                    std::printf("rect shader %d [%.1f %.1f %.1f %.1f] ", d.rect_shader, d.rect[0],
                                d.rect[1], d.rect[2], d.rect[3]);
                if (d.mip_level) std::printf("mip %d ", d.mip_level);
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
            if (const ShadeState* s = ShadeOf(*fc, d)) {
                const float* c1 = s->Vs(1);
                std::printf("      opt %016llX type %d shade %d env %u | c1 %.2f %.2f %.2f %.2f "
                            "| box %.2f | points %u/%d\n",
                            (unsigned long long)s->options, s->shader_type, d.shade,
                            unsigned(s->use_environ), c1[0], c1[1], c1[2], c1[3], BoxSum(*s),
                            s->OptionBits(shader_opt::kNumPoint, 2), PointsLit(*s));
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
        if (size_t(dump_tex) >= fc->draws.size() || !fc->draws[dump_tex].tex) {
            std::fprintf(stderr, "draw %ld has no texture\n", dump_tex);
            return 1;
        }
        const Texture& t = *fc->draws[dump_tex].tex;
        if (t.rgba.empty()) {
            std::fprintf(stderr, "draw %ld samples render target %08X version %u, kept without "
                         "pixels\n", dump_tex, t.tex_obj, t.version);
            return 1;
        }
        std::vector<uint32_t> px = t.rgba;
        for (uint32_t& p : px) p |= 0xff000000u;  // alpha off, to see the colour
        WritePng(argv[2], px, t.width, t.height);
        std::printf("%s: %ux%u format %u\n", argv[2], t.width, t.height, t.format);
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
        double diff = 0;
        for (uint32_t y = 0; y < hh; y++) {
            for (uint32_t x = 0; x < hw; x++) {
                const uint32_t a = shot[size_t(y * 2) * sw + x * 2];
                const uint32_t b = native[size_t(y * 2) * sw + x * 2];
                side[size_t(y) * hw * 2 + x] = a | 0xff000000u;
                side[size_t(y) * hw * 2 + hw + x] = b | 0xff000000u;
                for (int c = 0; c < 3; c++)
                    diff += std::abs(int((a >> (8 * c)) & 0xff) - int((b >> (8 * c)) & 0xff));
            }
        }
        WritePng(argv[2], side, hw * 2, hh);
        std::printf("%s: game | native, %s; mean difference %.1f of 255\n", argv[2],
                    drawn.c_str(), diff / (double(hw) * hh * 3));
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
        double diff = 0;
        size_t differ = 0;
        for (size_t i = 0; i < cpu.size(); i++) {
            int most = 0;
            for (int c = 0; c < 3; c++) {
                const int d = std::abs(int((cpu[i] >> (8 * c)) & 0xff) -
                                       int((other[i] >> (8 * c)) & 0xff));
                diff += d;
                most = std::max(most, d);
            }
            if (most > 8) differ++;
        }
        std::printf("%s: cpu, %u draws, %u texture passes, %u rt missing, %.1f ms; against %s: "
                    "mean difference %.2f of 255, %.2f%% of pixels off by more than 8\n",
                    argv[2], rs.draws, rs.passes, rs.rt_missing, rs.ms, diff_with.c_str(),
                    diff / (double(cpu.size()) * 3), 100.0 * double(differ) / double(cpu.size()));
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
