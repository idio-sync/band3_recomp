#pragma once

// The renderer setting's default (settings.cpp), chosen when band3 is built,
// not at startup: the launcher saves only the settings that differ from
// their registry default, so a default changed at startup would be written
// into every player's band3.toml as if they had picked it. Native on
// Windows, where it has been played and checked against the game's picture;
// emulated elsewhere until the native renderer has been run there (it builds
// on Linux, but nothing of it has run on Linux yet). A header of its own so
// the unit tests, which don't build the SDK, can check it.

namespace band3::settings {

#ifdef _WIN32
inline constexpr char kDefaultRenderer[] = "native";
#else
inline constexpr char kDefaultRenderer[] = "emulated";
#endif

}
