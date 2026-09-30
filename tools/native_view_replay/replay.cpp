// Experimental: draws a native view capture (BAND3_NATIVE_VIEW_DUMP) offline.
//
//   replay <file.cap> <out.png> [--size WxH] [--cam N] [--per-cam] [--list]
//                               [--no-tex] [--no-blend] [--transpose]
//                               [--no-skinned | --only-skinned] [--unskinned]
//
// Prints, for each camera, how many of its vertices land in front of the camera
// and inside the frustum with the matrix as captured and transposed. --list
// prints every draw: its mesh, sizes, material and where it lands on screen.
//
// Build (from the repository root):
//   clang++ -std=c++20 -O2 -I. tools/native_view_replay/replay.cpp
//     src/Render/soft_raster.cpp src/Render/capture_file.cpp src/Render/png_writer.cpp
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
    long cam_filter = -1;
    for (int i = 3; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--transpose") transpose = true;
        else if (a == "--per-cam") per_cam = true;
        else if (a == "--list") list = true;
        else if (a == "--no-blend") o.blending = false;
        else if (a == "--no-tex") o.textures = false;
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
            std::printf("#%3zu mesh %08X v%5zu t%5zu bones %2zu blend %d z %d tex %s%ux%u | local [%.1f %.1f %.1f]..[%.1f %.1f %.1f] | ndc x %.2f..%.2f y %.2f..%.2f front %d | col %.2f %.2f %.2f %.2f\n",
                i, d.mesh, d.geom->verts.size(), d.geom->indices.size() / 3, d.bones.size(), d.blend, d.z_mode,
                d.tex ? "" : "-", d.tex ? d.tex->width : 0, d.tex ? d.tex->height : 0,
                mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], lo[0], hi[0], lo[1], hi[1], front,
                d.color[0], d.color[1], d.color[2], d.color[3]);
        }
    }

    auto render = [&](const std::string& path, long only_cam) {
        FrameCapture f = *fc;
        f.draws.clear();
        for (const DrawItem& d : fc->draws) {
            if (only_cam >= 0 && d.cam != uint32_t(only_cam)) continue;
            if (no_skinned && !d.bones.empty()) continue;
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
