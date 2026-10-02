#include "launcher_cvars.h"
#include <rex/cvar.h>
#include "src/config.h"
#include "src/steam_deck.h"

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

Lock LockFrom(rex::cvar::Source source) {
    switch (source) {
    case rex::cvar::Source::kCommandLine: return Lock::kCommandLine;
    case rex::cvar::Source::kEnvironment: return Lock::kEnvironment;
    default: return Lock::kNone;
    }
}

}

std::string RexCvarStore::Get(std::string_view name) const {
    return rex::cvar::GetFlagByName(name);
}

bool RexCvarStore::Set(std::string_view name, std::string_view value) {
    return rex::cvar::SetFlagByName(name, value);
}

Environment ReadEnvironment(std::span<const Setting> table, const PathDefaults& paths,
                            const std::filesystem::path& anchor) {
    Environment env;
    env.steam_deck = steam_deck::IsSteamDeck();
    env.anchor = anchor;
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
            facts.lock = LockFrom(rex::cvar::GetFlagSource(setting.cvar));
        }
        env.cvars[std::string(setting.cvar)] = std::move(facts);
    }

    // what Band3App::OnPostInitLogging sets at startup
    auto facts_of = [&env](std::string_view cvar) -> CvarFacts* {
        const auto it = env.cvars.find(cvar);
        return it == env.cvars.end() || !it->second.exists ? nullptr : &it->second;
    };
    // band3's shorter audio queue, unless something else set one
    if (auto* f = facts_of("audio_maxqframes")) f->startup_default = "3";
#ifndef _WIN32
    // xinput is Windows only
    if (auto* f = facts_of("input_backend")) f->startup_forced = "sdl";
#endif
    // the folder settings fall back to the folders the game would start with
    if (auto* f = facts_of("game_data_root")) f->path_default = paths.game_data_root;
    if (auto* f = facts_of("user_data_root")) f->path_default = paths.user_data_root;
    return env;
}

}
