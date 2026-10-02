#pragma once
#include <filesystem>

// Whether a folder looks like Rock Band 3's game data: the extracted disc,
// with default.xex and the gen/ folder of archives beside it. The SDK only
// checks for default.xex, so a folder without gen/ used to fail inside the game.

namespace band3::launcher {

// the first thing missing, in this order
enum class GameDataProblem {
    kNone,
    kFolderMissing,
    kXexMissing,
    kArchiveMissing,  // gen/main_xbox.hdr
};

struct GameDataCheck {
    bool ok = true;
    GameDataProblem problem = GameDataProblem::kNone;
};

GameDataCheck CheckGameData(const std::filesystem::path& root);

// what's wrong, as a sentence for the player; empty for kNone
const char* DescribeProblem(GameDataProblem problem);

}
