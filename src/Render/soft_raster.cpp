#include "src/Render/soft_raster.h"

#include "src/Render/shade_model.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_set>

// See soft_raster.h.

namespace band3::render {
namespace {

constexpr float kNearW = 1e-3f;
constexpr uint32_t kClearColor = 0xff202020u;

// a vertex after the view-projection, with what the pixels interpolate
struct ClipVert {
    float p[4];  // clip x, y, z, w
    float uv[2];
    float n[3];
    float c[4];
    float wp[3];  // world position
    // a vertex-lit material's Lighting (shade.hlsli), diffuse and added
    float ld[3];
    float la[3];
};

ClipVert Lerp(const ClipVert& a, const ClipVert& b, float t) {
    ClipVert r;
    for (int i = 0; i < 4; i++) r.p[i] = a.p[i] + (b.p[i] - a.p[i]) * t;
    for (int i = 0; i < 2; i++) r.uv[i] = a.uv[i] + (b.uv[i] - a.uv[i]) * t;
    for (int i = 0; i < 3; i++) r.n[i] = a.n[i] + (b.n[i] - a.n[i]) * t;
    for (int i = 0; i < 4; i++) r.c[i] = a.c[i] + (b.c[i] - a.c[i]) * t;
    for (int i = 0; i < 3; i++) r.wp[i] = a.wp[i] + (b.wp[i] - a.wp[i]) * t;
    for (int i = 0; i < 3; i++) r.ld[i] = a.ld[i] + (b.ld[i] - a.ld[i]) * t;
    for (int i = 0; i < 3; i++) r.la[i] = a.la[i] + (b.la[i] - a.la[i]) * t;
    return r;
}

void Point(const float p[3], const Mat4& m, float out[3]) {
    for (int c = 0; c < 3; c++)
        out[c] = p[0] * m.m[0][c] + p[1] * m.m[1][c] + p[2] * m.m[2][c] + m.m[3][c];
}

void Dir(const float d[3], const Mat4& m, float out[3]) {
    for (int c = 0; c < 3; c++) out[c] = d[0] * m.m[0][c] + d[1] * m.m[1][c] + d[2] * m.m[2][c];
}

struct Target {
    uint32_t w, h;
    std::vector<uint32_t>& color;
    std::vector<float>& depth;  // 1/w, larger is nearer, 0 is cleared
    std::vector<int32_t>* ids;  // the draw that last wrote each pixel, if wanted
};

struct DrawState {
    int32_t index;  // in the frame's draws
    const Texture* tex;
    const Texture* spec_map;  // null unless shade samples it
    const Texture* glow;
    shade::ShadeParams shade;
    bool per_vertex;  // kShadePerVertex: ClipVert's ld and la are set
    int blend;
    bool z_test;
    bool z_equal_passes;
    bool z_write;
};

// nearest texel, wrapping; mesh.hlsl's Texel does the same arithmetic
void Texel(const Texture& t, const float uv[2], float out[4]) {
    const float u = uv[0] - std::floor(uv[0]), v = uv[1] - std::floor(uv[1]);
    const uint32_t x = std::min(uint32_t(u * float(t.width)), t.width - 1);
    const uint32_t y = std::min(uint32_t(v * float(t.height)), t.height - 1);
    const uint32_t c = t.rgba[size_t(y) * t.width + x];
    for (int i = 0; i < 4; i++) out[i] = float((c >> (8 * i)) & 0xff) / 255.0f;
}

void Shade(const DrawState& ds, const float uv[2], const float n[3], const float vc[4],
           const float wp[3], float depth, const float ld[3], const float la[3], float out[4]) {
    float texel[4] = {1, 1, 1, 1}, spec_map[4] = {1, 1, 1, 1}, glow[4] = {0, 0, 0, 0};
    if (ds.tex) Texel(*ds.tex, uv, texel);
    if (ds.spec_map) Texel(*ds.spec_map, uv, spec_map);
    if (ds.glow) Texel(*ds.glow, uv, glow);
    shade::ShadePixelCpu(ds.shade, wp, n, vc, texel, spec_map, glow, depth, ld, la, out);
}

uint32_t Blend(int mode, const float s[4], uint32_t dst) {
    float d[4];
    for (int i = 0; i < 4; i++) d[i] = float((dst >> (8 * i)) & 0xff) / 255.0f;
    float o[3];
    const float a = std::clamp(s[3], 0.0f, 1.0f);
    for (int i = 0; i < 3; i++) {
        switch (mode) {
            case 2: o[i] = d[i] + s[i]; break;                    // Add
            case 3: o[i] = s[i] * a + d[i] * (1.0f - a); break;  // SrcAlpha
            case 4: o[i] = d[i] + s[i] * a; break;               // SrcAlphaAdd
            case 5: o[i] = d[i] - s[i]; break;                   // Subtract
            case 6: o[i] = d[i] * s[i]; break;                   // Multiply
            default: o[i] = s[i]; break;                         // Src
        }
    }
    uint32_t r = 0xff000000u;
    for (int i = 0; i < 3; i++)
        r |= uint32_t(std::clamp(o[i], 0.0f, 1.0f) * 255.0f + 0.5f) << (8 * i);
    return r;
}

void RasterTri(const ClipVert& a, const ClipVert& b, const ClipVert& c, const DrawState& ds,
               Target& t, RasterStats& st) {
    const ClipVert* v[3] = {&a, &b, &c};
    float sx[3], sy[3], iw[3];
    for (int i = 0; i < 3; i++) {
        iw[i] = 1.0f / v[i]->p[3];
        sx[i] = (v[i]->p[0] * iw[i] * 0.5f + 0.5f) * float(t.w);
        sy[i] = (0.5f - v[i]->p[1] * iw[i] * 0.5f) * float(t.h);
    }
    const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
    if (!(std::fabs(area) > 1e-9f)) return;
    const float min_x = std::max(0.0f, std::floor(std::min({sx[0], sx[1], sx[2]})));
    const float max_x = std::min(float(t.w - 1), std::ceil(std::max({sx[0], sx[1], sx[2]})));
    const float min_y = std::max(0.0f, std::floor(std::min({sy[0], sy[1], sy[2]})));
    const float max_y = std::min(float(t.h - 1), std::ceil(std::max({sy[0], sy[1], sy[2]})));
    if (min_x > max_x || min_y > max_y) return;
    st.triangles++;
    const float inv_area = 1.0f / area;

    for (int y = int(min_y); y <= int(max_y); y++) {
        const float py = float(y) + 0.5f;
        for (int x = int(min_x); x <= int(max_x); x++) {
            const float px = float(x) + 0.5f;
            const float l0 = ((sx[2] - sx[1]) * (py - sy[1]) - (sy[2] - sy[1]) * (px - sx[1])) * inv_area;
            const float l1 = ((sx[0] - sx[2]) * (py - sy[2]) - (sy[0] - sy[2]) * (px - sx[2])) * inv_area;
            const float l2 = 1.0f - l0 - l1;
            if (l0 < 0 || l1 < 0 || l2 < 0) continue;
            const float z = l0 * iw[0] + l1 * iw[1] + l2 * iw[2];
            const size_t idx = size_t(y) * t.w + x;
            if (ds.z_test) {
                const float d = t.depth[idx];
                if (ds.z_equal_passes ? z < d * 0.9999f : z <= d) continue;
            }
            const float q0 = l0 * iw[0] / z, q1 = l1 * iw[1] / z, q2 = l2 * iw[2] / z;
            float uv[2], n[3], vc[4], wp[3];
            for (int i = 0; i < 2; i++) uv[i] = q0 * a.uv[i] + q1 * b.uv[i] + q2 * c.uv[i];
            for (int i = 0; i < 3; i++) n[i] = q0 * a.n[i] + q1 * b.n[i] + q2 * c.n[i];
            for (int i = 0; i < 4; i++) vc[i] = q0 * a.c[i] + q1 * b.c[i] + q2 * c.c[i];
            for (int i = 0; i < 3; i++) wp[i] = q0 * a.wp[i] + q1 * b.wp[i] + q2 * c.wp[i];
            float ld[3] = {0, 0, 0}, la[3] = {0, 0, 0};
            if (ds.per_vertex) {
                for (int i = 0; i < 3; i++) ld[i] = q0 * a.ld[i] + q1 * b.ld[i] + q2 * c.ld[i];
                for (int i = 0; i < 3; i++) la[i] = q0 * a.la[i] + q1 * b.la[i] + q2 * c.la[i];
            }
            float col[4];
            Shade(ds, uv, n, vc, wp, 1.0f / z, ld, la, col);
            if (shade::AlphaCutCpu(ds.shade, col[3])) continue;
            if (ds.blend != 0) {
                t.color[idx] = Blend(ds.blend, col, t.color[idx]);
                if (t.ids) (*t.ids)[idx] = ds.index;
            }
            if (ds.z_write) t.depth[idx] = z;
            st.pixels++;
        }
    }
}

// The clip planes, as distances that are >= 0 inside: the near plane, then a
// guard band kGuard times the screen's half-size each side. A triangle that
// crosses the near plane projects up to ~1e9 pixels away, where RasterTri's
// float edge functions lose the pixel position altogether (stripes and blocks
// across the screen); clipped to the band, no corner is more than a few
// thousand pixels out. The band's planes pass through the eye, so clipping to
// them moves no pixel: they only cut away what's off screen anyway.
constexpr float kGuard = 8.0f;
constexpr int kClipPlanes = 5;

float PlaneDist(const ClipVert& v, int plane) {
    switch (plane) {
        case 0: return v.p[3] - kNearW;
        case 1: return kGuard * v.p[3] - v.p[0];
        case 2: return kGuard * v.p[3] + v.p[0];
        case 3: return kGuard * v.p[3] - v.p[1];
        default: return kGuard * v.p[3] + v.p[1];
    }
}

// clips against the near plane and the guard band, then draws the fan
void ClipAndRaster(const ClipVert& a, const ClipVert& b, const ClipVert& c,
                   const DrawState& ds, Target& t, RasterStats& st) {
    uint32_t outside = 0;
    for (int p = 0; p < kClipPlanes; p++)
        if (PlaneDist(a, p) < 0 || PlaneDist(b, p) < 0 || PlaneDist(c, p) < 0) outside |= 1u << p;
    if (!outside) {
        RasterTri(a, b, c, ds, t, st);
        return;
    }
    // each plane adds at most one corner
    ClipVert buf[2][3 + kClipPlanes];
    buf[0][0] = a;
    buf[0][1] = b;
    buf[0][2] = c;
    int n = 3, cur_buf = 0;
    for (int p = 0; p < kClipPlanes && n >= 3; p++) {
        if (!(outside & (1u << p))) continue;
        const ClipVert* in = buf[cur_buf];
        ClipVert* out = buf[cur_buf ^ 1];
        int m = 0;
        for (int i = 0; i < n; i++) {
            const ClipVert& cur = in[i];
            const ClipVert& nxt = in[(i + 1) % n];
            const float dc = PlaneDist(cur, p), dn = PlaneDist(nxt, p);
            if (dc >= 0) out[m++] = cur;
            if ((dc >= 0) != (dn >= 0)) out[m++] = Lerp(cur, nxt, dc / (dc - dn));
        }
        n = m;
        cur_buf ^= 1;
    }
    const ClipVert* poly = buf[cur_buf];
    for (int i = 1; i + 1 < n; i++) RasterTri(poly[0], poly[i], poly[i + 1], ds, t, st);
}

void DrawOne(const DrawItem& it, int32_t index, const ShadeState* state, const RasterOptions& o,
             Target& t, RasterStats& st, std::vector<ClipVert>& cv) {
    const Geometry& g = *it.geom;
    const bool skinned = o.skinning && !it.bones.empty();
    DrawState ds;
    ds.index = index;
    ds.tex = o.textures && it.tex ? it.tex.get() : nullptr;
    shade::PackShade(it, state, o, ds.tex != nullptr, ds.shade);
    ds.spec_map = ds.shade.flags.x & shade::kShadeSpecMap ? state->maps[kMapSpecular].get() : nullptr;
    ds.glow = ds.shade.flags.x & shade::kShadeGlow ? state->maps[kMapGlow].get() : nullptr;
    ds.per_vertex = (ds.shade.flags.x & shade::kShadePerVertex) != 0;
    cv.resize(g.verts.size());
    for (size_t i = 0; i < g.verts.size(); i++) {
        const Vertex& v = g.verts[i];
        float wp[3] = {0, 0, 0}, wn[3] = {0, 0, 0};
        if (skinned) {
            float total = 0;
            for (int k = 0; k < 4; k++) {
                const float w = v.weight[k];
                if (w <= 0) continue;
                const Mat4& b = it.bones[v.bone[k] < it.bones.size() ? v.bone[k] : 0];
                float p[3], n[3];
                Point(v.pos, b, p);
                Dir(v.nrm, b, n);
                for (int c = 0; c < 3; c++) {
                    wp[c] += p[c] * w;
                    wn[c] += n[c] * w;
                }
                total += w;
            }
            if (total <= 0) {
                Point(v.pos, it.bones[0], wp);
                Dir(v.nrm, it.bones[0], wn);
            }
        } else {
            Point(v.pos, it.world, wp);
            Dir(v.nrm, it.world, wn);
        }
        ClipVert& c = cv[i];
        for (int col = 0; col < 4; col++)
            c.p[col] = wp[0] * it.view_proj.m[0][col] + wp[1] * it.view_proj.m[1][col] +
                       wp[2] * it.view_proj.m[2][col] + it.view_proj.m[3][col];
        shade::TexGenUv(ds.shade, v.uv, c.uv);
        for (int k = 0; k < 3; k++) c.n[k] = wn[k];
        for (int k = 0; k < 3; k++) c.wp[k] = wp[k];
        for (int k = 0; k < 4; k++) c.c[k] = float((v.color >> (8 * k)) & 0xff) / 255.0f;
        if (ds.per_vertex) shade::LightVertexCpu(ds.shade, wp, wn, c.c, c.ld, c.la);
    }

    ds.blend = o.blending ? it.blend : 1;
    switch (it.z_mode) {
        case 0: ds.z_test = false; ds.z_equal_passes = false; ds.z_write = false; break;
        case 2: ds.z_test = true; ds.z_equal_passes = true; ds.z_write = false; break;
        case 3: ds.z_test = false; ds.z_equal_passes = false; ds.z_write = true; break;
        case 4: ds.z_test = true; ds.z_equal_passes = true; ds.z_write = true; break;
        default: ds.z_test = true; ds.z_equal_passes = false; ds.z_write = true; break;
    }
    if (!o.blending) {
        ds.z_test = true;
        ds.z_write = true;
    }
    for (size_t i = 0; i + 2 < g.indices.size(); i += 3)
        ClipAndRaster(cv[g.indices[i]], cv[g.indices[i + 1]], cv[g.indices[i + 2]], ds, t, st);
    st.draws++;
}

}  // namespace

RasterStats Rasterize(const FrameCapture& frame, const RasterOptions& o,
                      std::vector<uint32_t>& rgba, std::vector<int32_t>* ids) {
    const auto start = std::chrono::steady_clock::now();
    RasterStats st;
    rgba.assign(size_t(o.width) * o.height, kClearColor);
    if (ids) ids->assign(size_t(o.width) * o.height, -1);
    std::vector<float> depth(size_t(o.width) * o.height, 0.0f);
    Target t{o.width, o.height, rgba, depth, ids};
    std::vector<ClipVert> cv;
    std::unordered_set<uint32_t> cams_seen;
    uint32_t last_cam = 0;
    for (size_t i = 0; i < frame.draws.size(); i++) {
        const DrawItem& it = frame.draws[i];
        if (o.clear_depth_per_camera && it.cam != last_cam && cams_seen.insert(it.cam).second)
            std::fill(depth.begin(), depth.end(), 0.0f);
        last_cam = it.cam;
        DrawOne(it, int32_t(i), shade::ShadeOf(frame, it), o, t, st, cv);
    }
    st.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
    return st;
}

}  // namespace band3::render
