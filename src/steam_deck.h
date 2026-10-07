#pragma once
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Settings that suit a Steam Deck, applied at startup when band3 finds itself on
// one and steam_deck_defaults is on. Like band3's other defaults they only fill
// in what band3.toml, the environment and the command line leave unset, but they
// win over band3_config.ini, whose window settings are for a desktop.

namespace band3::steam_deck {

// steam_deck_env: the SteamDeck environment variable, which Steam sets to 1 on
// a Deck (Proton passes it through to the Windows build). sys_vendor and
// product_name: /sys/class/dmi/id's, empty off Linux; the LCD Deck is Jupiter
// and the OLED one Galileo.
inline bool LooksLikeSteamDeck(std::string_view steam_deck_env, std::string_view sys_vendor,
                               std::string_view product_name) {
    if (steam_deck_env == "1") return true;
    return sys_vendor == "Valve" && (product_name == "Jupiter" || product_name == "Galileo");
}

// whether this is a Steam Deck, going by the above
bool IsSteamDeck();

struct DeckDefault {
    const char* cvar;
    const char* value;
};

inline constexpr DeckDefault kPresets[] = {
    // Game Mode shows the game fullscreen anyway; this covers Desktop Mode
    {"fullscreen", "true"},
    // the 16:9 game on the Deck's 16:10 screen
    {"present_letterbox", "true"},
    // the console's 60, not the display's (the OLED Deck's 90), to save battery.
    // Capped rather than left to the vertical blank: with the cap off the game's
    // thread spins in guest D3D's fence poll (BlockOnFence) until each vertical
    // blank, a whole core; the cap's timer lets it sleep instead
    {"frame_cap", "60"},
    {"rnd_sync", "1"},
    // Steam's performance overlay does this job on a Deck
    {"debug_overlay", "false"},
};

// the value steam_deck_defaults gives `cvar` on a Deck, if it's one of the
// presets; ApplyDefaults and the launcher's defaults both read them here
inline std::optional<std::string> Preset(std::string_view cvar) {
    for (const auto& d : kPresets) {
        if (cvar == d.cvar) return d.value;
    }
    return std::nullopt;
}

// The cvars as ApplyPresets sees them: rex::cvar in the game.
struct PresetCvars {
    // band3.toml, the environment or the command line set it (its source
    // isn't the default)
    std::function<bool(std::string_view cvar)> set_elsewhere;
    // false when the cvar refuses the value
    std::function<bool(std::string_view cvar, std::string_view value)> set;
};

// Sets each preset that nothing else set, also where the cvar already has the
// preset's value: setting it is what records the preset as the value's source,
// and band3_config.ini fills in only cvars that nothing set, so without that
// the ini's [window] fullscreen = false would win over the Deck's fullscreen =
// true, which is the cvar's own default. Returns how many it set; a value a
// cvar refuses goes to `refused`.
inline int ApplyPresets(const PresetCvars& cvars,
                        const std::function<void(const DeckDefault&)>& refused = {}) {
    int applied = 0;
    for (const auto& d : kPresets) {
        if (cvars.set_elsewhere(d.cvar)) continue;
        if (!cvars.set(d.cvar, d.value)) {
            if (refused) refused(d);
            continue;
        }
        applied++;
    }
    return applied;
}

// Call after band3.toml, the environment and the command line are applied and
// before band3_config.ini is.
void ApplyDefaults();

}
