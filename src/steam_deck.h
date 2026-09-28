#pragma once
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

// Call after band3.toml, the environment and the command line are applied and
// before band3_config.ini is.
void ApplyDefaults();

}
