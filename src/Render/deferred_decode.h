#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

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
// With native_bc_textures on, while the GPU draws with it (gpu_view.cpp sets
// SetKeepBlocks), a block-compressed texture (DXT1, DXT2_3, DXT4_5, DXN) is
// decoded only as far as its blocks, untiled and swapped (Texture::blocks),
// which the GPU samples as they are: decoding them to RGBA took the worker
// 17-70 ms at a song's first frames, and four to eight times the bytes to
// send and keep. Its rgba and mips are decoded from the blocks the first time
// something asks for them (EnsureRgba), by the same decoder, to the same
// texels. With it off, as before: rgba and mips at once.

namespace band3::render {

// what DecodeDeferred has done, on any thread (CaptureProfile's deferred_*)
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
    });
}

// Everything deferred a frame draws with decoded, before it's handed on: its
// draws' textures and geometry, its shades' maps, its noise map and its
// motion blur objects' geometry. A pass carried in from an earlier frame or
// a composed frame's world is among its draws, and brings its own along,
// perhaps never decoded if no one took that frame. Idempotent, and cheap
// once done.
inline void DecodeDeferred(const FrameCapture& fc) {
    for (const DrawItem& d : fc.draws) {
        if (d.tex && d.tex->deferred) DecodeDeferred(*d.tex);
        if (d.geom && d.geom->deferred) DecodeDeferred(*d.geom);
    }
    for (const ShadeState& st : fc.shades)
        for (const auto& m : st.maps)
            if (m && m->deferred) DecodeDeferred(*m);
    if (fc.noise_map && fc.noise_map->deferred) DecodeDeferred(*fc.noise_map);
    for (const VelocityObject& o : fc.velocity_objects)
        if (o.geom && o.geom->deferred) DecodeDeferred(*o.geom);
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
