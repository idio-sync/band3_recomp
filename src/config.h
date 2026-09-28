#pragma once
#include <string>
#include <vector>

// band3_config.ini predates the cvars in settings.h. It is still read, but only
// fills in settings that band3.toml, the environment and the command line leave
// unset, so anything saved from the F4 menu takes precedence over it.

namespace band3 {

inline constexpr const char* kLegacyIniPath = "band3_config.ini";

// the ini's game data root, or "assets"; paths are fixed before the other
// settings load, so this one is read on its own
std::string ReadIniGameDataRoot(const char* path = kLegacyIniPath);

// copies the ini's values onto the cvars nothing else has set
void ApplyLegacyIni(const char* path = kLegacyIniPath);

// adds the game arguments the settings drive (-fast, -lang) to GetArgs();
// call once, after the settings are loaded
void AddSettingArgs();

// the game data root the runtime was started with
const std::string& GameDataRoot();
void SetGameDataRoot(std::string root);

const std::vector<std::string>& GetArgs();

}
