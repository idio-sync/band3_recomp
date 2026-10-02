#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// When band3 has the game list its content again, for packages added to the
// content folders while it runs (live_content.h): only where the game does
// that itself, so nothing it's in the middle of changes under it. The main
// hub refreshes as it's entered, and the Music Library when told storage
// changed; anywhere else, the packages wait for one of the two.

namespace band3::content {

// the DTA that refreshes on this screen, as the screen's own script does;
// empty on any other
std::string RefreshScript(std::string_view screen);

// Whether to refresh now, each frame: once for each new lot of packages
// (generation, which rises as packages are added) on each safe screen it
// meets, until the game has listed them (listed: the generation the game's
// last listing had). Another try on the same screen would only make the
// Music Library rebuild its list again.
class RefreshPlanner {
public:
    // the script to run now, or empty
    std::string Next(uint64_t generation, uint64_t listed, std::string_view screen, bool in_game);

private:
    uint64_t tried_generation_ = 0;
    std::string tried_screen_;
};

}
