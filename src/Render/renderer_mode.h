#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "src/renderer_default.h"

// The renderer setting:
// - native: the native renderer alone, no emulated GPU; the sync-only GPU
//   (sync_gpu/sync_graphics_system.h) answers what the game waits on.
//   Default on Windows.
// - emulated: the emulated GPU (the SDK's xenos plugin) alone.
// - both: side by side, native picture shown, F8 toggles (A/B).
// Whether the emulated GPU runs is fixed at Band3App::OnPreSetup, so switching
// to or from native needs a restart; emulated <-> both applies at once.
// Pure rules for the unit tests; renderer_switch.h runs them.
// MigrateEmulatedGpu handles the retired emulated_gpu setting.

namespace band3::render {

enum class RendererMode { kNative, kEmulated, kBoth };

inline std::optional<RendererMode> ParseRenderer(std::string_view v) {
    if (v == "native") return RendererMode::kNative;
    if (v == "emulated") return RendererMode::kEmulated;
    if (v == "both") return RendererMode::kBoth;
    return std::nullopt;
}

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

// weakest first (rex::cvar::Source minus the runtime's, which can't have set
// anything this early)
enum class SettingSource { kUnset, kConfig, kEnvironment, kCommandLine };

struct OldSetting {
    // the default when unset
    std::string value;
    SettingSource source = SettingSource::kUnset;
};

struct RendererMigration {
    std::optional<std::string> renderer;
    // emulated_gpu's
    SettingSource source = SettingSource::kUnset;
    // empty when emulated_gpu isn't set
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

// emulated_gpu off -> native; on -> both, or emulated if renderer is
// emulated. A renderer set from a stronger source, or set to both (already
// migrated; a stale emulated_gpu mustn't flip it back), wins and
// emulated_gpu is ignored. An invalid emulated_gpu counts as unset.
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
    RendererMode mode = RendererMode::kEmulated;
    // band3's sync-only GPU instead of the plugin
    bool native_only = false;
    std::vector<std::string> log;
};

inline constexpr char kNotPresentableHere[] =
    "renderer native (no emulated GPU) isn't available on this platform yet; running with the "
    "emulated GPU, as renderer emulated";

// An invalid `renderer` is the build's default. Native ignores gpu_plugin.
// `presentable` (CanPresentNativeOnly): without it native falls back to
// emulated, since the SDK would stop band3 at startup otherwise.
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
    // fixed at startup
    bool native_only = false;
    bool presentable = true;
    RendererMode live = RendererMode::kEmulated;
    bool show_native = false;
};

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
    std::optional<std::string> log;
};

// renderer changed at runtime (F4, launcher, harness `set`): emulated and
// both apply at once while the emulated GPU runs; a change to or from native
// waits for the next start (native in a both run shows the native picture
// meanwhile). Invalid values change nothing.
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

// In anisotropic_override's encoding (-1 game's own, 0 off, 1..5 = 1x..16x;
// out of range = -1): native_anisotropic if set, else the emulated GPU's
// anisotropic_override when it runs (so both pictures match), else -1.
inline int NativeAnisotropy(int native_setting, std::optional<int> emulated_override) {
    auto valid = [](int v) { return v >= -1 && v <= 5 ? v : -1; };
    if (valid(native_setting) >= 0) return native_setting;
    return emulated_override ? valid(*emulated_override) : -1;
}

// whether `value` would pick a different GPU at startup (the launcher's Play
// restarts for it)
inline bool RendererRestartNeeded(const RendererState& run, std::string_view value) {
    if (!ParseRenderer(value)) return false;
    return PlanStartupGpu(value, "", run.presentable).native_only != run.native_only;
}

}  // namespace band3::render
