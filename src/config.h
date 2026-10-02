#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// band3_config.ini predates the cvars in settings.h. It is still read, but only
// fills in settings that band3.toml, the environment and the command line leave
// unset, so anything saved from the F4 menu takes precedence over it.

namespace band3 {

inline constexpr const char* kLegacyIniPath = "band3_config.ini";

// the ini found (working directory, then beside the exe), or kLegacyIniPath
// relative if neither has one; found once
const std::filesystem::path& LegacyIniPath();

// the folder the ini is in, absolute; the exe's folder if there is no ini.
// Every relative path band3 reads (the ini's, band3.toml's, the launcher's,
// content_folders) is relative to this
std::filesystem::path IniAnchor();

// [game] key from the ini, unquoted; empty if missing
std::string ReadIniString(const char* key);

// the ini's game data root, or "assets"; paths are fixed before the other
// settings load, so this one is read on its own
std::string ReadIniGameDataRoot();

// the value the ini gives `cvar`, as ApplyLegacyIni would set it (unquoted,
// inih's boolean spellings as true/false); nullopt where the ini leaves it
// unset, or has no key for it
std::optional<std::string> LegacyIniValue(std::string_view cvar);

// copies the ini's values onto the cvars nothing else has set
void ApplyLegacyIni();

// adds the game arguments the settings drive (-fast, -lang, -define MHX_PC)
// to GetArgs(), after the settings are loaded; calling it again replaces the
// ones the last call added, as the launcher's Play does
void AddSettingArgs();

// the game data root the runtime was started with
const std::filesystem::path& GameDataRoot();
void SetGameDataRoot(std::filesystem::path root);

const std::vector<std::string>& GetArgs();

}
