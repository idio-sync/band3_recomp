#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Experimental (N7): emulated_gpu (Band3 -> Graphics) says whether band3 runs
// the emulated Xbox 360 GPU (the SDK's xenos plugin) at all. On, the default,
// it does, as before: the native renderer and the emulated GPU side by side,
// F8 switching between them (A/B). Off, it doesn't load the plugin: band3's
// sync-only GPU (sync_graphics_system.h) consumes the game's command ring for
// what the game waits on, and the native renderer is the only picture. It's
// decided once, as the runtime is configured (Band3App::OnPreSetup); a change
// applies at the next start. The rules here are pure, for the unit tests.

namespace band3::render::sync_gpu {

// emulated_gpu's value: true for on, false for off, nothing for anything else
inline std::optional<bool> ParseEmulatedGpu(std::string_view v) {
    if (v == "on") return true;
    if (v == "off") return false;
    return std::nullopt;
}

// What startup does with emulated_gpu and the SDK's gpu_plugin (what loads
// the emulated GPU, which band3 names "xenos" when nothing else does).
struct StartupGpuPlan {
    // emulated_gpu off: band3's sync-only GPU instead of the plugin
    bool native_only = false;
    // what to log about it, a line each
    std::vector<std::string> log;
};

// what startup logs when emulated_gpu is off on a build that can't present
// without the emulated GPU
inline constexpr char kNotPresentableHere[] =
    "emulated_gpu off isn't available on this platform yet; running with the emulated GPU";

// Anything but off (on, or a value the setting wouldn't take) keeps the
// emulated GPU, and the plugin; off drops a plugin named (the command line's
// --gpu_plugin included). `presentable` is whether this build has something
// to present with on its own (CanPresentNativeOnly, sync_graphics_system.h):
// without it, off is ignored and says so, since the SDK would stop band3 at
// startup otherwise.
inline StartupGpuPlan PlanStartupGpu(std::string_view emulated_gpu, std::string_view gpu_plugin,
                                     bool presentable) {
    StartupGpuPlan plan;
    const bool off = ParseEmulatedGpu(emulated_gpu) == std::optional<bool>(false);
    if (off && !presentable) {
        plan.log.push_back(kNotPresentableHere);
        return plan;
    }
    plan.native_only = off;
    if (!plan.native_only) return plan;
    plan.log.push_back(
        "emulated_gpu off: no emulated GPU this run (experimental); band3's sync-only GPU "
        "answers the game's command ring and the native renderer draws the window. F8 is "
        "inert and emulated_gpu_while_native is ignored");
    // named in band3.toml or on the command line: off wins
    if (!gpu_plugin.empty()) {
        plan.log.push_back("emulated_gpu off: gpu_plugin " + std::string(gpu_plugin) +
                           " isn't loaded");
    }
    return plan;
}

// Native-only, renderer is native whatever it was set to, once the launcher
// (which edits it) is done: the line to log when that changes it, or nothing
inline std::optional<std::string> ForceRendererNative(bool native_only,
                                                      std::string_view renderer) {
    if (!native_only || renderer == "native") return std::nullopt;
    return "emulated_gpu off: renderer was " + std::string(renderer) +
           ", native for this run";
}

// what F8 (bind_renderer) and a change of renderer log while there's no
// emulated GPU to switch to
inline constexpr char kNoEmulatedGpuSwitch[] =
    "the emulated GPU is off this run (emulated_gpu off); restart with it on to switch";

namespace detail {
inline std::atomic<bool> g_native_only{false};
}

// Set once, by Band3App::OnPreSetup, before the runtime, the game and the
// test server start: whether this run has no emulated GPU. Any thread reads it.
inline void SetNativeOnly(bool native_only) {
    detail::g_native_only.store(native_only, std::memory_order_release);
}
inline bool NativeOnly() { return detail::g_native_only.load(std::memory_order_acquire); }

// whether `setting` (emulated_gpu's value now, e.g. from the launcher) asks
// for another mode than this run's, which only a restart applies
inline bool EmulatedGpuChanged(std::string_view setting) {
    const std::optional<bool> on = ParseEmulatedGpu(setting);
    return on.has_value() && *on == NativeOnly();
}

}  // namespace band3::render::sync_gpu
