#include "src/Render/vr_pose.h"

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
    const Mat4 new_vp = freecam::Mul(freecam::View(eye.pose), EyeProjection(proj, eye.fov));
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

}  // namespace band3::render::vr
