#include "game_data_check.h"
#include <system_error>

namespace band3::launcher {

GameDataCheck CheckGameData(const std::filesystem::path& root) {
    std::error_code ec;
    auto fail = [](GameDataProblem problem) { return GameDataCheck{false, problem}; };
    if (root.empty() || !std::filesystem::is_directory(root, ec)) {
        return fail(GameDataProblem::kFolderMissing);
    }
    if (!std::filesystem::is_regular_file(root / "default.xex", ec)) {
        return fail(GameDataProblem::kXexMissing);
    }
    if (!std::filesystem::is_regular_file(root / "gen" / "main_xbox.hdr", ec)) {
        return fail(GameDataProblem::kArchiveMissing);
    }
    return {};
}

const char* DescribeProblem(GameDataProblem problem) {
    switch (problem) {
        case GameDataProblem::kNone:
            return "";
        case GameDataProblem::kFolderMissing:
            return "The folder doesn't exist.";
        case GameDataProblem::kXexMissing:
            return "There's no default.xex in it: choose the folder Rock Band 3's disc was "
                   "extracted to.";
        case GameDataProblem::kArchiveMissing:
            return "There's no gen/main_xbox.hdr in it: the game's archives are missing, so "
                   "the extracted disc is incomplete.";
    }
    return "";
}

}
