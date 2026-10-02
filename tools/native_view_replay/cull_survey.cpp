// Experimental: the free-camera experiment's survey. For each capture, finds
// the world camera (the back buffer's camera with the most mesh draws), splits
// it into pose and projection (free_cam.h), and prints as JSON each of its
// draws: the mesh, whether it's skinned, its bounds in the world, and where it
// lies against the camera's frustum (every vertex outside one plane: wholly
// outside; else how many are inside). tools/native_view_replay/cull_survey.py
// reads it.
//
//   cull_survey <file.cap>... > survey.json
//
// Build (Git Bash, LLVM's clang++ on the PATH):
//   clang++ -std=c++20 -O2 -I. tools/native_view_replay/cull_survey.cpp
//     src/Render/capture_file.cpp -o out/cull_survey.exe

#include <cfloat>
#include <cstdio>
#include <map>
#include <vector>

#include "src/Render/capture_file.h"
#include "tools/native_view_replay/free_cam.h"

using namespace band3::render;
using namespace band3::render::freecam;

static void PrintVec(const char* name, const float v[3]) {
    std::printf("\"%s\": [%.3f, %.3f, %.3f]", name, v[0], v[1], v[2]);
}

int main(int argc, char** argv) {
    std::printf("[\n");
    for (int a = 1; a < argc; a++) {
        auto fc = LoadCapture(argv[a]);
        if (!fc) {
            std::fprintf(stderr, "can't load %s\n", argv[a]);
            return 1;
        }
        // the world camera
        std::map<uint32_t, int> counts;
        std::map<uint32_t, Mat4> vps;
        for (size_t i = 0; i < fc->draws.size(); i++) {
            const DrawItem& d = fc->draws[i];
            if (!DrawnToBackBuffer(d)) continue;
            if (fc->post_boundary != FrameCapture::kNoPost && i >= fc->post_boundary) continue;
            counts[d.cam]++;
            vps[d.cam] = d.view_proj;
        }
        uint32_t cam = 0;
        int best = 0;
        for (auto [c, n] : counts)
            if (n > best) cam = c, best = n;
        Pose pose{};
        Mat4 proj{};
        const bool persp = best > 0 && Decompose(vps[cam], pose, proj);
        std::printf("%s{\"file\": \"%s\", \"frame\": %llu, \"world_cam\": \"%08X\", \"perspective\": %s,\n",
                    a > 1 ? ",\n" : "", argv[a], (unsigned long long)fc->game_frame, cam,
                    persp ? "true" : "false");
        std::printf(" \"cams\": {");
        bool first = true;
        for (auto [c, n] : counts) {
            std::printf("%s\"%08X\": %d", first ? "" : ", ", c, n);
            first = false;
        }
        std::printf("},\n \"pose\": {");
        PrintVec("eye", pose.eye), std::printf(", ");
        PrintVec("fwd", pose.fwd), std::printf(", ");
        PrintVec("right", pose.right), std::printf(", ");
        PrintVec("up", pose.up);
        // the axes' orthogonality, a check on the decomposition
        std::printf(", \"ortho\": [%.5f, %.5f, %.5f]},\n", Dot3(pose.right, pose.fwd),
                    Dot3(pose.right, pose.up), Dot3(pose.fwd, pose.up));
        std::printf(" \"proj\": [");
        for (int r = 0; r < 4; r++)
            std::printf("%s[%.5f, %.5f, %.5f, %.5f]", r ? ", " : "", proj.m[r][0], proj.m[r][1],
                        proj.m[r][2], proj.m[r][3]);
        std::printf("],\n \"draws\": [");
        first = true;
        for (size_t i = 0; i < fc->draws.size(); i++) {
            const DrawItem& d = fc->draws[i];
            if (!DrawnToBackBuffer(d) || d.cam != cam || !d.geom) continue;
            if (fc->post_boundary != FrameCapture::kNoPost && i >= fc->post_boundary) continue;
            float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
            unsigned all_out = ~0u;
            size_t inside = 0;
            for (const Vertex& v : d.geom->verts) {
                float w[3], c[4];
                WorldPos(d, v, w);
                for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], w[k]), hi[k] = std::max(hi[k], w[k]);
                Transform(w, d.view_proj, c);
                const unsigned o = Outcode(c);
                all_out &= o;
                if (!o) inside++;
            }
            const size_t nv = d.geom->verts.size();
            std::printf("%s\n  {\"i\": %zu, \"mesh\": \"%08X\", \"skinned\": %s, \"verts\": %zu, "
                        "\"tris\": %zu, \"blend\": %d, \"shader_type\": %d, \"outside\": %u, "
                        "\"inside\": %zu, \"lo\": [%.2f, %.2f, %.2f], \"hi\": [%.2f, %.2f, %.2f]}",
                        first ? "" : ",", i, d.mesh, d.bones.empty() ? "false" : "true", nv,
                        d.geom->indices.size() / 3, d.blend,
                        d.shade >= 0 ? fc->shades[d.shade].shader_type : -1, nv ? all_out : 0,
                        inside, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
            first = false;
        }
        std::printf("]}");
    }
    std::printf("\n]\n");
    return 0;
}
