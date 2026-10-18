#include "src/Render/vr_pose.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "src/Render/camera_cut.h"

namespace band3::render::vr {

RePoseStats RePose(const FrameCapture& in, const Eye& eye, FrameCapture& out) {
    RePoseStats st;
    out = in;
    const WorldCamera cam = WorldCameraOf(in);
    freecam::Pose game{};
    Mat4 proj{};
    if (!cam.valid || !freecam::Decompose(cam.view_proj, game, proj)) return st;
    st.ok = true;
    st.cam = cam.cam;
    st.game = game;
    const Mat4 old_vp = cam.view_proj;
    const Mat4 new_vp = freecam::Mul(EyeView(eye), EyeProjection(proj, eye.fov));
    // each moved draw's shade, cloned once
    std::vector<int32_t> clone(in.shades.size(), -1);
    for (DrawItem& d : out.draws) {
        if (std::memcmp(&d.view_proj, &old_vp, sizeof(Mat4)) != 0) continue;
        d.view_proj = new_vp;
        st.draws_moved++;
        if (d.shade < 0 || size_t(d.shade) >= in.shades.size()) continue;
        int32_t& c = clone[size_t(d.shade)];
        if (c < 0) {
            ShadeState s = in.shades[size_t(d.shade)];
            for (int i = 0; i < 3; i++) s.eye[i] = eye.pose.eye[i];
            // a billboard faces the eye: c16..c18's columns are its right,
            // up and forward (shade.hlsli's Billboard)
            if (s.Option(shader_opt::kBillboard)) {
                const int i16 = ShadeRegIndex(16), i17 = ShadeRegIndex(17),
                          i18 = ShadeRegIndex(18);
                if (i16 >= 0 && i17 >= 0 && i18 >= 0) {
                    const int rows[3] = {i16, i17, i18};
                    for (int k = 0; k < 3; k++) {
                        s.vs[rows[k]][0] = eye.pose.right[k];
                        s.vs[rows[k]][1] = eye.pose.up[k];
                        s.vs[rows[k]][2] = eye.pose.fwd[k];
                    }
                }
            }
            c = int32_t(out.shades.size());
            out.shades.push_back(std::make_shared<ShadeState>(std::move(s)));
            st.shades_cloned++;
        }
        d.shade = c;
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
