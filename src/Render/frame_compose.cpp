#include "src/Render/frame_compose.h"

#include <algorithm>
#include <iterator>
#include <set>
#include <utility>
#include <vector>

// See frame_compose.h.

namespace band3::render {
namespace {

uint64_t RtKey(uint32_t tex, uint32_t version) { return uint64_t(tex) << 32 | version; }

// draws [first, end) of `from` to the end of `to`, in the passes they're in
// (cut to the range; the draws of a live capture are all in one), their
// shades moved on by `shade_base`; texture passes only if `textures_only`,
// and none of those `skip` has. A texture pass with no draws (one that only
// cleared) is in the range it starts in, and one after the last draw in the
// range that runs to the frame's end (`to_end`).
void AppendRange(FrameCapture& to, const FrameCapture& from, uint32_t first, uint32_t end,
                 int32_t shade_base, bool textures_only, bool to_end,
                 const std::set<uint64_t>* skip = nullptr) {
    const uint32_t n = uint32_t(from.draws.size());
    end = std::min(end, n);
    for (const Pass& p : from.passes) {
        const uint32_t a = std::max(p.first_draw, first);
        const uint32_t b = std::min(std::min(p.first_draw + p.draw_count, n), end);
        const bool clear_only = p.tex_obj && !p.draw_count && p.first_draw >= first &&
                                (p.first_draw < end || (to_end && p.first_draw == n));
        if (a >= b && !clear_only) continue;
        if (textures_only && !p.tex_obj) continue;
        if (skip && p.tex_obj && skip->count(RtKey(p.tex_obj, p.version))) continue;
        Pass q = p;
        q.first_draw = uint32_t(to.draws.size());
        q.draw_count = clear_only ? 0 : b - a;
        for (uint32_t d = a; d < b; d++) {
            DrawItem item = from.draws[d];
            if (item.shade >= 0) item.shade += shade_base;
            to.draws.push_back(std::move(item));
        }
        to.passes.push_back(std::move(q));
    }
}

// what both frames skipped, decoded and so on
void AddCounts(FrameCapture& to, const FrameCapture& from) {
    for (uint32_t FrameCapture::*f :
         {&FrameCapture::cams, &FrameCapture::skipped_target, &FrameCapture::skipped_velocity,
          &FrameCapture::skipped_shadow, &FrameCapture::skipped_draw_mode,
          &FrameCapture::skipped_no_geom, &FrameCapture::mutable_meshes,
          &FrameCapture::multimesh_instances, &FrameCapture::particles, &FrameCapture::textured,
          &FrameCapture::untextured_format, &FrameCapture::geom_cached, &FrameCapture::tex_cached,
          &FrameCapture::maps_decoded, &FrameCapture::maps_cube, &FrameCapture::maps_other_format,
          &FrameCapture::passes_empty, &FrameCapture::rt_snapshots,
          &FrameCapture::passes_unbalanced, &FrameCapture::later_passes,
          &FrameCapture::skipped_no_mat, &FrameCapture::faces_elsewhere})
        to.*f += from.*f;
}

}  // namespace

void CountRenderTargets(const FrameCapture& fc, const std::vector<uint64_t>& left_out,
                        uint32_t& sampled, uint32_t& missing, std::vector<uint64_t>& filtered) {
    std::set<uint64_t> made, seen;
    for (const Pass& p : fc.passes)
        if (p.tex_obj) made.insert(RtKey(p.tex_obj, p.version));
    const std::set<uint64_t> all_left_out(left_out.begin(), left_out.end());
    missing = 0;
    filtered.clear();
    auto count = [&](const Texture* t) {
        if (!t || !t->tex_obj || !IsPassTargetType(t->tex_type)) return;
        const uint64_t key = RtKey(t->tex_obj, t->version);
        if (!seen.insert(key).second || made.count(key)) return;
        if (all_left_out.count(key)) {
            filtered.push_back(key);
        } else {
            missing++;
        }
    };
    for (const DrawItem& d : fc.draws) {
        count(d.tex.get());
        // s5 kept as a render target (the shadow map, NgLight's shadow)
        if (d.shade >= 0 && size_t(d.shade) < fc.shades.size())
            count(fc.shades[d.shade].maps[kMapProjected].get());
    }
    sampled = uint32_t(seen.size());
}

std::shared_ptr<FrameCapture> ComposeFrame(const FrameCapture& world, const FrameCapture& frame) {
    auto out = std::make_shared<FrameCapture>();
    FrameCapture& fc = *out;
    fc.frame = frame.frame;
    fc.game_frame = frame.game_frame;
    fc.proc_cmds = frame.proc_cmds;
    fc.composed = 1;
    fc.world_frame = world.game_frame;
    // post-processing as the post frame ran it, on the world it presents
    fc.post = frame.post;
    fc.post_consts = frame.post_consts;
    fc.noise_map = frame.noise_map;
    fc.noise_sampler = frame.noise_sampler;
    // and the gamma ramp the presenter applied to it
    fc.gamma = frame.gamma;
    // the world's back buffer, cleared as the world frame cleared it, and its
    // cameras, then the frame's others (the overlay's)
    fc.has_clear_color = world.has_clear_color;
    std::copy(std::begin(world.clear_color), std::end(world.clear_color), fc.clear_color);
    fc.cameras = world.cameras;
    for (const CameraView& c : frame.cameras)
        if (!CameraOf(fc, c.cam)) fc.cameras.push_back(c);

    // shades: the world's, then the frame's
    fc.shades.reserve(world.shades.size() + frame.shades.size());
    fc.shades.insert(fc.shades.end(), world.shades.begin(), world.shades.end());
    fc.shades.insert(fc.shades.end(), frame.shades.begin(), frame.shades.end());
    const int32_t frame_shades = int32_t(world.shades.size());
    fc.draws.reserve(world.draws.size() + frame.draws.size());

    const uint32_t world_end = ProcKnown(world) ? world.post_boundary : uint32_t(world.draws.size());
    const uint32_t frame_post = ProcKnown(frame) ? frame.post_boundary : 0;
    AppendRange(fc, world, 0, world_end, 0, false, !ProcKnown(world));
    std::set<uint64_t> have;
    for (const Pass& p : fc.passes)
        if (p.tex_obj) have.insert(RtKey(p.tex_obj, p.version));
    AppendRange(fc, frame, 0, frame_post, frame_shades, true, false, &have);
    fc.post_boundary = uint32_t(fc.draws.size());
    AppendRange(fc, frame, frame_post, uint32_t(frame.draws.size()), frame_shades, false, true);

    AddCounts(fc, world);
    AddCounts(fc, frame);
    for (const Pass& p : fc.passes) {
        if (!p.tex_obj) continue;
        if (p.from_frame >= world.game_frame) fc.passes_own++;
        else fc.passes_carried++;
    }
    // a target whose pass's draws were all left out, in either frame
    std::vector<uint64_t> left_out = world.rt_filtered_keys;
    left_out.insert(left_out.end(), frame.rt_filtered_keys.begin(), frame.rt_filtered_keys.end());
    CountRenderTargets(fc, left_out, fc.rt_sampled, fc.rt_missing, fc.rt_filtered_keys);
    fc.rt_filtered = uint32_t(fc.rt_filtered_keys.size());
    return out;
}

}  // namespace band3::render
