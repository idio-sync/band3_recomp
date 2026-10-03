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

TEST_CASE("a test run shows it only with --launcher, and a relaunch never") {
    StartInputs in;
    in.shift_held = true;
    in.game_data_ok = false;
    in.test_port = true;
    CHECK_FALSE(DecideLauncher(in).show);
    // a harness that drives the launcher itself asks for it
    in.launcher_flag = true;
    CHECK(DecideLauncher(in).show);
    in.shift_held = false;
    in.game_data_ok = true;
    in.show_launcher = false;
    CHECK(DecideLauncher(in).show);
    in.test_port = false;
    in.relaunched = true;
    CHECK_FALSE(DecideLauncher(in).show);
    in.test_port = true;
    CHECK_FALSE(DecideLauncher(in).show);
}

TEST_CASE("every decision says why") {
    StartInputs in;
    CHECK_FALSE(std::string(DecideLauncher(in).reason).empty());
    in.test_port = true;
    CHECK_FALSE(std::string(DecideLauncher(in).reason).empty());
}

TEST_CASE("the fonts are skipped only when no input could show the launcher") {
    // every combination: whatever the game data check, Shift and the settings
    // turn out to be, a start LauncherPossible rules out never shows it
    for (int bits = 0; bits < 64; bits++) {
        StartInputs in;
        in.test_port = bits & 1;
        in.relaunched = bits & 2;
        in.game_data_ok = bits & 4;
        in.launcher_flag = bits & 8;
        in.shift_held = bits & 16;
        in.show_launcher = bits & 32;
        CAPTURE(bits);
        if (DecideLauncher(in).show) CHECK(LauncherPossible(in));
        CHECK(LauncherPossible(in) ==
              (!in.relaunched && (!in.test_port || in.launcher_flag)));
    }
}

TEST_CASE("Play restarts band3 for a new input backend only once it's saved, outside the harness") {
    CHECK(RestartsForInput({.backend_changed = true, .saved = true, .test_port = false}));
    CHECK_FALSE(RestartsForInput({.backend_changed = false, .saved = true, .test_port = false}));
    // the new run reads band3.toml
    CHECK_FALSE(RestartsForInput({.backend_changed = true, .saved = false, .test_port = false}));
    // the harness follows this process
    CHECK_FALSE(RestartsForInput({.backend_changed = true, .saved = true, .test_port = true}));
}
