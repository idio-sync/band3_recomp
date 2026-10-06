#pragma once
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include "launcher_settings.h"

// The settings model's view of the real cvars (the game's build only; the
// model itself is SDK-free and tested on its own).

namespace band3::launcher {

// GetFlagByName and SetFlagByName
class RexCvarStore final : public CvarStore {
public:
    std::string Get(std::string_view name) const override;
    bool Set(std::string_view name, std::string_view value) override;
};

// The folders a folder setting falls back to when it's empty: OnFinalizePaths'
// defaults.
struct PathDefaults {
    std::filesystem::path game_data_root;
    std::filesystem::path user_data_root;
};

// What the model needs to know about the table's cvars: types, descriptions,
// limits and lifecycles from the registry, the default layers (band3's startup
// values, the Steam Deck presets, band3_config.ini), and where each value came
// from (locked, or from band3.toml).
// Call it when the launcher opens, before anything is set, so a setting from
// the command line or the environment is recorded as locked. In game (F4) it's
// read when the settings first open, with what the game started with
// (settings::StartupValue) and what it reads only then
// (settings::ReadAtStartupOnly); `config_path` is band3.toml, which says
// whether the startup box was ticked (in game the cvar's source may be the
// launcher's save rather than the file).
Environment ReadEnvironment(std::span<const Setting> table, const PathDefaults& paths,
                            const std::filesystem::path& anchor, Where where,
                            const std::filesystem::path& config_path = {});

}
