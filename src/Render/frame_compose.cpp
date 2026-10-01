#include "src/Render/frame_compose.h"

#include <algorithm>
#include <set>
#include <utility>

// See frame_compose.h.

namespace band3::render {
namespace {

uint64_t RtKey(uint32_t tex, uint32_t version) { return uint64_t(tex) << 32 | version; }

// draws [first, end) of `from` to the end of `to`, in the passes they're in
// (cut to the range; the draws of a live capture are all in one), their
// shades moved on by `shade_base`; texture passes only if `textures_only`,
// and none of those `skip` has
void AppendRange(FrameCapture& to, const FrameCapture& from, uint32_t first, uint32_t end,
                 int32_t shade_base, bool textures_only, const std::set<uint64_t>* skip = nullptr) {
    const uint32_t n = uint32_t(from.draws.size());
    end = std::min(end, n);
    for (const Pass& p : from.passes) {
        const uint32_t a = std::max(p.first_draw, first);
        const uint32_t b = std::min(std::min(p.first_draw + p.draw_count, n), end);
        if (a >= b) continue;
        if (textures_only && !p.tex_obj) continue;
        if (skip && p.tex_obj && skip->count(RtKey(p.tex_obj, p.version))) continue;
        Pass q = p;
        q.first_draw = uint32_t(to.draws.size());
        q.draw_count = b - a;
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
          &FrameCapture::passes_unbalanced})
        to.*f += from.*f;
}

}  // namespace

void CountRenderTargets(const FrameCapture& fc, uint32_t& sampled, uint32_t& missing) {
    std::set<uint64_t> made, seen;
    for (const Pass& p : fc.passes)
        if (p.tex_obj) made.insert(RtKey(p.tex_obj, p.version));
    missing = 0;
    for (const DrawItem& d : fc.draws) {
        if (!d.tex || !d.tex->tex_obj || !IsPassTargetType(d.tex->tex_type)) continue;
        const uint64_t key = RtKey(d.tex->tex_obj, d.tex->version);
        if (seen.insert(key).second && !made.count(key)) missing++;
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

    // shades: the world's, then the frame's
    fc.shades.reserve(world.shades.size() + frame.shades.size());
    fc.shades.insert(fc.shades.end(), world.shades.begin(), world.shades.end());
    fc.shades.insert(fc.shades.end(), frame.shades.begin(), frame.shades.end());
    const int32_t frame_shades = int32_t(world.shades.size());
    fc.draws.reserve(world.draws.size() + frame.draws.size());

    const uint32_t world_end = ProcKnown(world) ? world.post_boundary : uint32_t(world.draws.size());
    const uint32_t frame_post = ProcKnown(frame) ? frame.post_boundary : 0;
    AppendRange(fc, world, 0, world_end, 0, false);
    std::set<uint64_t> have;
    for (const Pass& p : fc.passes)
        if (p.tex_obj) have.insert(RtKey(p.tex_obj, p.version));
    AppendRange(fc, frame, 0, frame_post, frame_shades, true, &have);
    fc.post_boundary = uint32_t(fc.draws.size());
    AppendRange(fc, frame, frame_post, uint32_t(frame.draws.size()), frame_shades, false);

    AddCounts(fc, world);
    AddCounts(fc, frame);
    for (const Pass& p : fc.passes) {
        if (!p.tex_obj) continue;
        if (p.from_frame >= world.game_frame) fc.passes_own++;
        else fc.passes_carried++;
    }
    CountRenderTargets(fc, fc.rt_sampled, fc.rt_missing);
    return out;
}

}  // namespace band3::render
