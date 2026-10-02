#pragma once
#include <filesystem>
#include <string>
#include <vector>

// band3_config.ini predates the cvars in settings.h. It is still read, but only
// fills in settings that band3.toml, the environment and the command line leave
// unset, so anything saved from the F4 menu takes precedence over it.

namespace band3 {

inline constexpr const char* kLegacyIniPath = "band3_config.ini";

// the ini found (working directory, then beside the exe), or kLegacyIniPath
// relative if neither has one; found once
const std::filesystem::path& LegacyIniPath();

// the folder the ini is in, absolute; the working directory if there is no ini.
// A relative path in the ini is relative to this
std::filesystem::path IniAnchor();

// [game] key from the ini, unquoted; empty if missing
std::string ReadIniString(const char* key);

// the ini's game data root, or "assets"; paths are fixed before the other
// settings load, so this one is read on its own
std::string ReadIniGameDataRoot();

// copies the ini's values onto the cvars nothing else has set
void ApplyLegacyIni();

// adds the game arguments the settings drive (-fast, -lang) to GetArgs();
// call once, after the settings are loaded
void AddSettingArgs();

// the game data root the runtime was started with
const std::filesystem::path& GameDataRoot();
void SetGameDataRoot(std::filesystem::path root);

const std::vector<std::string>& GetArgs();

}
