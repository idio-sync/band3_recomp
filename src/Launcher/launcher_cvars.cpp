#include "launcher_cvars.h"
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include "config_file.h"
#include "settings_reference.h"
#include "src/config.h"
#include "src/Hooks/frame_pacing.h"
#include "src/Render/renderer_switch.h"
#include "src/Render/sync_gpu/native_only.h"
#include "src/settings.h"
#include "src/steam_deck.h"
#include "src/Test/test_server.h"

namespace band3::launcher {

namespace {

std::optional<ValueType> TypeOf(rex::cvar::FlagType type) {
    using rex::cvar::FlagType;
    switch (type) {
    case FlagType::Boolean: return ValueType::kBool;
    case FlagType::Int32:
    case FlagType::Int64:
    case FlagType::Uint32:
    case FlagType::Uint64: return ValueType::kInt;
    case FlagType::Double: return ValueType::kFloat;
    case FlagType::String: return ValueType::kString;
    case FlagType::Command: break;
    }
    return std::nullopt;
}

Lifecycle LifecycleFrom(rex::cvar::Lifecycle lifecycle) {
    switch (lifecycle) {
    case rex::cvar::Lifecycle::kHotReload: return Lifecycle::kHotReload;
    case rex::cvar::Lifecycle::kRequiresRestart: return Lifecycle::kRequiresRestart;
    case rex::cvar::Lifecycle::kInitOnly: return Lifecycle::kInitOnly;
    }
    return Lifecycle::kHotReload;
}

// nullopt for a command
std::optional<RegistryCvar> AsRegistryCvar(const rex::cvar::FlagEntry& entry) {
    const auto type = TypeOf(entry.type);
    if (!type) return std::nullopt;
    return RegistryCvar{.name = entry.name,
                        .category = entry.category,
                        .type = *type,
                        .min = entry.constraints.min,
                        .max = entry.constraints.max,
                        .allowed = entry.constraints.allowed_values,
                        .description = entry.description,
                        .default_value = entry.default_value,
                        .lifecycle = LifecycleFrom(entry.lifecycle)};
}

// band3.toml has `key = true`
bool ConfigSaysTrue(const std::filesystem::path& file, std::string_view key) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const ParsedConfig parsed = ParseConfig(text);
    for (const ConfigLine& line : parsed.lines) {
        if (line.key != key) continue;
        const size_t equals = line.text.find('=');
        if (equals == std::string::npos) return false;
        std::string_view value = std::string_view(line.text).substr(equals + 1);
        while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        while (!value.empty() && value.back() == ' ') value.remove_suffix(1);
        return value == "true";
    }
    return false;
}

Lock LockFrom(rex::cvar::Source source) {
    switch (source) {
    case rex::cvar::Source::kCommandLine: return Lock::kCommandLine;
    case rex::cvar::Source::kEnvironment: return Lock::kEnvironment;
    default: return Lock::kNone;
    }
}

}

std::string RexCvarStore::Get(std::string_view name) const {
    if (name == "vsync") {
        if (const std::optional<bool> vsync = pacing::VsyncBeforeCap()) {
            return *vsync ? "true" : "false";
        }
    }
    return rex::cvar::GetFlagByName(name);
}

bool RexCvarStore::Set(std::string_view name, std::string_view value) {
    return rex::cvar::SetFlagByName(name, value);
}

std::vector<RegistryCvar> ReadRegistry() {
    std::vector<RegistryCvar> out;
    for (const rex::cvar::FlagEntry& entry : rex::cvar::GetRegistry()) {
        if (!entry.category.starts_with("Band3/")) continue;
        if (auto cvar = AsRegistryCvar(entry)) out.push_back(std::move(*cvar));
    }
    return out;
}

bool WriteSettingsReference(const std::filesystem::path& path) {
    // the emulated GPU's settings (vsync, resolution_scale, ...) are registered
    // as its plugin loads, which hasn't happened this early; loading it is
    // enough, nothing of it starts. Kept, never set up: band3 quits after this
    if (!rex::cvar::GetFlagInfo("vsync")) (void)rex::system::LoadGpuPlugin("xenos").release();

    std::vector<RegistryCvar> cvars = ReadRegistry();
    const GeneratedRows generated(cvars, SettingTable());
    const std::vector<Setting> page = JoinTables(SettingTable(), generated.Rows());
    auto add = [&cvars](std::string_view name) {
        if (std::ranges::find(cvars, name, &RegistryCvar::name) != cvars.end()) return;
        if (const auto* entry = rex::cvar::GetFlagInfo(name)) {
            if (auto cvar = AsRegistryCvar(*entry)) cvars.push_back(std::move(*cvar));
        }
    };
    for (const Setting& row : page) add(row.cvar);
    for (const LegacyIniKey& key : LegacyIniKeys()) add(key.cvar);
    for (RegistryCvar& cvar : cvars) {
        for (const auto& d : settings::kStartupDefaults) {
            if (cvar.name == d.cvar) cvar.default_value = d.value;
        }
        // F4 says a change to these waits for the next start, as for kRequiresRestart
        if (cvar.lifecycle == Lifecycle::kHotReload && settings::ReadAtStartupOnly(cvar.name)) {
            cvar.lifecycle = Lifecycle::kRequiresRestart;
        }
    }

#ifdef _WIN32
    constexpr std::string_view kPlatform = "Windows";
#else
    constexpr std::string_view kPlatform = "Linux";
#endif
    const std::string text = SettingsReference(
        {.page = page, .cvars = cvars, .ini_keys = LegacyIniKeys(), .platform = kPlatform});
    std::ofstream out(path, std::ios::binary);
    out << text;
    out.close();
    if (!out) {
        REXLOG_ERROR("Couldn't write the settings reference to {}", rex::path_to_utf8(path));
        return false;
    }
    REXLOG_INFO("Wrote the settings reference ({} settings) to {}", cvars.size(),
                rex::path_to_utf8(path));
    return true;
}

Environment ReadEnvironment(std::span<const Setting> table, const PathDefaults& paths,
                            const std::filesystem::path& anchor, Where where,
                            const std::filesystem::path& config_path) {
    Environment env;
    env.steam_deck = steam_deck::IsSteamDeck();
    env.anchor = anchor;
    env.in_game = where == Where::kInGame;
    // the GPU was chosen before the launcher showed (Band3App::OnPreSetup)
    env.emulated_gpu_running = !render::sync_gpu::NativeOnly();
    env.native_presentable = render::CurrentRenderer().presentable;
    for (const auto& setting : table) {
        CvarFacts facts;
        const auto* info = rex::cvar::GetFlagInfo(setting.cvar);
        const auto type = info ? TypeOf(info->type) : std::nullopt;
        if (type) {
            facts.exists = true;
            facts.type = *type;
            facts.description = info->description;
            facts.min = info->constraints.min;
            facts.max = info->constraints.max;
            facts.allowed = info->constraints.allowed_values;
            facts.registry_default = info->default_value;
            facts.deck_preset = steam_deck::Preset(setting.cvar);
            facts.ini = LegacyIniValue(setting.cvar);
            const rex::cvar::Source source = rex::cvar::GetFlagSource(setting.cvar);
            facts.lock = LockFrom(source);
            facts.from_config = source == rex::cvar::Source::kConfig;
            facts.lifecycle = LifecycleFrom(info->lifecycle);
            if (env.in_game) {
                facts.read_once = settings::ReadAtStartupOnly(setting.cvar);
                facts.started_with = settings::StartupValue(setting.cvar);
            }
        }
        env.cvars[std::string(setting.cvar)] = std::move(facts);
    }

    // what Band3App::OnPostInitLogging sets at startup
    auto facts_of = [&env](std::string_view cvar) -> CvarFacts* {
        const auto it = env.cvars.find(cvar);
        return it == env.cvars.end() || !it->second.exists ? nullptr : &it->second;
    };
    // band3's defaults for SDK settings (its audio queue, a window), unless something else set one
    for (const auto& d : settings::kStartupDefaults) {
        if (auto* f = facts_of(d.cvar)) f->startup_default = std::string(d.value);
    }
    // the test harness plays the virtual instrument as player 1 (test::Init)
    if (test::Enabled()) {
        if (auto* f = facts_of("virtual_instrument")) f->startup_forced = "true";
        if (auto* f = facts_of("virtual_instrument_player")) f->startup_forced = "1";
    }
#ifndef _WIN32
    // xinput is Windows only
    if (auto* f = facts_of("input_backend")) f->startup_forced = "sdl";
#endif
    // the folder settings fall back to the folders the game would start with
    if (auto* f = facts_of("game_data_root")) f->path_default = paths.game_data_root;
    if (auto* f = facts_of("user_data_root")) f->path_default = paths.user_data_root;
    // the launcher's save sets show_launcher at runtime, so in game only the
    // file says whether the box was ticked
    if (auto* f = facts_of("show_launcher"); f && env.in_game && f->lock == Lock::kNone) {
        f->from_config = ConfigSaysTrue(config_path, "show_launcher");
    }
    return env;
}

}
