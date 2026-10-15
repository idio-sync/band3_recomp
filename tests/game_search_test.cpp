// Checks Find Rock Band 3's folder search (src/Launcher/game_search.h): it
// finds a folder holding the game in a tree, stops at its depth and folder
// limits, leaves system folders alone, and lists a folder's folders in order.

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <string>
#include <vector>
#include "src/Launcher/game_search.h"

namespace fs = std::filesystem;
using namespace band3::launcher;

namespace {

struct Tree {
    fs::path root = fs::temp_directory_path() / "band3_game_search";

    Tree() {
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~Tree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    // a folder holding the game, at `path` under the root
    fs::path Game(const fs::path& path) {
        const fs::path folder = root / path;
        fs::create_directories(folder / "gen");
        std::ofstream(folder / "gen" / "main_xbox.hdr") << "hdr";
        return folder;
    }
    void Folder(const fs::path& path) { fs::create_directories(root / path); }
};

}

TEST_CASE("a folder holds the game when it has gen/main_xbox.hdr") {
    Tree t;
    const fs::path game = t.Game("Games/Rock Band 3");
    CHECK(HoldsGame(game));
    CHECK_FALSE(HoldsGame(t.root / "Games"));
    CHECK_FALSE(HoldsGame(t.root / "missing"));
}

TEST_CASE("the search finds the game's folders under the roots it's given") {
    Tree t;
    const fs::path a = t.Game("Games/Rock Band 3");
    const fs::path b = t.Game("Backup/RB3/disc");
    t.Folder("Music/Albums");
    const fs::path roots[] = {t.root};
    std::vector<fs::path> found = FindGameFolders(roots, SearchLimits{});
    std::sort(found.begin(), found.end());
    CHECK(found == std::vector<fs::path>{b, a});
}

TEST_CASE("the search stops at its depth and its folder count") {
    Tree t;
    t.Game("one/two/three/four/game");
    const fs::path roots[] = {t.root};
    CHECK(FindGameFolders(roots, SearchLimits{.depth = 3}).empty());
    CHECK(FindGameFolders(roots, SearchLimits{.depth = 5}).size() == 1);
    // so many folders before it that it gives up first
    for (int i = 0; i < 30; i++) t.Folder("aaa/f" + std::to_string(i));
    CHECK(FindGameFolders(roots, SearchLimits{.depth = 5, .max_folders = 10}).empty());
}

TEST_CASE("the search leaves the system's folders alone") {
    Tree t;
    t.Game("Windows/Rock Band 3");
    t.Game("$Recycle.Bin/Rock Band 3");
    const fs::path roots[] = {t.root};
    CHECK(FindGameFolders(roots, SearchLimits{}).empty());
}

TEST_CASE("a folder's folders are listed by name, files left out") {
    Tree t;
    t.Folder("b");
    t.Folder("A");
    t.Folder("c");
    std::ofstream(t.root / "file.txt") << "x";
    const std::vector<fs::path> folders = ListFolders(t.root);
    REQUIRE(folders.size() == 3);
    CHECK(folders[0].filename() == "A");
    CHECK(folders[1].filename() == "b");
    CHECK(folders[2].filename() == "c");
}
