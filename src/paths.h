#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// band3's folder settings: where its folders are, and the folders it reads
// songs from. A relative path is relative to the anchor: the folder
// band3_config.ini is in, or the exe's folder without one, so a whole install
// can move as one folder.

namespace band3::paths {

// value (UTF-8) resolved against anchor; an empty value stays empty
std::filesystem::path Resolve(std::string_view value, const std::filesystem::path& anchor);

// the anchor for relative paths: ini's folder (absolute) when ini is a file,
// otherwise exe_folder
std::filesystem::path Anchor(const std::filesystem::path& ini,
                             const std::filesystem::path& exe_folder);

// the first dir / name that is a file, or empty
std::filesystem::path FindFile(const std::vector<std::filesystem::path>& dirs,
                               std::string_view name);

// "a| b ||c" -> {"a", "b", "c"}
std::vector<std::string> SplitList(std::string_view value);

// The path rule: which game data, user data and cache folders the game starts
// with. The SDK fixes its folders before band3.toml loads, so a folder saved
// there (or chosen in the launcher) is applied here, over the SDK's.

// where a folder setting's value came from, as far as the rule cares
enum class PathSource {
    kUnset,  // nothing set the setting (band3_config.ini may have, in the defaults)
    kSaved,  // band3.toml or the launcher
    kFixed,  // the command line or the environment, already in the defaults
};

struct PathSetting {
    std::string value;
    PathSource source = PathSource::kUnset;
};

struct Folders {
    std::filesystem::path game_data;
    std::filesystem::path user_data;
    std::filesystem::path cache;
};

struct PathRuleInputs {
    // the SDK's folders as band3's OnConfigurePaths left them: the command
    // line's and the ini's applied, and the cache following the user data
    Folders defaults;
    PathSetting game_data;
    PathSetting user_data;
    PathSetting cache;
    // band3_config.ini sets cache_root
    bool cache_in_ini = false;
    std::filesystem::path anchor;
};

// Each saved, non-empty setting replaces its default, resolved against the
// anchor. A replaced user data folder takes the cache with it unless a cache
// folder is set somewhere (an empty saved one isn't).
Folders ApplyPathRule(const PathRuleInputs& in);

}
