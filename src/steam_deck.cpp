#include "steam_deck.h"
#include <cstdlib>
#include <string>
#include <rex/cvar.h>
#include <rex/logging.h>
#include "settings.h"

#ifndef _WIN32
#include <fstream>
#endif

namespace band3::steam_deck {

namespace {

// scaling past the Deck's 1280x800 screen costs GPU time for detail it can't show
constexpr const char* kResolutionScaleCvars[] = {
    "resolution_scale",
    "draw_resolution_scale_x",
    "draw_resolution_scale_y",
};

#ifndef _WIN32
std::string ReadDmi(const char* name) {
    std::ifstream file(std::string("/sys/class/dmi/id/") + name);
    std::string value;
    std::getline(file, value);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) value.pop_back();
    return value;
}
#endif

}

bool IsSteamDeck() {
    const char* env = std::getenv("SteamDeck");
#ifdef _WIN32
    return LooksLikeSteamDeck(env ? env : "", "", "");
#else
    return LooksLikeSteamDeck(env ? env : "", ReadDmi("sys_vendor"), ReadDmi("product_name"));
#endif
}

void ApplyDefaults() {
    if (!REXCVAR_GET(steam_deck_defaults) || !IsSteamDeck()) return;

    const int applied = ApplyPresets(
        {
            .set_elsewhere =
                [](std::string_view cvar) {
                    return rex::cvar::GetFlagSource(cvar) != rex::cvar::Source::kDefault;
                },
            // a value the cvar already has is set too: SetFlagByName records
            // the source either way, which is what keeps the ini off it
            .set = [](std::string_view cvar,
                      std::string_view value) { return rex::cvar::SetFlagByName(cvar, value); },
        },
        [](const DeckDefault& d) {
            REXLOG_WARN("Steam Deck: couldn't set {} = {}", d.cvar, d.value);
        });
    // startup defaults are not changes waiting on a restart
    rex::cvar::ClearPendingRestartFlags();
    REXLOG_INFO("Steam Deck: applied {} defaults (steam_deck_defaults = false turns them off)",
                applied);

    for (const char* cvar : kResolutionScaleCvars) {
        const std::string value = rex::cvar::GetFlagByName(cvar);
        if (!value.empty() && std::atoi(value.c_str()) > 1) {
            REXLOG_WARN("Steam Deck: {} = {} costs a lot of GPU time for detail the Deck's "
                        "1280x800 screen can't show; 1 is the console's resolution",
                        cvar, value);
        }
    }
}

}
