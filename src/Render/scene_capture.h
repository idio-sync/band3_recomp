#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

// Experimental: records what RB3 draws to the back buffer each frame, read
// straight out of guest memory, so the native view (native_view.cpp) can draw
// it without the emulated GPU.
//
// Offsets are rb3-xenon's (src/system/rndobj, src/system/rnddx9), checked
// against the recompiled DxMesh::DrawShowing, DxMesh::OnSync and
// DxMesh::SetTransforms.

namespace band3::render {

// row vectors, v' = v * M, as Milo and D3D9 use them
struct Mat4 {
    float m[4][4];
};

struct Vertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    uint32_t color;  // RGBA8, R in the low byte
    uint8_t bone[4];
    float weight[4];
};

struct Geometry {
    std::vector<Vertex> verts;
    std::vector<uint16_t> indices;  // triangle list
};

struct Texture {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint32_t> rgba;  // R in the low byte
    uint32_t format = 0;         // Xenos TextureFormat, for the stats
};

// one mesh draw that reached the back buffer (first material pass): a
// DxMesh::DrawShowing, one DxMultiMesh instance, or a particle system's quads
struct DrawItem {
    std::shared_ptr<const Geometry> geom;
    std::shared_ptr<const Texture> tex;
    Mat4 world;
    Mat4 view_proj;
    std::vector<Mat4> bones;  // skinned when not empty; world is unused then
    float color[4];
    int blend;   // RndMat::Blend
    int z_mode;  // RndMat::ZMode
    bool prelit;
    bool alpha_cut;
    int alpha_threshold;
    uint32_t cam;
    uint32_t mesh;
};

struct FrameCapture {
    uint64_t frame = 0;
    std::vector<DrawItem> draws;
    uint32_t cams = 0;             // camera selects that drew to the back buffer
    uint32_t skipped_target = 0;   // mesh draws into render targets
    uint32_t skipped_velocity = 0; // motion blur velocity pass
    uint32_t skipped_no_geom = 0;  // no material, buffers or faces
    uint32_t mutable_meshes = 0;   // drawn from CPU verts
    uint32_t multimesh_instances = 0;
    uint32_t particles = 0;
    uint32_t textured = 0;
    uint32_t untextured_format = 0; // texture present in a format not decoded
    uint32_t geom_cached = 0;
    uint32_t tex_cached = 0;
};

// capture costs a little every frame, so it only runs while something wants
// it: each Acquire is matched by a Release
void AcquireCapture();
void ReleaseCapture();

// For a render check: waits for the next full frame captured (RB3 alternates
// them with overlay-only frames; the request learns the difference over two
// frames), holds the game at the end of it, waits
// `settle` for the emulated GPU to show it, runs `while_held` (a screenshot of
// the same frame) and lets the game go on. The game is never held more than
// three seconds. Null, without running while_held, if no frame came in time.
std::shared_ptr<const FrameCapture> CaptureHeldFrame(
    const std::function<void()>& while_held, std::chrono::milliseconds timeout,
    std::chrono::milliseconds settle = std::chrono::milliseconds(150));

// the latest complete frame, or null before the first
std::shared_ptr<const FrameCapture> LatestCapture();

}  // namespace band3::render
