#include "src/Render/vr_pose.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "src/Render/camera_cut.h"
#include "src/Render/vr_venue.h"

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

// A track camera's draw cut where the game's picture ends below (clip y =
// -w, the bottom of the camera's viewport, through its captured matrix), so
// the track before the strikeline ends where the game shows it end rather
// than reaching under the head. In the mesh's own space: the plane through
// its world. Skinned and billboard draws are left whole.
std::shared_ptr<const Geometry> CutBelowPicture(const DrawItem& d, const ShadeState* s,
                                                const Mat4& vp) {
    if (!d.bones.empty() || (s && s->Option(shader_opt::kBillboard))) return d.geom;
    float world_plane[4];
    for (int i = 0; i < 4; i++) world_plane[i] = vp.m[i][1] + vp.m[i][3];
    float plane[4];
    for (int i = 0; i < 4; i++) {
        plane[i] = 0;
        for (int j = 0; j < 4; j++) plane[i] += d.world.m[i][j] * world_plane[j];
    }
    return CutAtPlane(d.geom, plane);
}

namespace {

// a quad's four corners and two triangles onto `g`
void AddQuad(Geometry& g, const float corners[4][3]) {
    const uint16_t base = uint16_t(g.verts.size());
    for (int k = 0; k < 4; k++) {
        Vertex v{};
        for (int c = 0; c < 3; c++) v.pos[c] = corners[k][c];
        v.color = 0xffffffffu;
        g.verts.push_back(v);
    }
    g.indices.insert(g.indices.end(), {base, uint16_t(base + 1), uint16_t(base + 2), base,
                                        uint16_t(base + 2), uint16_t(base + 3)});
}

// a black draw of `g` through `vp` at `alpha`, blended, without depth, by
// kEyeDarkCam
DrawItem DarkDraw(std::shared_ptr<const Geometry> g, const Mat4& vp, float alpha) {
    DrawItem d{};
    d.geom = std::move(g);
    for (int i = 0; i < 4; i++) d.world.m[i][i] = 1;
    d.view_proj = vp;
    d.color[3] = alpha;
    d.blend = 3;   // SrcAlpha
    d.z_mode = 0;  // no depth test or write
    d.prelit = true;
    d.cam = kEyeDarkCam;
    return d;
}

// The eye's own dark (Eye::fade, Eye::dark) into `out`, after the resolve
// and before its first track draw (or last): the fade a quad over the whole
// picture in clip space; the dark's quads in LOCAL space (metres, as
// Milo's axes: XrToMilo) seen from the eye's XR pose with its field of view
void AddDark(const Eye& eye, FrameCapture& out) {
    if (out.post_boundary == FrameCapture::kNoPost || out.post_boundary > out.draws.size()) return;
    std::vector<DrawItem> add;
    if (eye.fade > 0) {
        static const std::shared_ptr<const Geometry> picture = [] {
            auto g = std::make_shared<Geometry>();
            const float c[4][3] = {{-1, 1, 0.5f}, {1, 1, 0.5f}, {1, -1, 0.5f}, {-1, -1, 0.5f}};
            AddQuad(*g, c);
            return g;
        }();
        Mat4 identity{};
        for (int i = 0; i < 4; i++) identity.m[i][i] = 1;
        add.push_back(DarkDraw(picture, identity, std::fmin(eye.fade, 1.0f)));
    }
    if (eye.dark_count > 0 && eye.dark_alpha > 0) {
        // both eyes see the same quads: made once
        thread_local float last[kFrameQuads][4][3] = {};
        thread_local int last_count = 0;
        thread_local std::shared_ptr<const Geometry> quads;
        if (!quads || last_count != eye.dark_count ||
            std::memcmp(last, eye.dark, sizeof(last)) != 0) {
            auto g = std::make_shared<Geometry>();
            for (int q = 0; q < eye.dark_count && q < kFrameQuads; q++) {
                float milo[4][3];
                for (int k = 0; k < 4; k++) XrToMilo(eye.dark[q][k], 1, milo[k]);
                AddQuad(*g, milo);
            }
            quads = g;
            std::memcpy(last, eye.dark, sizeof(last));
            last_count = eye.dark_count;
        }
        // metres: the eye's pose at 1/kUnitsPerMetre of the venue's scale;
        // a centimetre to a kilometre
        const freecam::Pose pose = EyePose(eye.xr, Anchor{}, 1 / kUnitsPerMetre);
        Mat4 proj{};
        const float n = 0.01f, f = 1000;
        proj.m[1][2] = f / (f - n);
        proj.m[3][2] = -n * f / (f - n);
        proj.m[1][3] = 1;
        add.push_back(DarkDraw(quads, freecam::Mul(freecam::View(pose), EyeProjection(proj, eye.fov)),
                               eye.dark_alpha));
    }
    if (add.empty()) return;
    // before the first track draw, after the resolve
    size_t at = out.draws.size();
    for (int t = 0; t < eye.track_count; t++)
        for (size_t d = out.post_boundary; d < at; d++)
            if (out.draws[d].cam == eye.tracks[t].cam && out.draws[d].target == 0 &&
                out.draws[d].rect_shader < 0) {
                at = d;
                break;
            }
    out.draws.insert(out.draws.begin() + std::ptrdiff_t(at), add.begin(), add.end());
    // Passes after them move on; the back buffer's run they're in (or end)
    // takes them in, else one of their own
    const uint32_t n = uint32_t(add.size());
    bool taken = out.passes.empty();
    for (Pass& p : out.passes) {
        if (p.first_draw >= at && !(p.tex_obj == 0 && p.first_draw == at && !taken)) {
            p.first_draw += n;
        } else if (p.tex_obj == 0 && !taken && p.first_draw + p.draw_count >= at) {
            p.draw_count += n;
            taken = true;
        }
    }
    if (!taken) {
        Pass run{};
        run.first_draw = uint32_t(at);
        run.draw_count = n;
        auto after = std::find_if(out.passes.begin(), out.passes.end(),
                                  [&](const Pass& p) { return p.first_draw > at; });
        out.passes.insert(after, run);
    }
}

}  // namespace

namespace {

// Whether draw `d` stands within `radius` of `eye` across the floor (x, y):
// the middle of a few of its vertices in the world (a card's four; a crowd
// card's instance world doesn't put its origin at the card)
bool NearEye(const DrawItem& d, const float eye[3], float radius) {
    if (!d.geom || d.geom->verts.empty()) return false;
    const size_t n = d.geom->verts.size(), step = std::max<size_t>(1, n / 8);
    float sum[2] = {0, 0};
    int k = 0;
    for (size_t i = 0; i < n; i += step, k++) {
        float w[3];
        freecam::WorldPos(d, d.geom->verts[i], w);
        sum[0] += w[0];
        sum[1] += w[1];
    }
    const float dx = sum[0] / float(k) - eye[0], dy = sum[1] / float(k) - eye[1];
    return dx * dx + dy * dy < radius * radius;
}

}  // namespace

void SquashMatrix(Mat4& m) {
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 4; c++) m.m[r][c] = 0;
}

bool EyeOverrideOf(const FrameCapture& in, const Eye& eye, EyeOverride& out) {
    // the crowd list's room kept from the last
    std::vector<uint32_t> crowd = std::move(out.crowd);
    crowd.clear();
    out = EyeOverride{};
    out.crowd = std::move(crowd);
    const WorldCamera cam = WorldCameraOf(in);
    Mat4 proj{};
    if (cam.valid && freecam::Decompose(cam.view_proj, out.game, proj)) {
        out.ok = true;
        out.cam = cam.cam;
        ViewSwap& w = out.swap[out.count++];
        w.from = cam.view_proj;
        w.to = freecam::Mul(EyeView(eye), EyeProjection(proj, eye.fov));
        w.pose = eye.pose;
        // the crowds' meshes, for Eye::crowd_clear
        if (eye.crowd_clear > 0) {
            for (const MeshOwner& o : in.mesh_owners)
                if (o.kind == kOwnerCrowd) out.crowd.push_back(o.mesh);
            std::sort(out.crowd.begin(), out.crowd.end());
            out.crowd_clear = eye.crowd_clear;
            for (int c = 0; c < 3; c++) out.eye[c] = eye.pose.eye[c];
        }
    }
    out.tracks = out.count;
    for (int t = 0; t < eye.track_count && t < kMaxTrackCams; t++) {
        const TrackView& v = eye.tracks[t];
        ViewSwap& s = out.swap[out.count++];
        s.from = v.old_vp;
        s.to = v.view_proj;
        s.pose = v.pose;
        s.cam = v.cam;
        s.inverted = Inverse(v.old_vp, s.from_inv);
    }
    out.dark = eye.fade > 0 || (eye.dark_count > 0 && eye.dark_alpha > 0);
    return !out.dark;
}

EyeDraw EyeDrawOf(const EyeOverride& v, const FrameCapture& in, const DrawItem& d) {
    EyeDraw e;
    // the world camera's draws, in any pass
    if (v.ok && std::memcmp(&d.view_proj, &v.swap[0].from, sizeof(Mat4)) == 0) {
        e.matrix = EyeDraw::kSwap;
        e.swap = 0;
        e.pose = 0;
        e.squash = !v.crowd.empty() && std::binary_search(v.crowd.begin(), v.crowd.end(), d.mesh) &&
                   NearEye(d, v.eye, v.crowd_clear);
    }
    // each track camera's, as the eye sees the track, over the whole
    // picture; a draw with its transform in its matrix keeps it
    // (FoldedInto); its shade the track's eye's unless the world's took it
    if (d.target != 0 || d.rect_shader >= 0) return e;
    for (int t = v.tracks; t < v.count; t++) {
        const ViewSwap& s = v.swap[t];
        if (d.cam != s.cam) continue;
        const Mat4& now = EyeViewProj(v, e, d);
        e.cut = true;
        e.cut_view_proj = now;
        if (std::memcmp(&now, &s.from, sizeof(Mat4)) == 0) {
            e.matrix = EyeDraw::kSwap;
            e.swap = int8_t(t);
        } else {
            Mat4 folded = now;
            if (!s.inverted || !FoldedInto(folded, s.from_inv, s.to)) {
                e.unmoved = true;
                continue;
            }
            e.matrix = EyeDraw::kFolded;
            e.view_proj = folded;
        }
        if (e.pose < 0) e.pose = int8_t(t);
    }
    return e;
}

RePoseStats RePose(const FrameCapture& in, const Eye& eye, FrameCapture& out) {
    RePoseStats st;
    out = in;
    EyeOverride v;
    EyeOverrideOf(in, eye, v);
    st.ok = v.ok;
    if (v.ok) {
        st.cam = v.cam;
        st.game = v.game;
        st.view_proj = v.swap[0].to;
    }
    std::vector<EyeDraw> e(in.draws.size());
    for (size_t i = 0; i < in.draws.size(); i++) {
        e[i] = EyeDrawOf(v, in, in.draws[i]);
        DrawItem& d = out.draws[i];
        if (e[i].cut) {
            // the shade as the track's step found it: none where the
            // world's cloned it
            const ShadeState* s = e[i].pose == 0 || d.shade < 0 ||
                                          size_t(d.shade) >= in.shades.size()
                                      ? nullptr
                                      : &in.shades[size_t(d.shade)];
            d.geom = CutBelowPicture(d, s, e[i].cut_view_proj);
        }
        d.view_proj = EyeViewProj(v, e[i], in.draws[i]);
        if (e[i].squash) {
            SquashMatrix(d.world);
            for (Mat4& b : d.bones) SquashMatrix(b);
            st.crowd_cleared++;
        }
        if (e[i].pose == 0) st.draws_moved++;
        else if (e[i].pose > 0) st.track_draws++;
        if (e[i].unmoved) st.track_unmoved++;
    }
    // their shades cloned with the eye's point and axes, the world's first,
    // then each track's (a track's eye isn't the world's)
    for (int s = 0; s < v.count; s++) {
        std::vector<int32_t> clone(in.shades.size(), -1);
        for (size_t i = 0; i < in.draws.size(); i++)
            if (e[i].pose == s)
                st.shades_cloned += CloneShade(in, out, out.draws[i], v.swap[s].pose, clone);
    }
    for (int t = v.tracks; t < v.count; t++) {
        for (CameraView& c : out.cameras) {
            if (c.cam != v.swap[t].cam) continue;
            c.viewport[0] = c.viewport[1] = 0;
            c.viewport[2] = float(c.target_w);
            c.viewport[3] = float(c.target_h);
        }
    }
    AddDark(eye, out);
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

VenueRef VenueRefOf(const FrameCapture& fc) {
    VenueRef r;
    const WorldCamera cam = WorldCameraOf(fc);
    if (!cam.valid || fc.mesh_owners.empty()) return r;
    std::vector<MeshOwner> owners = fc.mesh_owners;
    std::sort(owners.begin(), owners.end(),
              [](const MeshOwner& a, const MeshOwner& b) { return a.mesh < b.mesh; });
    const size_t end = fc.post_boundary == FrameCapture::kNoPost
                           ? fc.draws.size()
                           : std::min<size_t>(fc.post_boundary, fc.draws.size());
    struct Box {
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        bool any = false;
    };
    // a draw's vertices in the world, about `samples` of them
    auto box_of = [](const DrawItem& d, size_t samples, Box& b) {
        const size_t n = d.geom->verts.size(), step = std::max<size_t>(1, n / samples);
        for (size_t i = 0; i < n; i += step) {
            float w[3];
            freecam::WorldPos(d, d.geom->verts[i], w);
            for (int c = 0; c < 3; c++) {
                b.lo[c] = std::fmin(b.lo[c], w[c]);
                b.hi[c] = std::fmax(b.hi[c], w[c]);
            }
            b.any = true;
        }
    };
    Box band[kMaxMembers];
    std::vector<std::array<float, 3>> crowd;
    for (size_t i = 0; i < end; i++) {
        const DrawItem& d = fc.draws[i];
        if (d.target != 0 || d.rect_shader >= 0 || d.draw_mode != kDrawModeNormal || !d.geom ||
            d.geom->verts.empty() || std::memcmp(&d.view_proj, &cam.view_proj, sizeof(Mat4)) != 0)
            continue;
        auto o = std::lower_bound(owners.begin(), owners.end(), d.mesh,
                                  [](const MeshOwner& m, uint32_t mesh) { return m.mesh < mesh; });
        if (o == owners.end() || o->mesh != d.mesh) continue;
        if (o->kind == kOwnerBand) {
            int m = 0;
            while (m < r.members && r.owner[m] != o->owner) m++;
            if (m == r.members) {
                if (r.members == kMaxMembers) continue;
                r.owner[r.members++] = o->owner;
            }
            // the feet are few of a body's vertices: enough samples to find them
            box_of(d, 512, band[m]);
        } else if (o->kind == kOwnerCrowd) {
            // a card's four corners; a 3D crowd member's feet are few of
            // its vertices, as a band member's
            Box b;
            box_of(d, d.bones.empty() ? 16 : 512, b);
            if (b.any) crowd.push_back({(b.lo[0] + b.hi[0]) / 2, (b.lo[1] + b.hi[1]) / 2, b.lo[2]});
        }
    }
    int kept = 0;
    for (int m = 0; m < r.members; m++) {
        if (!band[m].any) continue;
        r.owner[kept] = r.owner[m];
        r.member[kept][0] = (band[m].lo[0] + band[m].hi[0]) / 2;
        r.member[kept][1] = (band[m].lo[1] + band[m].hi[1]) / 2;
        r.member[kept][2] = band[m].lo[2];
        kept++;
    }
    r.members = kept;
    if (!kept) return r;
    // the stage floor: the members' median feet (a member on a riser
    // doesn't move it)
    float feet[kMaxMembers];
    for (int m = 0; m < kept; m++) {
        feet[m] = r.member[m][2];
        r.centre[0] += r.member[m][0] / float(kept);
        r.centre[1] += r.member[m][1] / float(kept);
    }
    std::sort(feet, feet + kept);
    r.centre[2] = feet[kept / 2];
    // toward the crowd's middle, where there's a crowd off the band's middle
    const float metre = kUnitsPerMetre;
    r.crowd = int(crowd.size());
    if (crowd.size() >= 10) {
        double sum[2] = {0, 0};
        for (const auto& p : crowd) {
            sum[0] += p[0];
            sum[1] += p[1];
        }
        const float d[2] = {float(sum[0] / double(crowd.size())) - r.centre[0],
                            float(sum[1] / double(crowd.size())) - r.centre[1]};
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1]);
        if (len > 2 * metre) {
            r.toward[0] = d[0] / len;
            r.toward[1] = d[1] / len;
            r.toward_crowd = true;
        }
    }
    r.across[0] = -r.toward[1];
    r.across[1] = r.toward[0];
    auto depth = [&](const float* p) {
        return (p[0] - r.centre[0]) * r.toward[0] + (p[1] - r.centre[1]) * r.toward[1];
    };
    auto across = [&](const float* p) {
        return (p[0] - r.centre[0]) * r.across[0] + (p[1] - r.centre[1]) * r.across[1];
    };
    r.band_front = -1e30f;
    r.band_back = 1e30f;
    for (int m = 0; m < kept; m++) {
        r.band_front = std::fmax(r.band_front, depth(r.member[m]));
        r.band_back = std::fmin(r.band_back, depth(r.member[m]));
        r.band_half = std::fmax(r.band_half, std::fabs(across(r.member[m])));
    }
    // the crowd in front of the band, as wide as it and 4 m either side, and
    // of that the part straight ahead of the band's middle (2 m either side),
    // where the front row stands: around a wide stage the crowd's wings can
    // reach further forward than its middle
    std::vector<float> ahead, middle, floor;
    for (const auto& p : crowd) {
        const float dp = depth(p.data()), side = std::fabs(across(p.data()));
        if (dp <= r.band_front + 0.5f * metre || side >= r.band_half + 4 * metre) continue;
        ahead.push_back(dp);
        if (side < 2 * metre) {
            middle.push_back(dp);
            floor.push_back(p[2]);
        }
    }
    if (ahead.size() >= 5) {
        std::sort(ahead.begin(), ahead.end());
        r.crowd_ahead = int(ahead.size());
        r.crowd_back = ahead[(ahead.size() - 1) * 9 / 10];
        // without a middle, the whole front's
        if (middle.size() < 3) {
            middle = ahead;
            floor.clear();
            for (const auto& p : crowd)
                if (depth(p.data()) > r.band_front + 0.5f * metre &&
                    std::fabs(across(p.data())) < r.band_half + 4 * metre)
                    floor.push_back(p[2]);
        }
        std::sort(middle.begin(), middle.end());
        std::sort(floor.begin(), floor.end());
        // a few stray cards in front of the rest don't count
        r.crowd_front = middle[(middle.size() - 1) / 50];
        // never over the stage's: no venue's audience looks down on the band
        r.audience_floor = std::fmin(floor[floor.size() / 2], r.centre[2]);
    }
    return r;
}

bool PresetAnchor(ViewMode mode, const VenueRef& r, const ViewOffset& off, float units,
                  Anchor& out) {
    if (mode == ViewMode::kGameCamera || !r.Valid()) return false;
    const float metre = kUnitsPerMetre;
    float depth = 0, side = 0, z = 0;
    // facing the band (front_row, venue) or the audience (on_stage)
    float face[2] = {-r.toward[0], -r.toward[1]};
    if (mode == ViewMode::kFrontRow) {
        depth = r.crowd_ahead ? r.crowd_front - 0.3f * metre : r.band_front + 5 * metre;
        depth = std::fmax(depth, r.band_front + 2 * metre);
        z = (r.crowd_ahead ? r.audience_floor : r.centre[2] - 1.2f * metre) + kEyeHeight * units;
    } else if (mode == ViewMode::kOnStage) {
        face[0] = r.toward[0];
        face[1] = r.toward[1];
        z = r.centre[2] + kEyeHeight * units;
        // the members in depth and across, and the place sought among them
        float at[kMaxMembers][2];
        for (int m = 0; m < r.members; m++) {
            const float d[2] = {r.member[m][0] - r.centre[0], r.member[m][1] - r.centre[1]};
            at[m][0] = d[0] * r.toward[0] + d[1] * r.toward[1];
            at[m][1] = d[0] * r.across[0] + d[1] * r.across[1];
        }
        // the band's front line (a drummer on a riser far upstage doesn't
        // pull it back): the median member's depth
        float depths[kMaxMembers];
        for (int m = 0; m < r.members; m++) depths[m] = at[m][0];
        std::sort(depths, depths + r.members);
        const float line = r.members % 2 ? depths[r.members / 2]
                                         : (depths[r.members / 2 - 1] + depths[r.members / 2]) / 2;
        const float target = line + 0.5f * metre, step = 0.25f * metre;
        float best = -1e30f;
        depth = r.band_front + 0.5f * metre;
        for (float dp = r.band_back; dp <= r.band_front + 0.5f * metre + 1e-3f; dp += step) {
            for (float sd = -(r.band_half + metre); sd <= r.band_half + metre + 1e-3f; sd += step) {
                // clear of every member, none of them between it and the
                // audience (within 0.5 m and a widening cone ahead)
                float nearest = 1e30f;
                bool blocked = false;
                for (int m = 0; m < r.members; m++) {
                    nearest = std::fmin(nearest, std::hypot(dp - at[m][0], sd - at[m][1]));
                    const float ahead = at[m][0] - dp;
                    blocked |= ahead > 0 && std::fabs(at[m][1] - sd) < 0.5f * metre + 0.3f * ahead;
                }
                if (nearest < 0.9f * metre || blocked) continue;
                const float score = std::fmin(nearest, 1.2f * metre) -
                                    0.5f * std::fabs(dp - target) - 0.25f * std::fabs(sd);
                if (score > best) {
                    best = score;
                    depth = dp;
                    side = sd;
                }
            }
        }
    } else {
        depth = r.crowd_ahead ? r.crowd_back + 2 * metre : r.band_front + 15 * metre;
        const float rise =
            std::fmin(std::fmax(0.35f * (depth - r.band_front), 2.5f * metre), 7 * metre);
        z = r.centre[2] + rise;
    }
    // the facing's right (x right of y forward): (f.y, -f.x)
    const float right[2] = {face[1], -face[0]};
    for (int c = 0; c < 2; c++)
        out.pos[c] = r.centre[c] + depth * r.toward[c] + side * r.across[c] +
                     off.right * metre * right[c] + off.ahead * metre * face[c];
    out.pos[2] = z + off.up * metre;
    out.yaw = std::atan2(-face[0], face[1]) + off.yaw_deg * 3.14159265f / 180;
    return true;
}

}  // namespace band3::render::vr
