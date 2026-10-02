// Checks the launcher's startup pieces (src/Launcher/): the game data folder
// check and when the launcher shows.

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <string>
#include "src/Launcher/game_data_check.h"
#include "src/Launcher/launcher_start.h"

namespace fs = std::filesystem;
using namespace band3::launcher;

TEST_CASE("the game data check names the first thing missing") {
    const fs::path root = fs::temp_directory_path() / "band3_game_data_test";
    fs::remove_all(root);

    CHECK(CheckGameData(root).problem == GameDataProblem::kFolderMissing);
    CHECK_FALSE(CheckGameData(root).ok);
    CHECK(CheckGameData({}).problem == GameDataProblem::kFolderMissing);

    fs::create_directories(root);
    CHECK(CheckGameData(root).problem == GameDataProblem::kXexMissing);
    // a folder named default.xex isn't the executable
    fs::create_directories(root / "default.xex");
    CHECK(CheckGameData(root).problem == GameDataProblem::kXexMissing);
    fs::remove(root / "default.xex");

    std::ofstream(root / "default.xex") << "XEX2";
    CHECK(CheckGameData(root).problem == GameDataProblem::kArchiveMissing);
    fs::create_directories(root / "gen");
    CHECK(CheckGameData(root).problem == GameDataProblem::kArchiveMissing);

    std::ofstream(root / "gen" / "main_xbox.hdr") << "hdr";
    CHECK(CheckGameData(root).ok);
    CHECK(CheckGameData(root).problem == GameDataProblem::kNone);

    // a file where the folder should be
    CHECK(CheckGameData(root / "default.xex").problem == GameDataProblem::kFolderMissing);
    fs::remove_all(root);
}

TEST_CASE("every problem has a description, and none has none") {
    CHECK(std::string(DescribeProblem(GameDataProblem::kNone)).empty());
    CHECK_FALSE(std::string(DescribeProblem(GameDataProblem::kFolderMissing)).empty());
    CHECK_FALSE(std::string(DescribeProblem(GameDataProblem::kXexMissing)).empty());
    CHECK_FALSE(std::string(DescribeProblem(GameDataProblem::kArchiveMissing)).empty());
}

TEST_CASE("the launcher shows on a first run, and not once show_launcher is off") {
    StartInputs in;
    CHECK(DecideLauncher(in).show);
    in.show_launcher = false;
    CHECK_FALSE(DecideLauncher(in).show);
}

TEST_CASE("--launcher, Shift and missing game data show it whatever show_launcher says") {
    StartInputs in;
    in.show_launcher = false;
    in.launcher_flag = true;
    CHECK(DecideLauncher(in).show);
    in.launcher_flag = false;
    in.shift_held = true;
    CHECK(DecideLauncher(in).show);
    in.shift_held = false;
    in.game_data_ok = false;
    CHECK(DecideLauncher(in).show);
}

TEST_CASE("a test run and a relaunch never show it") {
    StartInputs in;
    in.launcher_flag = true;
    in.shift_held = true;
    in.game_data_ok = false;
    in.test_port = true;
    CHECK_FALSE(DecideLauncher(in).show);
    in.test_port = false;
    in.relaunched = true;
    CHECK_FALSE(DecideLauncher(in).show);
}

TEST_CASE("every decision says why") {
    StartInputs in;
    CHECK_FALSE(std::string(DecideLauncher(in).reason).empty());
    in.test_port = true;
    CHECK_FALSE(std::string(DecideLauncher(in).reason).empty());
}
