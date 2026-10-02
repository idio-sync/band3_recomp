// Checks band3's folder settings (src/paths.cpp): how a configured path is
// resolved against the anchor, the anchor itself, finding the ini, folder
// lists, and the path rule that picks the folders the game starts with.

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include "src/paths.h"

namespace fs = std::filesystem;
using namespace band3::paths;

TEST_CASE("a relative path is relative to the anchor") {
    const fs::path anchor = fs::temp_directory_path() / "band3";
    CHECK(Resolve("user_data", anchor) == (anchor / "user_data").lexically_normal());
    CHECK(Resolve("../shared/songs", anchor) == (anchor / ".." / "shared" / "songs").lexically_normal());
}

TEST_CASE("an absolute path is kept and an empty one stays empty") {
    const fs::path anchor = fs::temp_directory_path() / "band3";
    const fs::path absolute = fs::temp_directory_path() / "rb" / "songs";
    CHECK(Resolve(absolute.string(), anchor) == absolute);
    CHECK(Resolve("//BISHOP/Downloads/rb", anchor) == fs::path("//BISHOP/Downloads/rb"));
#ifdef _WIN32
    CHECK(Resolve("D:/rb/songs", anchor) == fs::path("D:/rb/songs"));
#else
    CHECK(Resolve("/srv/rb/songs", anchor) == fs::path("/srv/rb/songs"));
#endif
    CHECK(Resolve("", anchor).empty());
}

TEST_CASE("the first folder holding the file wins") {
    const fs::path root = fs::temp_directory_path() / "band3_paths_test";
    fs::remove_all(root);
    fs::create_directories(root / "cwd");
    fs::create_directories(root / "exe");
    std::ofstream(root / "exe" / "band3_config.ini") << "[game]\n";
    CHECK(FindFile({root / "cwd", root / "exe"}, "band3_config.ini") == root / "exe" / "band3_config.ini");
    std::ofstream(root / "cwd" / "band3_config.ini") << "[game]\n";
    CHECK(FindFile({root / "cwd", root / "exe"}, "band3_config.ini") == root / "cwd" / "band3_config.ini");
    CHECK(FindFile({root / "missing"}, "band3_config.ini").empty());
    // a folder of that name isn't the file
    fs::create_directories(root / "dir" / "band3_config.ini");
    CHECK(FindFile({root / "dir"}, "band3_config.ini").empty());
    fs::remove_all(root);
}

TEST_CASE("a folder list splits on bars and drops blanks") {
    CHECK(SplitList("").empty());
    CHECK(SplitList(" | |").empty());
    CHECK(SplitList("songs") == std::vector<std::string>{"songs"});
    CHECK(SplitList(" songs | \\\\BISHOP\\Downloads\\rb |D:/dlc") ==
          std::vector<std::string>{"songs", "\\\\BISHOP\\Downloads\\rb", "D:/dlc"});
    // spaces and semicolons inside a folder's name are kept
    CHECK(SplitList("My Songs|a;b") == std::vector<std::string>{"My Songs", "a;b"});
}

TEST_CASE("the anchor is the ini's folder, or the exe's without an ini") {
    const fs::path root = fs::temp_directory_path() / "band3_anchor_test";
    fs::remove_all(root);
    fs::create_directories(root / "install");
    fs::create_directories(root / "exe");
    CHECK(Anchor(root / "install" / "band3_config.ini", root / "exe") == root / "exe");
    // the relative name LegacyIniPath falls back to, when no ini was found
    CHECK(Anchor("band3_config.ini_missing", root / "exe") == root / "exe");
    std::ofstream(root / "install" / "band3_config.ini") << "[game]\n";
    CHECK(Anchor(root / "install" / "band3_config.ini", root / "exe") == root / "install");
    // a folder of that name isn't an ini
    fs::create_directories(root / "dir" / "band3_config.ini");
    CHECK(Anchor(root / "dir" / "band3_config.ini", root / "exe") == root / "exe");
    fs::remove_all(root);
}

namespace {

// the SDK's folders with band3_config.ini's applied, as OnConfigurePaths leaves them
PathRuleInputs RuleDefaults() {
    PathRuleInputs in;
    in.anchor = fs::temp_directory_path() / "band3";
    in.defaults = {in.anchor / "assets", fs::temp_directory_path() / "home" / "band3",
                   fs::temp_directory_path() / "home" / "band3" / "cache"};
    return in;
}

}

TEST_CASE("the path rule keeps the defaults when nothing is saved") {
    auto in = RuleDefaults();
    const Folders out = ApplyPathRule(in);
    CHECK(out.game_data == in.defaults.game_data);
    CHECK(out.user_data == in.defaults.user_data);
    CHECK(out.cache == in.defaults.cache);
}

TEST_CASE("a saved folder replaces the default, relative to the anchor") {
    auto in = RuleDefaults();
    in.game_data = {"rb3", PathSource::kSaved};
    const fs::path absolute = fs::temp_directory_path() / "elsewhere" / "rb3";
    in.cache = {absolute.string(), PathSource::kSaved};
    const Folders out = ApplyPathRule(in);
    CHECK(out.game_data == in.anchor / "rb3");
    CHECK(out.cache == absolute);
    CHECK(out.user_data == in.defaults.user_data);
}

TEST_CASE("the command line and the environment keep the SDK's folders") {
    auto in = RuleDefaults();
    // the value is the cvar's; the defaults already hold what the SDK made of it
    in.game_data = {"from_the_command_line", PathSource::kFixed};
    in.user_data = {"from_the_environment", PathSource::kFixed};
    const Folders out = ApplyPathRule(in);
    CHECK(out.game_data == in.defaults.game_data);
    CHECK(out.user_data == in.defaults.user_data);
    CHECK(out.cache == in.defaults.cache);
}

TEST_CASE("an empty saved folder keeps the default") {
    auto in = RuleDefaults();
    in.game_data = {"", PathSource::kSaved};
    CHECK(ApplyPathRule(in).game_data == in.defaults.game_data);
}

TEST_CASE("the cache follows a saved user data folder unless it's set somewhere") {
    auto in = RuleDefaults();
    in.user_data = {"user_data", PathSource::kSaved};
    CHECK(ApplyPathRule(in).user_data == in.anchor / "user_data");
    CHECK(ApplyPathRule(in).cache == in.anchor / "user_data" / "cache");

    // an empty saved cache folder is no cache folder
    in.cache = {"", PathSource::kSaved};
    CHECK(ApplyPathRule(in).cache == in.anchor / "user_data" / "cache");

    // the ini's cache folder is in the defaults, and stays
    in.cache = {};
    in.cache_in_ini = true;
    CHECK(ApplyPathRule(in).cache == in.defaults.cache);

    // so does the command line's
    in.cache_in_ini = false;
    in.cache = {"cli_cache", PathSource::kFixed};
    CHECK(ApplyPathRule(in).cache == in.defaults.cache);

    // and a saved one wins
    in.cache = {"saved_cache", PathSource::kSaved};
    CHECK(ApplyPathRule(in).cache == in.anchor / "saved_cache");
}
