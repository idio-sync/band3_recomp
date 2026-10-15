#pragma once
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

// Find Rock Band 3's search: the folders on this PC that hold the game (its
// gen/main_xbox.hdr), looked for breadth first from the drives down to a
// depth, for a while at most, so a large drive doesn't keep the player
// waiting; and the folders a folder holds, to browse to one by hand.

namespace band3::launcher {

// the folder holds Rock Band 3's game data: gen/main_xbox.hdr is in it
bool HoldsGame(const std::filesystem::path& folder);

struct SearchLimits {
    // how many folder levels below a root are looked in
    int depth = 4;
    // how long the whole search may take
    std::chrono::milliseconds time{5000};
    // how many folders it may open
    size_t max_folders = 40000;
};

// the folders under `roots` (the roots too) that hold the game, nearest
// first; the system's folders and hidden ones are passed over. `stop`, when
// set, ends the search early.
std::vector<std::filesystem::path> FindGameFolders(std::span<const std::filesystem::path> roots,
                                                   const SearchLimits& limits,
                                                   const std::atomic<bool>* stop = nullptr);

// a folder's folders, by name ignoring case; hidden and system ones left out
std::vector<std::filesystem::path> ListFolders(const std::filesystem::path& folder);

// where a search starts: the drives on Windows; elsewhere the home folder and
// the mounted drives
std::vector<std::filesystem::path> SearchRoots();

}
