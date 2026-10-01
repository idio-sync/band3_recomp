// Experimental: draws a native view capture (BAND3_NATIVE_VIEW_DUMP) offline.
//
//   replay <file.cap> <out.png> [--size WxH] [--cam N] [--per-cam] [--list]
//                               [--compare <screenshot.png> [--image <native.png>]]
//                               [--diff <native.png>] [--mesh <hex>]
//                               [--dump-tex <draw>] [--shade <draw>]
//                               [--no-tex] [--no-blend] [--transpose]
//                               [--no-skinned | --only-skinned] [--unskinned]
//                               [--legacy-light | --no-light] [--pick X,Y]
//
// Prints, for each camera, how many of its vertices land in front of the camera
// and inside the frustum with the matrix as captured and transposed. --list
// prints every draw: its mesh, sizes, material and where it lands on screen,
// and what its shader was given (option word, shade, ambient c1, the box map's
// sum, point lights in the option word / with a colour); --shade prints all of
// one draw's ShadeState. A capture with shades also gets a summary of them.
// --compare draws the frame at the size of a harness `capture` screenshot and
// writes the two side by side (game left, native right), with their mean
// difference; with --image the native side is that PNG instead (a harness
// capture's <name>.gpu.png, the GPU backend's picture). --diff draws the frame
// on the CPU at that PNG's size into out.png and prints their mean difference,
// to check the GPU backend against the CPU's reference. --legacy-light draws
// with the placeholder lighting from before the game's shading, --no-light
// with none (every material unlit). --pick draws the frame at --size and
// prints the draw that last wrote pixel X,Y, its colour and its shade.
//
// Build (from the repository root):
//   clang++ -std=c++20 -O2 -I. tools/native_view_replay/replay.cpp
//     src/Render/soft_raster.cpp src/Render/shade_model.cpp src/Render/capture_file.cpp
//     src/Render/png_writer.cpp -o out/native_view_replay.exe

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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
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
    std::string compare, image, diff_with;
    long mesh_filter = -1, dump_tex = -1, shade_draw = -1;
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
        else if (a == "--pick" && i + 1 < argc) std::sscanf(argv[++i], "%d,%d", &pick_x, &pick_y);
        else if (a == "--no-blend") o.blending = false;
        else if (a == "--no-tex") o.textures = false;
        else if (a == "--legacy-light") o.legacy_light = true;
        else if (a == "--no-light") o.lighting = false;
        else if (a == "--no-skinned") no_skinned = true;
        else if (a == "--only-skinned") only_skinned = true;
        else if (a == "--unskinned") o.skinning = false;
        else if (a == "--cam" && i + 1 < argc) cam_filter = std::strtol(argv[++i], nullptr, 0);
        else if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%ux%u", &o.width, &o.height);
    }

    // per-camera diagnostics, both matrix orders
    std::map<uint32_t, CamStats> cams;
    std::vector<uint32_t> order;
    for (const DrawItem& d : fc->draws) {
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
    std::printf("frame %llu, %zu draws, %zu cameras\n", (unsigned long long)fc->frame,
                fc->draws.size(), cams.size());
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
            std::printf("#%3zu mesh %08X v%5zu t%5zu bones %2zu blend %d z %d cut %d/%d prelit %d tex %s%ux%u fmt %u | local [%.1f %.1f %.1f]..[%.1f %.1f %.1f] | ndc x %.2f..%.2f y %.2f..%.2f front %d | col %.2f %.2f %.2f %.2f\n",
                i, d.mesh, d.geom->verts.size(), d.geom->indices.size() / 3, d.bones.size(), d.blend, d.z_mode,
                int(d.alpha_cut), d.alpha_threshold, int(d.prelit),
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

    if (dump_tex >= 0) {
        if (size_t(dump_tex) >= fc->draws.size() || !fc->draws[dump_tex].tex) {
            std::fprintf(stderr, "draw %ld has no texture\n", dump_tex);
            return 1;
        }
        const Texture& t = *fc->draws[dump_tex].tex;
        std::vector<uint32_t> px = t.rgba;
        for (uint32_t& p : px) p |= 0xff000000u;  // alpha off, to see the colour
        WritePng(argv[2], px, t.width, t.height);
        std::printf("%s: %ux%u format %u\n", argv[2], t.width, t.height, t.format);
        return 0;
    }

    auto render = [&](const std::string& path, long only_cam) {
        FrameCapture f = *fc;
        f.draws.clear();
        for (const DrawItem& d : fc->draws) {
            if (only_cam >= 0 && d.cam != uint32_t(only_cam)) continue;
            if (no_skinned && !d.bones.empty()) continue;
            if (mesh_filter >= 0 && d.mesh != uint32_t(mesh_filter)) continue;
            if (only_skinned && d.bones.empty()) continue;
            DrawItem c = d;
            if (transpose) c.view_proj = Transpose(c.view_proj);
            f.draws.push_back(std::move(c));
        }
        std::vector<uint32_t> rgba;
        const RasterStats rs = Rasterize(f, o, rgba);
        WritePng(path, rgba, o.width, o.height);
        std::printf("%s: %u draws, %u tris, %u pixels, %.1f ms\n", path.c_str(), rs.draws,
                    rs.triangles, rs.pixels, rs.ms);
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
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%u draws, %.1f ms", rs.draws, rs.ms);
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
        std::printf("%s: cpu, %u draws, %.1f ms; against %s: mean difference %.2f of 255, "
                    "%.2f%% of pixels off by more than 8\n",
                    argv[2], rs.draws, rs.ms, diff_with.c_str(), diff / (double(cpu.size()) * 3),
                    100.0 * double(differ) / double(cpu.size()));
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
