#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The launcher hub's model, apart from drawing it: its menu, and the facts
// its status card reads cheaply before the game starts (Deluxe's version
// from the game data, the last song played from the score history, each
// controller's icon). Pure.

namespace band3::launcher::ui {

enum class HubItem { kFindGame, kPlay, kSettings, kMusicVideos, kTestControllers, kQuit };

struct HubEntry {
    HubItem item;
    // greyed: Play while the game can't be found
    bool enabled = true;
};

// the menu: Play first, or Find Rock Band 3 first (and Play greyed) when the
// game data folder doesn't hold the game
std::vector<HubEntry> HubMenu(bool game_found);
std::string_view HubItemName(HubItem item);

// the version Deluxe's dx/locale/gen/dx_version.dtb (decrypted) gives as
// rb3dx_version: "r1736" of "r1736+84a0b08"; nullopt without one
std::optional<std::string> DeluxeVersion(std::span<const uint8_t> dtb);

struct Played {
    std::string title;
    std::string artist;
};
// the song the score history's last whole line is of; `tail` is the end of
// score_history.jsonl, its first line possibly cut
std::optional<Played> LastPlayed(std::string_view tail);

// a cell of rb3_icons' 4x4 sheet: bass, guitar, drums, keys; the pro ones;
// the microphones
struct IconCell {
    int column = 0;
    int row = 0;
    bool operator==(const IconCell&) const = default;
};
inline constexpr IconCell kVocalsIcon{0, 2};
// an instrument's icon by the XInput subtype it reports; nullopt for a gamepad
std::optional<IconCell> IconFor(uint8_t sub_type);

}
