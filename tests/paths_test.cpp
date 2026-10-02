// Checks band3's folder settings (src/paths.cpp): how a configured path is
// resolved against the ini's folder, finding the ini, and folder lists.

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
