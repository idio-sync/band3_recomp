#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "src/renderer_default.h"

// renderer (Band3 -> Graphics) says what draws the game's picture:
// - native: band3's native renderer alone, with no emulated Xbox 360 GPU at
//   all. band3's sync-only GPU (sync_gpu/sync_graphics_system.h) answers what
//   the game waits on from its GPU. The default on Windows.
// - emulated: the emulated GPU (the SDK's xenos plugin) alone.
// - both, "Native + emulated (debug)": the two side by side, the native
//   picture shown, F8 switching to the emulated GPU's and back (A/B).
// Whether the emulated GPU runs is decided once, as the runtime is configured
// (Band3App::OnPreSetup), so a change between native and the other two
// applies at the next start; between emulated and both it applies at once.
// The rules here are pure, for the unit tests; renderer_switch.h runs them.
//
// It replaces two settings: renderer (native or emulated, both with the
// emulated GPU, F8 flipping it) and emulated_gpu (on, or off for native
// alone). MigrateEmulatedGpu reads a band3.toml or command line that still
// sets emulated_gpu.

namespace band3::render {

enum class RendererMode { kNative, kEmulated, kBoth };

inline std::optional<RendererMode> ParseRenderer(std::string_view v) {
    if (v == "native") return RendererMode::kNative;
    if (v == "emulated") return RendererMode::kEmulated;
    if (v == "both") return RendererMode::kBoth;
    return std::nullopt;
}

// the setting's value
inline const char* RendererName(RendererMode mode) {
    switch (mode) {
    case RendererMode::kNative: return "native";
    case RendererMode::kEmulated: return "emulated";
    case RendererMode::kBoth: return "both";
    }
    return "native";
}

// what the launcher calls it
inline const char* RendererLabel(RendererMode mode) {
    switch (mode) {
    case RendererMode::kNative: return "Native";
    case RendererMode::kEmulated: return "Emulated";
    case RendererMode::kBoth: return "Native + emulated (debug)";
    }
    return "Native";
}

// Migration

// where a setting's value came from, weakest first (rex::cvar::Source without
// the runtime's, which can't have set anything this early)
enum class SettingSource { kUnset, kConfig, kEnvironment, kCommandLine };

struct OldSetting {
    // the value now: the default when unset
    std::string value;
    SettingSource source = SettingSource::kUnset;
};

struct RendererMigration {
    // what renderer becomes, if emulated_gpu changes it
    std::optional<std::string> renderer;
    // where it counts as set from: emulated_gpu's
    SettingSource source = SettingSource::kUnset;
    // the line to log; empty when emulated_gpu isn't set
    std::string log;
};

namespace detail {
inline const char* SourceName(SettingSource source) {
    switch (source) {
    case SettingSource::kConfig: return "band3.toml";
    case SettingSource::kEnvironment: return "the environment";
    case SettingSource::kCommandLine: return "the command line";
    case SettingSource::kUnset: break;
    }
    return "its default";
}
}  // namespace detail

// emulated_gpu was read with renderer as a pair: off ran native alone,
// whatever renderer said; on ran the emulated GPU, renderer native showing
// the native picture over it (both, now) and emulated the emulated GPU's
// (emulated). So: off becomes native, on becomes both when renderer is native
// (or its default was) and emulated when it's emulated. The pair counts from
// where emulated_gpu was set: a renderer set somewhere stronger (the command
// line, over band3.toml's emulated_gpu) was written for the new setting and
// wins, emulated_gpu ignored. So does a renderer set to both, which no build
// with emulated_gpu wrote: it was migrated already (a band3.toml that kept a
// stale emulated_gpu beside it doesn't flip back to native at every start).
// A value emulated_gpu wouldn't take is unset.
inline RendererMigration MigrateEmulatedGpu(const OldSetting& emulated_gpu,
                                            const OldSetting& renderer) {
    RendererMigration m;
    const bool off = emulated_gpu.value == "off";
    if (emulated_gpu.source == SettingSource::kUnset || (!off && emulated_gpu.value != "on")) {
        return m;
    }
    const std::string old = "emulated_gpu = " + emulated_gpu.value + " (" +
                            detail::SourceName(emulated_gpu.source) + ")";
    const bool migrated = renderer.source != SettingSource::kUnset && renderer.value == "both";
    if (renderer.source > emulated_gpu.source || migrated) {
        m.log = old + " is retired and ignored: renderer = " + renderer.value + " (" +
                detail::SourceName(renderer.source) + ") wins";
        return m;
    }
    m.renderer = off ? "native" : renderer.value == "emulated" ? "emulated" : "both";
    m.source = emulated_gpu.source;
    m.log = old + " is retired: renderer = " + *m.renderer + " (it was " + renderer.value + ")";
    return m;
}

// Startup

struct StartupGpuPlan {
    // what this run is: native, or with the emulated GPU emulated or both
    RendererMode mode = RendererMode::kEmulated;
    // native: band3's sync-only GPU instead of the plugin
    bool native_only = false;
    // what to log about it, a line each
    std::vector<std::string> log;
};

// what startup logs when renderer is native on a build that can't present
// without the emulated GPU
inline constexpr char kNotPresentableHere[] =
    "renderer native (no emulated GPU) isn't available on this platform yet; running with the "
    "emulated GPU, as renderer emulated";

// `renderer` is the setting as startup reads it; a value it wouldn't take is
// the build's default. Native drops a plugin named (the command line's
// --gpu_plugin included). `presentable` is whether this build has something
// to present with on its own (CanPresentNativeOnly, sync_graphics_system.h):
// without it, native runs as emulated and says so, since the SDK would stop
// band3 at startup otherwise.
inline StartupGpuPlan PlanStartupGpu(std::string_view renderer, std::string_view gpu_plugin,
                                     bool presentable) {
    StartupGpuPlan plan;
    plan.mode = ParseRenderer(renderer).value_or(
        ParseRenderer(settings::kDefaultRenderer).value_or(RendererMode::kEmulated));
    switch (plan.mode) {
    case RendererMode::kNative:
        if (!presentable) {
            plan.mode = RendererMode::kEmulated;
            plan.log.push_back(kNotPresentableHere);
            return plan;
        }
        plan.native_only = true;
        plan.log.push_back(
            "renderer native: no emulated GPU this run; band3's sync-only GPU answers the game's "
            "command ring and the native renderer draws the window. F8 does nothing, and "
            "emulated_gpu_while_native doesn't apply");
        // named in band3.toml or on the command line: native wins
        if (!gpu_plugin.empty()) {
            plan.log.push_back("renderer native: gpu_plugin " + std::string(gpu_plugin) +
                               " isn't loaded");
        }
        break;
    case RendererMode::kEmulated:
        plan.log.push_back("renderer emulated: the emulated GPU draws the window");
        break;
    case RendererMode::kBoth:
        plan.log.push_back(
            "renderer both (debug): the native renderer and the emulated GPU both run, the "
            "native picture shown; F8 switches between them");
        break;
    }
    return plan;
}

// While band3 runs

struct RendererState {
    // no emulated GPU this run: fixed at startup
    bool native_only = false;
    // the build could run native alone (startup's `presentable`)
    bool presentable = true;
    // what runs now: native when native_only, emulated or both otherwise
    RendererMode live = RendererMode::kEmulated;
    // the window shows the native renderer's picture
    bool show_native = false;
};

// the run startup planned: native and both show the native picture first
inline RendererState StartState(const StartupGpuPlan& plan, bool presentable) {
    RendererState s;
    s.native_only = plan.native_only;
    s.presentable = presentable;
    s.live = plan.mode;
    s.show_native = plan.mode != RendererMode::kEmulated;
    return s;
}

struct RendererStep {
    RendererState next;
    // the line to log, if any
    std::optional<std::string> log;
};

// renderer set to `value` while band3 runs (F4, the launcher, the harness's
// `set`): emulated and both apply at once while the emulated GPU runs; native
// with it, or anything else without it, at the next start (native in a both
// run shows the native picture meanwhile). A value the setting wouldn't take
// changes nothing.
inline RendererStep OnRendererSetting(const RendererState& run, std::string_view value) {
    RendererStep step{run, std::nullopt};
    const std::optional<RendererMode> mode = ParseRenderer(value);
    if (!mode) return step;
    if (run.native_only) {
        if (*mode != RendererMode::kNative) {
            step.log = "renderer " + std::string(value) +
                       ": applies at the next start (this run has no emulated GPU)";
        }
        return step;
    }
    switch (*mode) {
    case RendererMode::kNative:
        step.log = run.presentable
                       ? "renderer native: applies at the next start (without the emulated GPU); "
                         "this run keeps it, as renderer " +
                             std::string(RendererName(run.live))
                       : "renderer native isn't available on this platform yet; this run keeps "
                         "the emulated GPU";
        if (run.live == RendererMode::kBoth) step.next.show_native = true;
        break;
    case RendererMode::kEmulated:
        step.next.live = RendererMode::kEmulated;
        step.next.show_native = false;
        break;
    case RendererMode::kBoth:
        step.next.live = RendererMode::kBoth;
        // the native picture, as at a start with both, unless it already shows
        if (run.live != RendererMode::kBoth) step.next.show_native = true;
        break;
    }
    return step;
}

// F8 (bind_renderer): the other picture in both; nothing otherwise
inline RendererStep OnSwitchKey(const RendererState& run) {
    RendererStep step{run, std::nullopt};
    if (run.live != RendererMode::kBoth) {
        step.log = std::string("F8: nothing to switch to with renderer = ") +
                   RendererName(run.live) +
                   "; only renderer = both (\"Native + emulated (debug)\") runs both pictures";
        return step;
    }
    step.next.show_native = !run.show_native;
    step.log = step.next.show_native ? "F8: the native renderer's picture"
                                     : "F8: the emulated GPU's picture";
    return step;
}

// The anisotropic filtering the native renderer samples textures with, in
// the emulated GPU's anisotropic_override encoding (-1 the game's own, 0 off,
// 1..5 1x..16x): native_anisotropic when it sets one, otherwise the emulated
// GPU's anisotropic_override where it exists (renderer emulated or both, so
// the two pictures, and the test captures compared with the emulated one,
// match), otherwise the game's own. A value out of range is the game's own.
inline int NativeAnisotropy(int native_setting, std::optional<int> emulated_override) {
    auto valid = [](int v) { return v >= -1 && v <= 5 ? v : -1; };
    if (valid(native_setting) >= 0) return native_setting;
    return emulated_override ? valid(*emulated_override) : -1;
}

// whether startup would pick another GPU for `value` than this run's: the
// launcher's Play restarts band3 for it. A value the setting wouldn't take
// doesn't.
inline bool RendererRestartNeeded(const RendererState& run, std::string_view value) {
    if (!ParseRenderer(value)) return false;
    return PlanStartupGpu(value, "", run.presentable).native_only != run.native_only;
}

}  // namespace band3::render
