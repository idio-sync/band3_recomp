#include "src/Render/vr_pose.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "src/Render/camera_cut.h"

namespace band3::render::vr {

namespace {

// draw `d`'s shade as seen from `pose`, cloned once per source shade
// (`clone`, by the source's index): its eye position and, for billboards,
// its axes. Returns whether a clone was made.
bool CloneShade(const FrameCapture& in, FrameCapture& out, DrawItem& d, const freecam::Pose& pose,
                std::vector<int32_t>& clone) {
    if (d.shade < 0 || size_t(d.shade) >= in.shades.size()) return false;
    int32_t& c = clone[size_t(d.shade)];
    const bool made = c < 0;
    if (made) {
        ShadeState s = in.shades[size_t(d.shade)];
        for (int i = 0; i < 3; i++) s.eye[i] = pose.eye[i];
        // a billboard faces the eye: c16..c18's columns are its right, up
        // and forward (shade.hlsli's Billboard)
        if (s.Option(shader_opt::kBillboard)) {
            const int i16 = ShadeRegIndex(16), i17 = ShadeRegIndex(17), i18 = ShadeRegIndex(18);
            if (i16 >= 0 && i17 >= 0 && i18 >= 0) {
                const int rows[3] = {i16, i17, i18};
                for (int k = 0; k < 3; k++) {
                    s.vs[rows[k]][0] = pose.right[k];
                    s.vs[rows[k]][1] = pose.up[k];
                    s.vs[rows[k]][2] = pose.fwd[k];
                }
            }
        }
        c = int32_t(out.shades.size());
        out.shades.push_back(std::make_shared<ShadeState>(std::move(s)));
    }
    d.shade = c;
    return made;
}

// m's inverse in doubles (Gauss-Jordan with partial pivoting); false if
// it's singular
bool Inverse(const Mat4& m, double out[4][4]) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m.m[r][c] : (c - 4 == r ? 1 : 0);
    for (int c = 0; c < 4; c++) {
        int pivot = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
        if (!(std::fabs(a[pivot][c]) > 1e-12)) return false;
        if (pivot != c)
            for (int k = 0; k < 8; k++) std::swap(a[c][k], a[pivot][k]);
        const double inv = 1 / a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] *= inv;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            const double f = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r][c] = a[r][c + 4];
    return true;
}

// A track camera's draw drawn with its own transform folded into its
// view_proj (`vp` = M old: the beat lines, gems and sustains, whose
// matrices are each a translation or so of the camera's): `vp` re-posed
// as M new, where M = vp old^-1 is affine (row vectors: its last column 0 0
// 0 1). False, leaving `vp`, if it isn't.
bool FoldedInto(Mat4& vp, const double old_inv[4][4], const Mat4& now) {
    double m[4][4];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += double(vp.m[r][k]) * old_inv[k][c];
            m[r][c] = s;
        }
    for (int r = 0; r < 4; r++)
        if (std::fabs(m[r][3] - (r == 3 ? 1 : 0)) > 1e-3) return false;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += m[r][k] * double(now.m[k][c]);
            vp.m[r][c] = float(s);
        }
    return true;
}

// a vertex between a and b, t of the way from a
Vertex Between(const Vertex& a, const Vertex& b, float t) {
    Vertex v = a;
    auto lerp = [&](const float* x, const float* y, float* out, int n) {
        for (int i = 0; i < n; i++) out[i] = x[i] + (y[i] - x[i]) * t;
    };
    lerp(a.pos, b.pos, v.pos, 3);
    lerp(a.nrm, b.nrm, v.nrm, 3);
    lerp(a.uv, b.uv, v.uv, 2);
    lerp(a.weight, b.weight, v.weight, 4);
    lerp(a.tan, b.tan, v.tan, 3);
    v.color = 0;
    for (int k = 0; k < 4; k++) {
        const float x = float(a.color >> (8 * k) & 0xff), y = float(b.color >> (8 * k) & 0xff);
        v.color |= uint32_t(std::lround(x + (y - x) * t)) << (8 * k);
    }
    if (t > 0.5f) std::memcpy(v.bone, b.bone, sizeof(v.bone));
    return v;
}

// the cut geometries last made on this thread (both eyes cut the same
// draws, and the track's own meshes are the same each frame)
struct CutEntry {
    std::weak_ptr<const Geometry> from;
    float plane[4];
    std::shared_ptr<const Geometry> cut;
};
constexpr size_t kCutCache = 64;

}  // namespace

std::shared_ptr<const Geometry> CutAtPlane(const std::shared_ptr<const Geometry>& g,
                                           const float plane[4]) {
    if (!g || g->verts.empty() || g->indices.size() < 3) return g;
    std::vector<float> dist(g->verts.size());
    bool below = false;
    for (size_t i = 0; i < g->verts.size(); i++) {
        const float* p = g->verts[i].pos;
        dist[i] = p[0] * plane[0] + p[1] * plane[1] + p[2] * plane[2] + plane[3];
        below |= dist[i] < 0;
    }
    if (!below) return g;
    thread_local std::vector<CutEntry> cache;
    thread_local size_t next = 0;
    for (const CutEntry& e : cache)
        if (e.from.lock() == g && std::memcmp(e.plane, plane, sizeof(e.plane)) == 0) return e.cut;
    auto out = std::make_shared<Geometry>();
    out->verts = g->verts;
    out->tangents = g->tangents;
    auto add = [&](const Vertex& v) {
        out->verts.push_back(v);
        return uint16_t(out->verts.size() - 1);
    };
    for (size_t i = 0; i + 2 < g->indices.size(); i += 3) {
        const uint16_t tri[3] = {g->indices[i], g->indices[i + 1], g->indices[i + 2]};
        if (tri[0] >= dist.size() || tri[1] >= dist.size() || tri[2] >= dist.size()) continue;
        const float d[3] = {dist[tri[0]], dist[tri[1]], dist[tri[2]]};
        const int kept = (d[0] >= 0) + (d[1] >= 0) + (d[2] >= 0);
        if (kept == 0) continue;
        if (kept == 3) {
            out->indices.insert(out->indices.end(), tri, tri + 3);
            continue;
        }
        // the triangle as a polygon cut at the plane (Sutherland-Hodgman),
        // in its own winding: 3 or 4 corners, fanned
        uint16_t poly[4];
        int n = 0;
        for (int k = 0; k < 3; k++) {
            const int a = k, b = (k + 1) % 3;
            if (d[a] >= 0) poly[n++] = tri[a];
            if ((d[a] >= 0) != (d[b] >= 0)) {
                const float t = d[a] / (d[a] - d[b]);
                poly[n++] = add(Between(g->verts[tri[a]], g->verts[tri[b]], t));
            }
        }
        for (int k = 1; k + 1 < n; k++) out->indices.insert(out->indices.end(), {poly[0], poly[k], poly[k + 1]});
        if (out->verts.size() > 0xffff) return g;  // past uint16 indices: uncut
    }
    std::shared_ptr<const Geometry> cut = out;
    CutEntry e{g, {plane[0], plane[1], plane[2], plane[3]}, cut};
    if (cache.size() < kCutCache) cache.push_back(e);
    else cache[next++ % kCutCache] = e;
    return cut;
}

namespace {

// Draw `d` of a track camera cut where the game's picture ends below (clip
// y = -w, the bottom of the camera's viewport, through its captured
// matrix), so the track before the strikeline ends where the game shows it
// end rather than reaching under the head. In the mesh's own space: the
// plane through its world. Skinned and billboard draws are left whole.
void CutBelowPicture(DrawItem& d, const ShadeState* s) {
    if (!d.bones.empty() || (s && s->Option(shader_opt::kBillboard))) return;
    float world_plane[4];
    for (int i = 0; i < 4; i++) world_plane[i] = d.view_proj.m[i][1] + d.view_proj.m[i][3];
    float plane[4];
    for (int i = 0; i < 4; i++) {
        plane[i] = 0;
        for (int j = 0; j < 4; j++) plane[i] += d.world.m[i][j] * world_plane[j];
    }
    d.geom = CutAtPlane(d.geom, plane);
}

}  // namespace

RePoseStats RePose(const FrameCapture& in, const Eye& eye, FrameCapture& out) {
    RePoseStats st;
    out = in;
    const WorldCamera cam = WorldCameraOf(in);
    freecam::Pose game{};
    Mat4 proj{};
    if (cam.valid && freecam::Decompose(cam.view_proj, game, proj)) {
        st.ok = true;
        st.cam = cam.cam;
        st.game = game;
        const Mat4 old_vp = cam.view_proj;
        const Mat4 new_vp = freecam::Mul(EyeView(eye), EyeProjection(proj, eye.fov));
        std::vector<int32_t> clone(in.shades.size(), -1);
        for (DrawItem& d : out.draws) {
            if (std::memcmp(&d.view_proj, &old_vp, sizeof(Mat4)) != 0) continue;
            d.view_proj = new_vp;
            st.draws_moved++;
            st.shades_cloned += CloneShade(in, out, d, eye.pose, clone);
        }
    }
    // each track camera's draws as the eye sees the track, its shades their
    // own clones (a track's eye isn't the world's), over the whole picture;
    // a draw with its transform in its matrix keeps it (FoldedInto)
    for (int t = 0; t < eye.track_count && t < kMaxTrackCams; t++) {
        const TrackView& v = eye.tracks[t];
        std::vector<int32_t> clone(in.shades.size(), -1);
        double old_inv[4][4];
        bool inverted = false, tried = false;
        for (DrawItem& d : out.draws) {
            if (d.cam != v.cam || d.target != 0 || d.rect_shader >= 0) continue;
            CutBelowPicture(d, d.shade >= 0 && size_t(d.shade) < in.shades.size()
                                   ? &in.shades[size_t(d.shade)]
                                   : nullptr);
            if (std::memcmp(&d.view_proj, &v.old_vp, sizeof(Mat4)) == 0) {
                d.view_proj = v.view_proj;
            } else {
                if (!tried) {
                    tried = true;
                    inverted = Inverse(v.old_vp, old_inv);
                }
                if (!inverted || !FoldedInto(d.view_proj, old_inv, v.view_proj)) {
                    st.track_unmoved++;
                    continue;
                }
            }
            st.track_draws++;
            st.shades_cloned += CloneShade(in, out, d, v.pose, clone);
        }
        for (CameraView& c : out.cameras) {
            if (c.cam != v.cam) continue;
            c.viewport[0] = c.viewport[1] = 0;
            c.viewport[2] = float(c.target_w);
            c.viewport[3] = float(c.target_h);
        }
    }
    return st;
}

TrackCams TrackCamerasOf(const FrameCapture& fc) {
    TrackCams out;
    if (fc.post_boundary == FrameCapture::kNoPost) return out;
    // the candidates' draws, in order of their first
    struct Seen {
        uint32_t cam, first, draws;
    };
    std::vector<Seen> seen;
    for (size_t d = fc.post_boundary; d < fc.draws.size(); d++) {
        const DrawItem& it = fc.draws[d];
        if (it.target != 0 || it.rect_shader >= 0) continue;
        auto s = std::find_if(seen.begin(), seen.end(), [&](const Seen& e) { return e.cam == it.cam; });
        if (s == seen.end())
            seen.push_back({it.cam, uint32_t(d), 1});
        else
            s->draws++;
    }
    // the flat HUD's camera (z range 0..0.1): the tracks come before it, the
    // pause menu's camera (one more 0..1) after
    uint32_t hud_first = 0;
    bool hud = false;
    for (const Seen& s : seen) {
        const CameraView* view = CameraOf(fc, s.cam);
        if (view && view->zrange[1] < 1) {
            hud = true;
            hud_first = s.first;
            break;
        }
    }
    if (!hud) return out;
    for (const Seen& s : seen) {
        if (out.count == kMaxTrackCams || s.first > hud_first) break;
        const CameraView* view = CameraOf(fc, s.cam);
        if (!view || view->zrange[0] != 0 || view->zrange[1] != 1) continue;
        TrackCamera t;
        t.cam = s.cam;
        t.view_proj = fc.draws[s.first].view_proj;
        if (!freecam::Decompose(t.view_proj, t.pose, t.proj)) continue;
        if (s.draws == 1) {
            // one draw: the fade's quad covers the picture (clip -1.08..1.08
            // across, -1.71..1.71 up); unknown geometry is taken for it
            const DrawItem& it = fc.draws[s.first];
            float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
            bool any = false;
            if (it.geom) {
                for (const Vertex& v : it.geom->verts) {
                    float w[3], c[4];
                    freecam::WorldPos(it, v, w);
                    freecam::Transform(w, it.view_proj, c);
                    if (!(c[3] > 0)) continue;
                    any = true;
                    for (int k = 0; k < 2; k++) {
                        lo[k] = std::fmin(lo[k], c[k] / c[3]);
                        hi[k] = std::fmax(hi[k], c[k] / c[3]);
                    }
                }
            }
            const float edge = 0.99f;
            if (!any || (lo[0] <= -edge && hi[0] >= edge && lo[1] <= -edge && hi[1] >= edge))
                continue;
        }
        for (int k = 0; k < 4; k++) t.viewport[k] = view->viewport[k];
        t.target_w = view->target_w;
        t.target_h = view->target_h;
        t.first_draw = s.first;
        t.draws = s.draws;
        out.cams[out.count++] = t;
    }
    return out;
}

}  // namespace band3::render::vr
