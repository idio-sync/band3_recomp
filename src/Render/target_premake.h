#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "src/Render/scene_capture.h"
#include "src/Render/soft_raster.h"

// Render targets made ahead (native_view_premake_targets). Making a texture
// pass's target (gpu_view.cpp's TargetFor: a colour and a depth texture,
// each a committed resource) costs about a millisecond, and a camera cut
// that first shows a song's characters made 25 of them in one frame (23 to
// 26 ms of a 47 ms frame). RB3 gives each rendered texture (RndTex::Type bit
// 2) its surfaces in DxTex::SyncBitmap as it loads, seconds before the first
// pass draws into it: scene_capture.cpp's hook announces it there
// (AnnounceTexture), and the worker makes it between frames, a few at a time
// (GpuRenderer::Idle), where the frame that first draws into it finds it.
//
// A target made ahead is undrawn, so it draws and samples as one not yet
// made (transparent black, or its guest pixels); only absent targets are
// made, never one a pass already has (remaking that would forget what it
// drew). It's sized as TargetFor would from what the announcement says,
// without the frame: a soft-particle surface (soft_raster.h's SoftTarget)
// comes out at the game's size and its first pass remakes it, as before.
//
// A target with no depth (kTexTypeNoZ) clears its depth at every pass and
// keeps none (gpu_view.cpp's begin_rt), so those of one size share one depth
// texture (SharesDepth): half the textures made for them. A target that
// loads depth never gets a shared one; a target whose type changes is made
// again.

// Texture arrays made ahead (native_view_premake_arrays). A size class and
// format's first array (gpu_view.cpp's PlaceTexture; 4 MB of level 0, ~0.6
// ms) is made when the first texture of that class is drawn: a song's first
// frame made 19 (11.5 ms), its camera cuts a few more. SyncBitmap sees every
// texture as it loads, 0.1 to 0.2 s before the frame that first draws it (a
// song's 240 in the last 0.2 s of its loading screen), with its size and
// D3DFORMAT, whose low six bits are its Xenos format: the worker makes the
// class's first array if it has none, as PlaceTexture would. At 20th Century
// Boy's start that predicted 20 of the first frame's 21 arrays, and 2 it
// didn't use. A format guessed wrong (a texture kept RGBA for its fetch's
// swizzle) only makes an array nothing uses; one made ahead is kept unused
// for kArrayKeepSeconds, then goes as an empty array does.

namespace band3::render {

// a rendered DxTex as SyncBitmap left it (tex+0x4c, +0x50, +0x48, +0x64)
struct AnnouncedTarget {
    uint32_t tex_obj = 0;
    uint32_t width = 0, height = 0, tex_type = 0, num_mips = 0;
};

// Announced targets not yet made, oldest first: one per DxTex (a newer
// announcement replaces its place in line), at most kMax (the oldest go).
// Not thread-safe: scene_capture.cpp's queue holds a lock.
class AnnounceQueue {
 public:
    static constexpr size_t kMax = 256;
    void Announce(const AnnouncedTarget& t) {
        Forget(t.tex_obj);
        if (items_.size() >= kMax) items_.erase(items_.begin());
        items_.push_back(t);
    }
    // a DxTex gone (RndTex::~RndTex)
    void Forget(uint32_t tex_obj) {
        std::erase_if(items_, [&](const AnnouncedTarget& a) { return a.tex_obj == tex_obj; });
    }
    // appends them to `out` in order (its own announcements of the same
    // DxTex replaced) and empties the queue
    void TakeInto(AnnounceQueue& out) {
        for (const AnnouncedTarget& t : items_) out.Announce(t);
        items_.clear();
    }
    bool Empty() const { return items_.empty(); }
    size_t Size() const { return items_.size(); }
    const AnnouncedTarget& Front() const { return items_.front(); }
    void PopFront() { items_.erase(items_.begin()); }
    void Clear() { items_.clear(); }

 private:
    std::vector<AnnouncedTarget> items_;
};

// a loaded texture as SyncBitmap left it (tex+0x4c, +0x50; D3DFORMAT's low
// six bits, +0x74)
struct AnnouncedTexture {
    uint32_t width = 0, height = 0, xenos_format = 0;
    bool operator==(const AnnouncedTexture&) const = default;
};

// Loaded textures' sizes and formats not yet looked at, oldest first, each
// once (a song loads hundreds of a few dozen kinds), at most kMax (the
// oldest go). Not thread-safe, as AnnounceQueue.
class TextureAnnounceQueue {
 public:
    static constexpr size_t kMax = 256;
    void Announce(const AnnouncedTexture& t) {
        if (std::find(items_.begin(), items_.end(), t) != items_.end()) return;
        if (items_.size() >= kMax) items_.erase(items_.begin());
        items_.push_back(t);
    }
    void TakeInto(TextureAnnounceQueue& out) {
        for (const AnnouncedTexture& t : items_) out.Announce(t);
        items_.clear();
    }
    bool Empty() const { return items_.empty(); }
    size_t Size() const { return items_.size(); }
    const AnnouncedTexture& Front() const { return items_.front(); }
    void PopFront() { items_.erase(items_.begin()); }
    void Clear() { items_.clear(); }

 private:
    std::vector<AnnouncedTexture> items_;
};

// seconds an array made ahead is kept unused
inline constexpr double kArrayKeepSeconds = 10;

// Moves the textures announced since the last call into `into`. Any thread;
// scene_capture.cpp's.
void TakeAnnouncedTextures(TextureAnnounceQueue& into);

// Moves the targets announced since the last call into `into` (the worker's
// own queue). Any thread; scene_capture.cpp's.
void TakeAnnouncedTargets(AnnounceQueue& into);

// whether a rendered texture's target is made: RndTex::Type bit 2, and a
// size RB3 could draw into
inline bool AnnouncesTarget(uint32_t tex_type, uint32_t width, uint32_t height) {
    return IsPassTargetType(tex_type) && width && height && width <= 8192 && height <= 8192;
}
// and a loaded one's array
inline bool AnnouncesTexture(uint32_t tex_type, uint32_t width, uint32_t height) {
    return !IsPassTargetType(tex_type) && width && height && width <= 8192 && height <= 8192;
}

// How many targets the worker makes ahead between two frames
// (GpuRenderer::Idle): up to kPerIdle, and not once kIdleMs has gone; none
// while kMaxUnused it made are still unused (they're released as any target
// after 30 s; gpu_view.cpp's kMaxRts is 128)
struct PremakeBudget {
    static constexpr uint32_t kPerIdle = 4;
    static constexpr double kIdleMs = 4.0;
    static constexpr uint32_t kMaxUnused = 64;
};
inline bool PremakeMore(uint32_t made_now, double elapsed_ms, uint32_t unused) {
    return made_now < PremakeBudget::kPerIdle && elapsed_ms < PremakeBudget::kIdleMs &&
           unused < PremakeBudget::kMaxUnused;
}

// a target sharing its size's depth texture: one without depth (cleared at
// each pass, never kept), never a shadow map (its depth is its picture)
inline bool SharesDepth(uint32_t tex_type, bool shadow_map) {
    return (tex_type & kTexTypeNoZ) != 0 && !shadow_map;
}

// a target's mip levels, as FinishDrawTarget downsamples (to 1x1 at most);
// a shadow map's one (read by Load)
inline uint32_t TargetLevels(uint32_t num_mips, uint32_t w, uint32_t h, bool shadow_map) {
    if (shadow_map || num_mips <= 1) return 1;
    uint32_t chain = 1;
    for (uint32_t s = std::max(w, h); s > 1; s >>= 1) chain++;
    return std::min(num_mips, chain);
}

}  // namespace band3::render
