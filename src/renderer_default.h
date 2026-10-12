#pragma once

// The renderer setting's default (settings.cpp), chosen when band3 is built,
// not at startup: the launcher saves only the settings that differ from
// their registry default, so a default changed at startup would be written
// into every player's band3.toml as if they had picked it. Native on
// Windows, where it has been played and checked against the game's picture;
// emulated elsewhere. On Linux the native renderer has run on a Steam Deck
// (Vulkan on RADV), whose presets give it native (steam_deck.h); other
// drivers haven't run it yet. A header of its own so the unit tests, which
// don't build the SDK, can check it.

namespace band3::settings {

#ifdef _WIN32
inline constexpr char kDefaultRenderer[] = "native";
#else
inline constexpr char kDefaultRenderer[] = "emulated";
#endif

}
