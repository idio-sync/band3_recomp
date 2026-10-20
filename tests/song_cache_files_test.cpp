// Checks the song cache's files (src/Game/song_cache_files.cpp): which packages
// are song caches, and that a song cache package missing its cache file, which
// the game fails to read and never writes again, gets the empty cache a new
// profile starts with.

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include "src/Game/song_cache_files.h"

namespace fs = std::filesystem;
using namespace band3::song_cache;

namespace {

struct TempRoot {
    fs::path path;
    TempRoot() {
        path = fs::temp_directory_path() / "band3_song_cache_test";
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempRoot() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::vector<char> Contents(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}

TEST_CASE("songcache and rbdxcache are song caches") {
    CHECK(IsSongCache("songcache"));
    CHECK(IsSongCache("rbdxcache"));
    CHECK_FALSE(IsSongCache("band3"));
    CHECK_FALSE(IsSongCache("globaloptions"));
}

TEST_CASE("a song cache package without its file gets the empty cache") {
    TempRoot root;
    const fs::path package = root.path / "B13EBABEBABEBABE" / "45410914" / "00000001" / "rbdxcache";
    fs::create_directories(package);

    const auto written = RestoreMissingCaches(root.path, "45410914", "00000001", "rbdxcache");
    REQUIRE(written.size() == 1);
    CHECK(written[0] == package / "rbdxcache");
    // what the game writes for a new profile: format 13, no songs
    std::vector<char> expected(32, 0);
    expected[0] = 0x0d;
    CHECK(Contents(package / "rbdxcache") == expected);
}

TEST_CASE("a song cache with its file is left alone") {
    TempRoot root;
    const fs::path package = root.path / "B13EBABEBABEBABE" / "45410914" / "00000001" / "rbdxcache";
    fs::create_directories(package);
    std::ofstream(package / "rbdxcache", std::ios::binary) << "songs";

    CHECK(RestoreMissingCaches(root.path, "45410914", "00000001", "rbdxcache").empty());
    CHECK(Contents(package / "rbdxcache") == std::vector<char>{'s', 'o', 'n', 'g', 's'});
}

TEST_CASE("every profile folder is looked in, and no package is fine") {
    TempRoot root;
    const fs::path empty = root.path / "E000000000000001" / "45410914" / "00000001" / "songcache";
    fs::create_directories(empty);
    fs::create_directories(root.path / "E000000000000002" / "45410914");
    fs::create_directories(root.path / "cache");

    const auto written = RestoreMissingCaches(root.path, "45410914", "00000001", "songcache");
    CHECK(written.size() == 1);
    CHECK(fs::is_regular_file(empty / "songcache"));
    // no package: the game makes one, and its first cache, itself
    CHECK_FALSE(fs::exists(root.path / "E000000000000002" / "45410914" / "00000001"));
    CHECK(RestoreMissingCaches(root.path / "missing", "45410914", "00000001", "songcache").empty());
}
