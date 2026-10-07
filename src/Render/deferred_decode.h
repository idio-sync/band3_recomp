#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "src/Render/guest_formats.h"
#include "src/Render/scene_capture.h"

// What a capture's game-thread copies (Texture::deferred, Geometry::deferred)
// are decoded into, off the game's thread: at first sight of a song's or a
// shot's textures and meshes, decoding them where they were drawn cost the
// game's thread 33-69 ms a frame (22-47 MB of RGBA, 7-16 MB of geometry), so
// it copies their bytes as guest memory holds them (a few milliseconds) and
// whoever takes a capture first decodes them (scene_capture.h's
// LatestCapture, CaptureHeldFrame), as the same decoder would have from the
// same bytes. Apart from scene_capture.cpp so capture files (SaveCapture)
// and the unit tests reach it too.
//
// Each is decoded once, whichever thread asks first; the others wait for it.
// Only the decoded fields (rgba and mips, or blocks; verts and indices) are
// written, and the game's thread never reads those of an object with
// `deferred` set (scene_capture.cpp's HasPixels, HasFaces), so it may go on
// using the same object meanwhile.
//
// A frame with much to decode (a song's first frames, a camera cut's) is
// decoded on a few helper threads besides the one asking, biggest first
// (native_deferred_decode_threads, DecodeHelpers), as each texture or mesh is
// decoded once all the same.
//
// With native_bc_textures on, while the GPU draws with it (gpu_view.cpp sets
// SetKeepBlocks), a block-compressed texture (DXT1, DXT2_3, DXT4_5, DXN) is
// decoded only as far as its blocks, untiled and swapped (Texture::blocks),
// which the GPU samples as they are: decoding them to RGBA took the worker
// 17-70 ms at a song's first frames, and four to eight times the bytes to
// send and keep. Its rgba and mips are decoded from the blocks the first time
// something asks for them (EnsureRgba), by the same decoder, to the same
// texels. With it off, as before: rgba and mips at once.

namespace band3::render {

// what DecodeDeferred has done, on any thread (CaptureProfile's deferred_*);
// ns each decoding thread's time, summed: more than the time a frame waited
// for them when helpers decoded it (DecodeHelpers)
struct DeferredDecodeCounts {
    std::atomic<uint64_t> decodes{0}, ns{0}, bytes{0};
    // block-compressed textures kept as blocks, decoded to RGBA all the same
    // for their swizzle, and kept as blocks then decoded to RGBA (EnsureRgba)
    std::atomic<uint64_t> bc_blocks{0}, bc_swizzled{0}, bc_rgba{0};
};
inline DeferredDecodeCounts g_deferred_decode;

// whether DecodeDeferred keeps block-compressed textures as blocks: what the
// GPU drawing them last asked for (native_bc_textures, and formats it has);
// off until it has drawn a frame, and for the CPU's rasterizer
inline std::atomic<bool> g_keep_blocks{false};
inline void SetKeepBlocks(bool keep) { g_keep_blocks.store(keep, std::memory_order_relaxed); }

namespace deferred_detail {
inline void Count(std::chrono::steady_clock::time_point start, uint64_t bytes) {
    g_deferred_decode.decodes.fetch_add(1, std::memory_order_relaxed);
    g_deferred_decode.ns.fetch_add(
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count()),
        std::memory_order_relaxed);
    g_deferred_decode.bytes.fetch_add(bytes, std::memory_order_relaxed);
}
}  // namespace deferred_detail

// a deferred texture's rgba and mips from the bytes copied (DecodeTexture's
// decode, guest_formats.h's DecodeTextureLevels, from the same bytes), or its
// blocks (g_keep_blocks: DecodeTextureBlocks, the GPU's to sample); the
// copies let go of after
inline void DecodeDeferred(const Texture& t) {
    DeferredPixels& d = *t.deferred;
    std::call_once(d.once, [&] {
        const auto start = std::chrono::steady_clock::now();
        const uint8_t* mips = d.mips.empty() ? nullptr : d.mips.data();
        auto& out = const_cast<Texture&>(t);
        Texture decoded;
        uint64_t bytes = 0;
        const guest_format::FetchLayout l = guest_format::ReadFetchLayout(d.fetch);
        const bool bc = g_keep_blocks.load(std::memory_order_relaxed) &&
                        guest_format::IsBlockCompressed(l.format);
        if (bc && guest_format::DecodeTextureBlocks(d.bytes.data(), mips, d.fetch, decoded)) {
            out.blocks = std::move(decoded.blocks);
            bytes = out.blocks->level0.size();
            for (const auto& level : out.blocks->mips) bytes += level.size();
            g_deferred_decode.bc_blocks.fetch_add(1, std::memory_order_relaxed);
        } else if (guest_format::DecodeTextureLevels(d.bytes.data(), mips, d.fetch, decoded)) {
            out.rgba = std::move(decoded.rgba);
            out.mips = std::move(decoded.mips);
            bytes = out.rgba.size() * 4;
            for (const auto& level : out.mips) bytes += level.size() * 4;
            if (bc && l.swizzle != guest_format::kIdentitySwizzle)
                g_deferred_decode.bc_swizzled.fetch_add(1, std::memory_order_relaxed);
        }
        d.bytes = {};
        d.mips = {};
        deferred_detail::Count(start, bytes);
        d.done.store(true, std::memory_order_release);
    });
}

// A texture's rgba and mips, decoded if they're not yet: a deferred one's
// (DecodeDeferred), and a block-compressed one kept as blocks, from them
// (guest_formats.h's DecodeRgbaFromBlocks: DecodeTextureLevels' texels). What
// reads a texture's rgba calls it first, but for the game's thread on one
// it hasn't captured deferred; any thread, once each, the others waiting.
inline void EnsureRgba(const Texture& t) {
    if (t.deferred) DecodeDeferred(t);
    if (!t.blocks) return;
    BlockPixels& b = *t.blocks;
    std::call_once(b.rgba_once, [&] {
        auto& out = const_cast<Texture&>(t);
        guest_format::DecodeRgbaFromBlocks(b, t.width, t.height, out.rgba, out.mips);
        g_deferred_decode.bc_rgba.fetch_add(1, std::memory_order_relaxed);
    });
}

// a deferred mesh's verts and indices from the buffers' bytes copied
// (guest_formats.h's DecodeGeometryBytes); the copies let go of after
inline void DecodeDeferred(const Geometry& g) {
    DeferredGeometry& d = *g.deferred;
    std::call_once(d.once, [&] {
        const auto start = std::chrono::steady_clock::now();
        Geometry decoded;
        guest_format::DecodeGeometryBytes(d.vb.data(), d.num_verts, d.ib.data(), d.num_indices,
                                          decoded);
        auto& out = const_cast<Geometry&>(g);
        out.verts = std::move(decoded.verts);
        out.indices = std::move(decoded.indices);
        d.vb = {};
        d.ib = {};
        deferred_detail::Count(start, out.verts.size() * sizeof(Vertex) +
                                          out.indices.size() * sizeof(uint16_t));
        d.done.store(true, std::memory_order_release);
    });
}

// One of a frame's deferred textures or meshes not decoded yet
// (GatherPending), and the bytes the game's thread copied of it, what decoding
// it costs, roughly: a texture's levels (guest_formats.h's BaseLevelBytes and
// MipChainBytes, from its fetch constant), a mesh's buffers. Weighed from
// what doesn't change, not the copies, which a decode on another thread may
// be letting go of meanwhile.
struct PendingDecode {
    const Texture* tex = nullptr;
    const Geometry* geom = nullptr;
    uint64_t bytes = 0;
};

// A frame's textures and meshes DecodeDeferred has yet to decode: its draws',
// its shades' maps, its noise map and its motion blur objects' geometry, each
// once (a texture is drawn and shaded with many times), biggest first, so
// that decoding them on several threads (DecodeHelpers) doesn't leave the
// biggest to start last. None once they're done, the frames between a
// song's or a shot's first.
inline std::vector<PendingDecode> GatherPending(const FrameCapture& fc) {
    std::vector<PendingDecode> out;
    const auto tex = [&](const Texture* t) {
        if (!t || !t->deferred || t->deferred->done.load(std::memory_order_acquire)) return;
        const uint32_t* f = t->deferred->fetch;
        out.push_back({t, nullptr,
                       uint64_t(guest_format::BaseLevelBytes(f)) + guest_format::MipChainBytes(f)});
    };
    const auto geom = [&](const Geometry* g) {
        if (!g || !g->deferred || g->deferred->done.load(std::memory_order_acquire)) return;
        const DeferredGeometry& d = *g->deferred;
        out.push_back({nullptr, g,
                       uint64_t(d.num_verts) * guest_format::kPackedVertSize +
                           uint64_t(d.num_indices) * 2});
    };
    for (const DrawItem& d : fc.draws) {
        tex(d.tex.get());
        geom(d.geom.get());
    }
    for (const ShadeState& st : fc.shades)
        for (const auto& m : st.maps) tex(m.get());
    tex(fc.noise_map.get());
    for (const VelocityObject& o : fc.velocity_objects) geom(o.geom.get());
    if (out.size() < 2) return out;
    const auto key = [](const PendingDecode& p) -> const void* {
        return p.tex ? static_cast<const void*>(p.tex) : static_cast<const void*>(p.geom);
    };
    const std::less<const void*> before;
    std::sort(out.begin(), out.end(),
              [&](const PendingDecode& a, const PendingDecode& b) { return before(key(a), key(b)); });
    out.erase(std::unique(out.begin(), out.end(),
                          [&](const PendingDecode& a, const PendingDecode& b) {
                              return key(a) == key(b);
                          }),
              out.end());
    std::stable_sort(out.begin(), out.end(), [](const PendingDecode& a, const PendingDecode& b) {
        return a.bytes > b.bytes;
    });
    return out;
}

// Under this many bytes in all, a frame's pending decodes stay on the thread
// that asks (DecodeHelpers): some 5 ms of decoding at a song's first frames'
// rate (0.7 MB a millisecond), where they have 5 to 15 MB. A movie's planes,
// decoded again each of its frames (1.4 MB of them in the menus), stay
// there: split, they'd save half a millisecond a frame for waking the
// helpers 30 times a second.
inline constexpr uint64_t kParallelDecodeBytes = 4u << 20;

// How many helper threads decode `pending` with the thread that asks: none
// for `threads` 0, a single one or fewer than kParallelDecodeBytes in all,
// else `threads`, but no more than there are others to decode
inline unsigned DecodeHelpers(const std::vector<PendingDecode>& pending, unsigned threads) {
    if (threads == 0 || pending.size() < 2) return 0;
    uint64_t bytes = 0;
    for (const PendingDecode& p : pending) bytes += p.bytes;
    if (bytes < kParallelDecodeBytes) return 0;
    return unsigned(std::min<size_t>(threads, pending.size() - 1));
}

// how many threads help DecodeDeferred(FrameCapture) decode a frame's
// pending textures and meshes (native_deferred_decode_threads); 0, inline
inline std::atomic<unsigned> g_decode_threads{0};
inline void SetDecodeThreads(unsigned threads) {
    g_decode_threads.store(threads, std::memory_order_relaxed);
}
// called on each helper as it starts, with its index (band3 names it)
inline std::atomic<void (*)(unsigned)> g_decode_thread_started{nullptr};

namespace deferred_detail {

// The helpers, started as first wanted and kept for the next frame that
// wants them, asleep on a condition variable meanwhile (a song's first frames
// and its camera cuts each want them for a few milliseconds). Never
// destroyed: they sleep through the process's exit.
class DecodePool {
public:
    // `work` on this thread and on `helpers` of the pool's at once, each
    // taking what's left until nothing is; back once all that took it have
    // finished. One caller at a time: another (the harness's `capture` while
    // the worker decodes) does its work alone.
    void Run(const std::function<void()>& work, unsigned helpers) {
        std::unique_lock run(run_mutex_, std::try_to_lock);
        if (!run.owns_lock()) {
            work();
            return;
        }
        {
            std::lock_guard lock(mutex_);
            while (threads_.size() < helpers) {
                const unsigned index = unsigned(threads_.size());
                threads_.emplace_back([this, index] { Loop(index); });
            }
            work_ = &work;
            wanted_ = helpers;
        }
        wake_.notify_all();
        // a helper not woken yet by the time this thread has run out finds
        // nothing to take, so isn't waited for
        const auto join = [&] {
            std::unique_lock lock(mutex_);
            wanted_ = 0;
            idle_.wait(lock, [&] { return busy_ == 0; });
            work_ = nullptr;
        };
        try {
            work();
        } catch (...) {
            join();
            throw;
        }
        join();
    }

private:
    void Loop(unsigned index) {
        if (const auto started = g_decode_thread_started.load()) started(index);
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return wanted_ > 0; });
            wanted_--;
            busy_++;
            const std::function<void()>& work = *work_;
            lock.unlock();
            work();
            lock.lock();
            if (--busy_ == 0) idle_.notify_all();
        }
    }

    std::mutex run_mutex_, mutex_;
    std::condition_variable wake_, idle_;
    std::vector<std::thread> threads_;
    const std::function<void()>* work_ = nullptr;
    unsigned wanted_ = 0, busy_ = 0;
};

inline DecodePool& Pool() {
    static DecodePool* pool = new DecodePool;
    return *pool;
}

}  // namespace deferred_detail

// Everything deferred a frame draws with decoded, before it's handed on
// (GatherPending's). A pass carried in from an earlier frame or a composed
// frame's world is among its draws, and brings its own along, perhaps never
// decoded if no one took that frame. With g_decode_threads, and enough to
// decode (DecodeHelpers), on that many helpers too, each taking the biggest
// left: a song's first frames and its camera cuts decoded 10-24 ms of it on
// the worker alone. Each is still decoded once (DecodeDeferred's call_once),
// and counted once, its ns the decoding thread's. Idempotent, and cheap once
// done.
inline void DecodeDeferred(const FrameCapture& fc) {
    const std::vector<PendingDecode> pending = GatherPending(fc);
    if (pending.empty()) return;
    std::atomic<size_t> next{0};
    const std::function<void()> work = [&] {
        for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < pending.size();) {
            if (pending[i].tex)
                DecodeDeferred(*pending[i].tex);
            else
                DecodeDeferred(*pending[i].geom);
        }
    };
    const unsigned helpers =
        DecodeHelpers(pending, g_decode_threads.load(std::memory_order_relaxed));
    if (helpers == 0)
        work();
    else
        deferred_detail::Pool().Run(work, helpers);
}

// DecodeDeferred's, and every texture's rgba and mips there too (EnsureRgba):
// for what reads them all, a capture file
inline void EnsureRgba(const FrameCapture& fc) {
    DecodeDeferred(fc);
    for (const DrawItem& d : fc.draws)
        if (d.tex) EnsureRgba(*d.tex);
    for (const ShadeState& st : fc.shades)
        for (const auto& m : st.maps)
            if (m) EnsureRgba(*m);
    if (fc.noise_map) EnsureRgba(*fc.noise_map);
}

}  // namespace band3::render
