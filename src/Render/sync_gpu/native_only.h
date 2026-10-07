#pragma once

#include <atomic>

// Whether this run has no emulated Xbox 360 GPU (renderer native,
// src/Render/renderer_mode.h). Then band3's sync-only GPU
// (sync_graphics_system.h) consumes the game's command ring for what the game
// waits on, and the native renderer is the only picture.

namespace band3::render::sync_gpu {

namespace detail {
inline std::atomic<bool> g_native_only{false};
}

// Set once, by Band3App::OnPreSetup, before the runtime, the game and the
// test server start. Any thread reads it.
inline void SetNativeOnly(bool native_only) {
    detail::g_native_only.store(native_only, std::memory_order_release);
}
inline bool NativeOnly() { return detail::g_native_only.load(std::memory_order_acquire); }

}  // namespace band3::render::sync_gpu
