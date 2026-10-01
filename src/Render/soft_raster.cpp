#include "src/Render/soft_raster.h"

#include "src/Render/post_model.h"
#include "src/Render/shade_model.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

// See soft_raster.h.

namespace band3::render {
namespace {

// the picture's clear; the scene target's is the same with alpha 0
constexpr uint32_t kClearColor = 0xff202020u;
// what a render target nothing has drawn samples as
constexpr uint32_t kTransparentBlack = 0;

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

// what a target does with alpha: the picture keeps it 1, a texture blends
// it by the colour's factors, the scene target as RB3's back buffer does
// (WritesSceneAlpha)
enum class TargetAlpha { kOpaque, kTexture, kScene };

// what a draw does with its target's alpha: Blend()
enum class AlphaRule { kOpaque, kColorFactors, kKeep, kMax };

struct Target {
    uint32_t w, h;
    std::vector<uint32_t>& color;
    std::vector<float>& depth;  // 1/w, larger is nearer, 0 is cleared
    std::vector<int32_t>* ids;  // the draw that last wrote each pixel, if wanted
    TargetAlpha alpha = TargetAlpha::kOpaque;
    // a texture pass's: the texture it draws into, which its draws can't sample
    uint32_t tex_obj = 0;
    bool no_z = false;  // no depth buffer: nothing tests or writes depth
    // the viewport (clip space -1..1 maps to x..x+w, y..y+h), and the pixels
    // it covers, which is all a triangle can reach
    float vx = 0, vy = 0, vw = 0, vh = 0;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    void SetViewport(float x, float y, float vw_, float vh_) {
        vx = x;
        vy = y;
        vw = vw_;
        vh = vh_;
        x0 = std::clamp(int(std::floor(x)), 0, int(w));
        y0 = std::clamp(int(std::floor(y)), 0, int(h));
        x1 = std::clamp(int(std::ceil(x + vw_)), x0, int(w));
        y1 = std::clamp(int(std::ceil(y + vh_)), y0, int(h));
    }
};

// a texture to sample: its size and pixels (RGBA8, R low), null for none
struct TexView {
    uint32_t w = 0, h = 0;
    const uint32_t* px = nullptr;
};

TexView View(const Texture* t) {
    if (!t || t->rgba.empty()) return {};
    return {t->width, t->height, t->rgba.data()};
}

struct DrawState {
    int32_t index;  // in the frame's draws
    TexView tex;
    TexView spec_map;  // none unless shade samples it
    TexView glow;
    shade::ShadeParams shade;
    bool per_vertex;  // kShadePerVertex: ClipVert's ld and la are set
    int blend;
    AlphaRule alpha;
    bool z_test;
    bool z_equal_passes;
    bool z_write;
};

// nearest texel, wrapping; mesh.hlsl's Texel does the same arithmetic
void Texel(const TexView& t, const float uv[2], float out[4]) {
    const float u = uv[0] - std::floor(uv[0]), v = uv[1] - std::floor(uv[1]);
    const uint32_t x = std::min(uint32_t(u * float(t.w)), t.w - 1);
    const uint32_t y = std::min(uint32_t(v * float(t.h)), t.h - 1);
    const uint32_t c = t.px[size_t(y) * t.w + x];
    for (int i = 0; i < 4; i++) out[i] = float((c >> (8 * i)) & 0xff) / 255.0f;
}

void Shade(const DrawState& ds, const float uv[2], const float n[3], const float vc[4],
           const float wp[3], float depth, const float ld[3], const float la[3], float out[4]) {
    float texel[4] = {1, 1, 1, 1}, spec_map[4] = {1, 1, 1, 1}, glow[4] = {0, 0, 0, 0};
    if (ds.tex.px) Texel(ds.tex, uv, texel);
    if (ds.spec_map.px) Texel(ds.spec_map, uv, spec_map);
    if (ds.glow.px) Texel(ds.glow, uv, glow);
    shade::ShadePixelCpu(ds.shade, wp, n, vc, texel, spec_map, glow, depth, ld, la, out);
}

// The colour by the material's blend mode (Dest keeps it), and alpha by
// `alpha`: 1, blended by the colour's factors (a texture's, as gpu_view.cpp's
// pipelines do), kept, or RB3's back buffer's ONE ONE MAX, the larger of the
// two where the mode blends (any but Src, which Blend() draws other modes as)
uint32_t Blend(int mode, const float s[4], uint32_t dst, AlphaRule alpha) {
    float d[4];
    for (int i = 0; i < 4; i++) d[i] = float((dst >> (8 * i)) & 0xff) / 255.0f;
    float o[4];
    const float a = std::clamp(s[3], 0.0f, 1.0f);
    for (int i = 0; i < 3; i++) {
        switch (mode) {
            case 2: o[i] = d[i] + s[i]; break;                    // Add
            case 3: o[i] = s[i] * a + d[i] * (1.0f - a); break;  // SrcAlpha
            case 4: o[i] = d[i] + s[i] * a; break;               // SrcAlphaAdd
            case 5: o[i] = d[i] - s[i]; break;                   // Subtract
            case 6: o[i] = d[i] * s[i]; break;                   // Multiply
            case 0: o[i] = d[i]; break;                          // Dest
            default: o[i] = s[i]; break;                         // Src
        }
    }
    switch (alpha) {
        case AlphaRule::kOpaque: o[3] = 1.0f; break;
        case AlphaRule::kKeep: o[3] = d[3]; break;
        case AlphaRule::kMax:
            o[3] = mode < 0 || mode == 1 || mode > 6 ? a : std::max(a, d[3]);
            break;
        case AlphaRule::kColorFactors:
            switch (mode) {
                case 2: o[3] = d[3] + a; break;
                case 3: o[3] = a * a + d[3] * (1.0f - a); break;
                case 4: o[3] = d[3] + a * a; break;
                case 5: o[3] = d[3] - a; break;
                case 6: o[3] = d[3] * a; break;
                default: o[3] = a; break;
            }
            break;
    }
    uint32_t r = 0;
    for (int i = 0; i < 4; i++)
        r |= uint32_t(std::clamp(o[i], 0.0f, 1.0f) * 255.0f + 0.5f) << (8 * i);
    return r;
}

void RasterTri(const ClipVert& a, const ClipVert& b, const ClipVert& c, const DrawState& ds,
               Target& t, RasterStats& st) {
    const ClipVert* v[3] = {&a, &b, &c};
    float sx[3], sy[3], iw[3];
    for (int i = 0; i < 3; i++) {
        iw[i] = 1.0f / v[i]->p[3];
        sx[i] = t.vx + (v[i]->p[0] * iw[i] * 0.5f + 0.5f) * t.vw;
        sy[i] = t.vy + (0.5f - v[i]->p[1] * iw[i] * 0.5f) * t.vh;
    }
    const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
    if (!(std::fabs(area) > 1e-9f)) return;
    const float min_x = std::max(float(t.x0), std::floor(std::min({sx[0], sx[1], sx[2]})));
    const float max_x = std::min(float(t.x1 - 1), std::ceil(std::max({sx[0], sx[1], sx[2]})));
    const float min_y = std::max(float(t.y0), std::floor(std::min({sy[0], sy[1], sy[2]})));
    const float max_y = std::min(float(t.y1 - 1), std::ceil(std::max({sy[0], sy[1], sy[2]})));
    if (min_x > max_x || min_y > max_y) return;
    st.triangles++;
    const float inv_area = 1.0f / area;
    // D3D's top-left rule, as the GPU fills: a pixel centre on an edge is
    // the triangle's only if that's a left edge (the inside to its right) or
    // a top one (level, the inside below), so of two triangles sharing an
    // edge (a quad's diagonal) only one draws it, and a blended quad blends
    // once. Edge k is opposite vertex k; (gx, gy) is where its l grows.
    auto owns = [&](int a, int b) {
        const float gx = -(sy[b] - sy[a]) * inv_area, gy = (sx[b] - sx[a]) * inv_area;
        return gx > 0 || (gx == 0 && gy > 0);
    };
    const bool own0 = owns(1, 2), own1 = owns(2, 0), own2 = owns(0, 1);

    for (int y = int(min_y); y <= int(max_y); y++) {
        const float py = float(y) + 0.5f;
        for (int x = int(min_x); x <= int(max_x); x++) {
            const float px = float(x) + 0.5f;
            const float l0 = ((sx[2] - sx[1]) * (py - sy[1]) - (sy[2] - sy[1]) * (px - sx[1])) * inv_area;
            const float l1 = ((sx[0] - sx[2]) * (py - sy[2]) - (sy[0] - sy[2]) * (px - sx[2])) * inv_area;
            const float l2 = ((sx[1] - sx[0]) * (py - sy[0]) - (sy[1] - sy[0]) * (px - sx[0])) * inv_area;
            if (l0 < 0 || l1 < 0 || l2 < 0) continue;
            if ((l0 == 0 && !own0) || (l1 == 0 && !own1) || (l2 == 0 && !own2)) continue;
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
            // Dest draws no colour, but may still write the scene's alpha
            if (ds.blend != 0 || ds.alpha == AlphaRule::kMax) {
                t.color[idx] = Blend(ds.blend, col, t.color[idx], ds.alpha);
                if (t.ids && ds.blend != 0) (*t.ids)[idx] = ds.index;
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

// a texture pass's target, kept for the frame
struct RtTarget {
    uint32_t w = 0, h = 0;
    std::vector<uint32_t> color;
    std::vector<float> depth;
};
using RtTargets = std::unordered_map<uint32_t, RtTarget>;

// What a draw's diffuse texture samples. A render target is what its passes
// have drawn so far this frame, else guest memory's pixels if they're kept
// and wanted, else transparent black (counted); the target being drawn can't
// sample itself, so it's black too. Without texture passes, a render target
// is its guest pixels if wanted. A texture without pixels (a render target
// kept without them, or a format that isn't decoded) draws untextured.
TexView Diffuse(const DrawItem& it, const RasterOptions& o, const RtTargets& rts,
                const Target& t, RasterStats& st) {
    static constexpr uint32_t kBlack = kTransparentBlack;
    if (!o.textures || !it.tex) return {};
    const Texture& tex = *it.tex;
    if (o.texture_passes && IsPassTarget(&tex)) {
        if (tex.tex_obj != t.tex_obj) {
            if (auto f = rts.find(tex.tex_obj); f != rts.end())
                return {f->second.w, f->second.h, f->second.color.data()};
            if (o.rt_guest_pixels && !tex.rgba.empty()) return View(&tex);
        }
        st.rt_missing++;
        return {1, 1, &kBlack};
    }
    if (tex.tex_obj && !o.rt_guest_pixels) return {};
    return View(&tex);
}

void DrawOne(const DrawItem& it, int32_t index, const ShadeState* state, const RasterOptions& o,
             const RtTargets& rts, Target& t, RasterStats& st, std::vector<ClipVert>& cv) {
    const Geometry& g = *it.geom;
    const bool skinned = o.skinning && !it.bones.empty();
    DrawState ds;
    ds.index = index;
    ds.tex = Diffuse(it, o, rts, t, st);
    shade::PackShade(it, state, o, ds.tex.px != nullptr, ds.shade);
    if (ds.shade.flags.x & shade::kShadeSpecMap)
        ds.spec_map = View(state->maps[kMapSpecular].get());
    if (ds.shade.flags.x & shade::kShadeGlow) ds.glow = View(state->maps[kMapGlow].get());
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
    switch (t.alpha) {
        case TargetAlpha::kOpaque: ds.alpha = AlphaRule::kOpaque; break;
        case TargetAlpha::kTexture: ds.alpha = AlphaRule::kColorFactors; break;
        case TargetAlpha::kScene:
            ds.alpha = WritesSceneAlpha(state) ? AlphaRule::kMax : AlphaRule::kKeep;
            break;
    }
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
    if (t.no_z) ds.z_test = ds.z_write = false;
    for (size_t i = 0; i + 2 < g.indices.size(); i += 3)
        ClipAndRaster(cv[g.indices[i]], cv[g.indices[i + 1]], cv[g.indices[i + 2]], ds, t, st);
    st.draws++;
}

bool Drawable(const DrawItem& it) {
    return it.geom && !it.geom->verts.empty() && it.geom->indices.size() >= 3;
}

// PlanPasses, with `also` (a DxTex, 0 none) wanted whatever samples it,
// even after post-processing starts: at the frame's end, and its pass of
// `also_version` (0 none) too
std::vector<PassRun> Plan(const FrameCapture& f, const RasterOptions& o, uint32_t also,
                          uint32_t also_version) {
    std::vector<PassRun> runs;
    const uint32_t n = uint32_t(f.draws.size());
    if (f.passes.empty()) {
        runs.push_back({nullptr, 0, n});
        return runs;
    }
    // backwards, so a pass is drawn only if something after it samples its
    // texture
    std::unordered_set<uint32_t> needed;
    if (also) needed.insert(also);
    auto samples = [&](uint32_t first, uint32_t end, bool texture) {
        if (!o.textures) return;
        for (uint32_t d = first; d < end; d++) {
            const DrawItem& it = f.draws[d];
            if (texture ? !DrawnInTexturePass(it) : !DrawnToBackBuffer(it)) continue;
            if (IsPassTarget(it.tex.get())) needed.insert(it.tex->tex_obj);
        }
    };
    for (size_t i = f.passes.size(); i-- > 0;) {
        const Pass& p = f.passes[i];
        const uint32_t first = std::min(p.first_draw, n);
        const uint32_t end = std::min(first + p.draw_count, n);
        if (!p.tex_obj) {
            runs.push_back({nullptr, first, end});
            samples(first, end, false);
            continue;
        }
        const bool wanted = also && p.tex_obj == also;
        if (!o.texture_passes || !p.width || !p.height) continue;
        if (!needed.count(p.tex_obj) && !(wanted && p.version == also_version)) continue;
        if (first >= f.post_boundary && !wanted) continue;
        // what it drew over isn't seen
        if (p.clear_flags & 0x0f) needed.erase(p.tex_obj);
        runs.push_back({&p, first, end});
        samples(first, end, true);
    }
    std::reverse(runs.begin(), runs.end());
    return runs;
}

// Rasterize(), and the texture targets it drew: `stop` (a DxTex, 0 none)
// ends the frame after `stop_version` of it (0 its last) is drawn
RasterStats Run(const FrameCapture& frame, const RasterOptions& o, std::vector<uint32_t>& rgba,
                std::vector<int32_t>* ids, RtTargets& rts, uint32_t stop,
                uint32_t stop_version) {
    const auto start = std::chrono::steady_clock::now();
    RasterStats st;
    const size_t pixels = size_t(o.width) * o.height;
    rgba.assign(pixels, kClearColor);
    if (ids) ids->assign(pixels, -1);
    std::vector<float> depth(pixels, 0.0f);
    // the world's draws into the scene target, cleared with alpha 0; the
    // overlay's into the picture, over the depth the world left
    std::vector<uint32_t> scene(pixels, kClearColor & 0x00ffffffu);
    Target world{o.width, o.height, scene, depth, ids};
    world.alpha = TargetAlpha::kScene;
    world.SetViewport(0, 0, float(o.width), float(o.height));
    Target overlay{o.width, o.height, rgba, depth, ids};
    overlay.SetViewport(0, 0, float(o.width), float(o.height));
    Target* back = &world;
    post::PostPlan post_plan;
    const bool post_on =
        o.post && o.view == RasterView::kFinal && post::PlanPost(frame, o.post_only, post_plan);
    // the scene into the picture, at post_boundary (or the frame's end):
    // post-processed, or as it is. A view of the scene target ends the frame
    // there.
    auto resolve = [&] {
        back = &overlay;
        if (post_on) {
            // the depth buffer has 1/w, as RunPost wants it
            post::RunPost(post_plan, scene, depth, o.width, o.height, rgba);
            return;
        }
        for (size_t i = 0; i < pixels; i++) {
            const uint32_t c = scene[i];
            uint32_t g;
            switch (o.view) {
                case RasterView::kSceneAlpha: g = c >> 24; break;
                case RasterView::kSceneDepth:
                    // the depth buffer has 1/w
                    g = uint32_t(DepthViewGrey(depth[i] * kNearW) * 255.0f + 0.5f);
                    break;
                default: rgba[i] = c | 0xff000000u; continue;
            }
            rgba[i] = g | g << 8 | g << 16 | 0xff000000u;
        }
    };
    std::vector<ClipVert> cv;
    std::unordered_set<uint32_t> cams_seen;
    uint32_t last_cam = 0;
    for (const PassRun& run : Plan(frame, o, stop, stop_version)) {
        if (!run.pass) {
            for (uint32_t i = run.first; i < run.end; i++) {
                const DrawItem& it = frame.draws[i];
                if (!DrawnToBackBuffer(it)) continue;
                if (back == &world && i >= frame.post_boundary) resolve();
                if (back == &overlay && o.view != RasterView::kFinal) break;
                if (o.clear_depth_per_camera && it.cam != last_cam &&
                    cams_seen.insert(it.cam).second)
                    std::fill(depth.begin(), depth.end(), 0.0f);
                last_cam = it.cam;
                if (!Drawable(it)) continue;
                DrawOne(it, int32_t(i), shade::ShadeOf(frame, it), o, rts, *back, st, cv);
            }
            continue;
        }
        // into the texture's own target, made at its size (again if that
        // changed) and cleared as DxCam::Select cleared it; one no camera
        // cleared starts transparent black
        const Pass& p = *run.pass;
        RtTarget& rt = rts[p.tex_obj];
        if (rt.w != p.width || rt.h != p.height) {
            rt.w = p.width;
            rt.h = p.height;
            rt.color.assign(size_t(rt.w) * rt.h, kTransparentBlack);
            rt.depth.assign(size_t(rt.w) * rt.h, 0.0f);
        }
        if (p.clear_flags & 0x0f)
            std::fill(rt.color.begin(), rt.color.end(), ArgbToRgba(p.clear_color));
        if (p.clear_flags & 0x30) std::fill(rt.depth.begin(), rt.depth.end(), 0.0f);
        Target rtt{rt.w, rt.h, rt.color, rt.depth, nullptr};
        rtt.alpha = TargetAlpha::kTexture;
        rtt.tex_obj = p.tex_obj;
        rtt.no_z = (p.tex_type & kTexTypeNoZ) != 0;
        for (uint32_t i = run.first; i < run.end; i++) {
            const DrawItem& it = frame.draws[i];
            if (!DrawnInTexturePass(it) || !Drawable(it)) continue;
            // the camera's viewport; DrawRect's quads are in the target's
            // pixels, over all of it
            if (it.rect_shader < 0 && p.viewport[2] > 0 && p.viewport[3] > 0)
                rtt.SetViewport(p.viewport[0], p.viewport[1], p.viewport[2], p.viewport[3]);
            else
                rtt.SetViewport(0, 0, float(rt.w), float(rt.h));
            DrawOne(it, int32_t(i), shade::ShadeOf(frame, it), o, rts, rtt, st, cv);
        }
        st.passes++;
        if (stop && p.tex_obj == stop && p.version == stop_version) break;
    }
    if (back == &world) resolve();
    st.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
    return st;
}

}  // namespace

std::vector<PassRun> PlanPasses(const FrameCapture& frame, const RasterOptions& options) {
    return Plan(frame, options, 0, 0);
}

RasterStats Rasterize(const FrameCapture& frame, const RasterOptions& o,
                      std::vector<uint32_t>& rgba, std::vector<int32_t>* ids) {
    RtTargets rts;
    return Run(frame, o, rgba, ids, rts, 0, 0);
}

bool RasterizeTarget(const FrameCapture& frame, const RasterOptions& options, uint32_t tex_obj,
                     uint32_t version, std::vector<uint32_t>& rgba, uint32_t& width,
                     uint32_t& height, RasterStats* stats) {
    RtTargets rts;
    std::vector<uint32_t> screen;
    const RasterStats st = Run(frame, options, screen, nullptr, rts, tex_obj, version);
    if (stats) *stats = st;
    auto it = rts.find(tex_obj);
    if (it == rts.end()) return false;
    rgba = it->second.color;
    width = it->second.w;
    height = it->second.h;
    return true;
}

}  // namespace band3::render
