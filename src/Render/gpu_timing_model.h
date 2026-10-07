#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// GPU time per frame by part (native_gpu_timestamps; Direct3D 12 only), for
// native_view stats' by_kind and the slow-frame log. gpu_view.cpp writes one
// timestamp at each part boundary (a ladder, not pairs): each span belongs
// to the part its start began. One queue runs the frame's command buffers in
// order, so the ladder spans them; the gap between buffers is kIdle (GPU
// waiting on the CPU to submit), excluded from the total. Spans with an
// unwritten (0) or backwards timestamp are counted bad, never charged.

namespace band3::render::gpu_timing {

// in a frame's order
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

inline const char* PartName(uint8_t p) {
    static constexpr const char* kNames[kParts] = {
        "upload", "world", "pass_shadow", "pass_spot", "pass_other", "pass_blur",
        "mips", "idle", "copies", "velocity", "dof", "bloom", "composite",
        "overlay", "overlay_resolve", "gamma", "readback", "pre_world", "world_ahead"};
    return p < kParts ? kNames[p] : "none";
}

// One frame's timestamp labels; `capacity` is the frame's query heap slots.
class Ladder {
 public:
    explicit Ladder(uint32_t capacity = 0) : capacity_(capacity) {}

    // The slot for a timestamp starting `part` (kNone ends the ladder), or -1
    // if already in that part or full. The last slot is reserved for the end;
    // parts that don't fit are counted dropped and charged to the last that
    // did.
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

// total_ms excludes kIdle; `bad` counts spans left out
struct Times {
    double ms[kParts] = {};
    double total_ms = 0;
    uint32_t bad = 0;
};

// longer spans are garbage, counted bad
inline constexpr double kMaxSpanMs = 10000;

// Adds a ladder's spans into `t`; `frequency` in ticks per second. Spans
// starting at kNone are gaps.
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
