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

// Decodes a capture's game-thread copies (Texture::deferred,
// Geometry::deferred) off the game's thread: decoding a song's or shot's new
// textures and meshes where drawn cost it 33-69 ms a frame, so it copies the
// raw guest bytes and whoever takes the capture first decodes them
// (LatestCapture, CaptureHeldFrame). Kept out of scene_capture.cpp for
// SaveCapture and the unit tests.
//
// Each is decoded once, whichever thread asks first; the others wait. Only
// the decoded fields are written, and the game's thread never reads those of
// an object with `deferred` set (scene_capture.cpp's HasPixels, HasFaces), so
// it may keep using the object meanwhile.
//
// With native_bc_textures on and the GPU drawing (SetKeepBlocks), a
// block-compressed texture is decoded only to untiled, swapped blocks
// (Texture::blocks) for the GPU to sample; rgba and mips come from the blocks
// on first request (EnsureRgba), to the same texels.

namespace band3::render {

// CaptureProfile's deferred_*; ns sums every decoding thread's time, so
// exceeds the wait when helpers ran
struct DeferredDecodeCounts {
    std::atomic<uint64_t> decodes{0}, ns{0}, bytes{0};
    // BC textures kept as blocks, decoded to RGBA anyway for their swizzle,
    // and kept as blocks then decoded to RGBA (EnsureRgba)
    std::atomic<uint64_t> bc_blocks{0}, bc_swizzled{0}, bc_rgba{0};
};
inline DeferredDecodeCounts g_deferred_decode;

// whether DecodeDeferred keeps BC textures as blocks, as the GPU last asked;
// off until it has drawn a frame, and for the CPU rasterizer
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

// rgba and mips, or blocks with g_keep_blocks; frees the copies after
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

// Decodes a texture's rgba and mips if not yet, from its deferred copy or its
// blocks. Call before reading rgba (except the game's thread on a texture it
// didn't capture deferred); once per texture, other threads waiting.
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

// frees the copies after
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

// `bytes` approximates the decode cost. It's computed from the fetch constant
// or counts, not the copies, which another thread's decode may be freeing.
struct PendingDecode {
    const Texture* tex = nullptr;
    const Geometry* geom = nullptr;
    uint64_t bytes = 0;
};

// A frame's undecoded textures and meshes, each once, biggest first so the
// biggest doesn't start last on the helpers
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

// Below this (~5 ms of decoding), a frame decodes on the asking thread alone;
// it keeps a movie's per-frame planes (~1.4 MB) from waking the helpers 30
// times a second
inline constexpr uint64_t kParallelDecodeBytes = 4u << 20;

// helpers to use besides the asking thread, at most one per remaining item
inline unsigned DecodeHelpers(const std::vector<PendingDecode>& pending, unsigned threads) {
    if (threads == 0 || pending.size() < 2) return 0;
    uint64_t bytes = 0;
    for (const PendingDecode& p : pending) bytes += p.bytes;
    if (bytes < kParallelDecodeBytes) return 0;
    return unsigned(std::min<size_t>(threads, pending.size() - 1));
}

// native_deferred_decode_threads; 0 decodes inline
inline std::atomic<unsigned> g_decode_threads{0};
inline void SetDecodeThreads(unsigned threads) {
    g_decode_threads.store(threads, std::memory_order_relaxed);
}
// called on each helper as it starts, with its index (band3 names it)
inline std::atomic<void (*)(unsigned)> g_decode_thread_started{nullptr};

namespace deferred_detail {

// Helpers started on demand and kept asleep between uses. Never destroyed:
// they sleep through the process's exit.
class DecodePool {
public:
    // Runs `work` on this thread and `helpers` others; returns once all that
    // took it have finished. One caller at a time: a concurrent caller (the
    // harness's `capture` during the worker's decode) runs its work alone.
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

// Decodes everything deferred a frame draws with, including carried-in passes
// and a composed frame's world, whose earlier frame may never have been
// taken. Idempotent, and cheap once done.
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

// DecodeDeferred plus every texture's rgba, for a capture file
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
