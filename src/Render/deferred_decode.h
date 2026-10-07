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
// Only the decoded fields (rgba and mips; verts and indices) are written, and
// the game's thread never reads those of an object with `deferred` set
// (scene_capture.cpp's HasPixels, HasFaces), so it may go on using the same
// object meanwhile.

namespace band3::render {

// what DecodeDeferred has done, on any thread (CaptureProfile's deferred_*)
struct DeferredDecodeCounts {
    std::atomic<uint64_t> decodes{0}, ns{0}, bytes{0};
};
inline DeferredDecodeCounts g_deferred_decode;

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
// decode, guest_formats.h's DecodeTextureLevels, from the same bytes); the
// copies let go of after
inline void DecodeDeferred(const Texture& t) {
    DeferredPixels& d = *t.deferred;
    std::call_once(d.once, [&] {
        const auto start = std::chrono::steady_clock::now();
        Texture decoded;
        uint64_t bytes = 0;
        if (guest_format::DecodeTextureLevels(d.bytes.data(),
                                              d.mips.empty() ? nullptr : d.mips.data(), d.fetch,
                                              decoded)) {
            auto& out = const_cast<Texture&>(t);
            out.rgba = std::move(decoded.rgba);
            out.mips = std::move(decoded.mips);
            bytes = out.rgba.size() * 4;
            for (const auto& level : out.mips) bytes += level.size() * 4;
        }
        d.bytes = {};
        d.mips = {};
        deferred_detail::Count(start, bytes);
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

}  // namespace band3::render
