#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// The native renderer's GPU timings (native_gpu_timestamps): where a frame's
// time on the GPU goes, by part (the world, its texture passes, post-
// processing, the overlay...), for native_view stats' by_kind and the slow-
// frame log. On Direct3D 12 only: gpu_view.cpp writes a timestamp into the
// command list at each boundary between parts, a ladder rather than pairs,
// so the span from one timestamp to the next is the part the first one
// started (Ladder), and once the GPU has finished the frame its ticks become
// milliseconds by part (Accumulate). One queue runs SDL_gpu's command buffers
// in order, so a ladder carries on across the command buffers a frame takes
// (it's submitted after each texture pass with mips, with
// native_view_submit_points at the resolve too, and with
// native_view_inline_mips off the mips are made in one of their own): the
// span from the last timestamp of one to the first of the next is the part
// it started, kIdle there. The GPU starts on a command buffer once it's
// submitted, and a frame's first is submitted while the CPU still records
// the rest (at the first mips), so between them the GPU waits for the CPU:
// that's kIdle's, which is no work of the frame's and isn't in its total. Spans whose end or
// start the GPU didn't write (0) or that run backwards are left out and
// counted, never charged.

namespace band3::render::gpu_timing {

// What a span of the GPU's time went on, in a frame's order
enum Part : uint8_t {
    kUpload,         // the frame's copy pass: meshes, textures, bones
    kWorld,          // the back buffer's draws before the resolve: the world into the scene
    kPassShadow,     // a shadow map's pass (the characters' self-shadows)
    kPassSpot,       // the spotlights' depth volume and density map
    kPassOther,      // every other texture pass: outfits, the crowd, NgLight's shadow, heads...
    kPassBlur,       // a blur inside a texture pass, its copy included (depth volume, soft particles)
    kMips,           // a texture pass's mips (inline_mips off: in a command buffer of their own)
    kIdle,           // waiting for the CPU to submit the next command buffer (at mips, the resolve)
    kCopies,         // the resolve's copies: the scene and the picture kept for later frames
    kVelocity,       // the camera motion blur's velocity pass and its objects
    kDof,            // depth of field's downsample and blurs
    kBloom,          // bloom's (and glare's) downsamples and blurs
    kComposite,      // post-processing's composite, or the plain resolve into the picture
    kOverlay,        // the back buffer's draws after the resolve: the track, the HUD, the menus
    kOverlayResolve, // a multisampled overlay pass's resolve into the picture, as it ends
    kGamma,          // the gamma ramp's pass into the output
    kReadback,       // the picture copied for reading back (RenderFrame's)
    kPreWorld,       // a world pass before the frame (soft_raster.h's kPreBufferPasses)
    kWorldAhead,     // the world drawn ahead (native_world_ahead), timed with the frame after it
    kParts
};
// a mark that starts no part: the end of a ladder, or a gap that isn't one
inline constexpr uint8_t kNone = 0xff;

// for the harness's by_kind and the slow-frame log
inline const char* PartName(uint8_t p) {
    static constexpr const char* kNames[kParts] = {
        "upload", "world", "pass_shadow", "pass_spot", "pass_other", "pass_blur",
        "mips", "idle", "copies", "velocity", "dof", "bloom", "composite",
        "overlay", "overlay_resolve", "gamma", "readback", "pre_world", "world_ahead"};
    return p < kParts ? kNames[p] : "none";
}

// One frame's timestamps, as recorded: each one's part (what the GPU's time
// goes on from it to the next), at most `capacity` of them (the query
// heap's slots the frame has).
class Ladder {
 public:
    explicit Ladder(uint32_t capacity = 0) : capacity_(capacity) {}

    // The slot for a timestamp from which the GPU's time goes to `part`
    // (kNone: nothing, the ladder's end), or -1 for none: the time goes there
    // already (no new span), or the ladder is full. The last slot is kept
    // for the end, so a full ladder still ends: the parts that didn't fit
    // are counted (dropped) and their time charged to the last that did.
    int Mark(uint8_t part) {
        const uint8_t now = labels_.empty() ? kNone : labels_.back();
        if (part == now) return -1;
        const size_t room = part == kNone ? capacity_ : (capacity_ ? capacity_ - 1 : 0);
        if (labels_.size() >= room) {
            if (part != kNone) dropped_++;
            return -1;
        }
        labels_.push_back(part);
        return int(labels_.size() - 1);
    }
    void Reset() {
        labels_.clear();
        dropped_ = 0;
    }
    uint32_t Count() const { return uint32_t(labels_.size()); }
    bool Open() const { return !labels_.empty() && labels_.back() != kNone; }
    uint32_t Dropped() const { return dropped_; }
    const std::vector<uint8_t>& Labels() const { return labels_; }

 private:
    uint32_t capacity_;
    std::vector<uint8_t> labels_;
    uint32_t dropped_ = 0;
};

// A frame's milliseconds by part, and all of them but kIdle's (the GPU busy
// on the frame); `bad` the spans left out
struct Times {
    double ms[kParts] = {};
    double total_ms = 0;
    uint32_t bad = 0;
};

// a span longer than this is no frame's: garbage, left out as bad
inline constexpr double kMaxSpanMs = 10000;

// Adds the spans of a ladder of `count` timestamps (`labels` each one's part,
// `ticks` what the GPU wrote, at `frequency` ticks a second) into `t`. A
// span starting at kNone is a gap and adds nothing, one at kIdle adds to its
// part but not the total; one with a 0 end, one
// that runs backwards or one over kMaxSpanMs is counted bad and adds nothing.
inline void Accumulate(const uint8_t* labels, const uint64_t* ticks, size_t count,
                       uint64_t frequency, Times& t) {
    if (!frequency) return;
    for (size_t i = 0; i + 1 < count; i++) {
        if (labels[i] >= kParts) continue;
        if (!ticks[i] || !ticks[i + 1] || ticks[i + 1] < ticks[i]) {
            t.bad++;
            continue;
        }
        const double ms = double(ticks[i + 1] - ticks[i]) * 1000.0 / double(frequency);
        if (ms > kMaxSpanMs) {
            t.bad++;
            continue;
        }
        t.ms[labels[i]] += ms;
        if (labels[i] != kIdle) t.total_ms += ms;
    }
}

}  // namespace band3::render::gpu_timing
