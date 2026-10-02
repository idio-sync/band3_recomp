#pragma once

// Whether band3 shows the launcher before the game starts. Decided once, in
// Band3App::OnFinalizePaths.

namespace band3::launcher {

struct StartInputs {
    // test_port is set: a test harness run
    bool test_port = false;
    // started again by rb3e_relaunch_game (relaunch::WasRelaunched)
    bool relaunched = false;
    // CheckGameData passed on the folder the game would start with
    bool game_data_ok = true;
    // --launcher
    bool launcher_flag = false;
    // Shift held while band3 started (Windows only)
    bool shift_held = false;
    // the show_launcher setting
    bool show_launcher = true;
};

struct StartDecision {
    bool show = false;
    // why, for the log
    const char* reason = "";
};

// Never for a test run or a relaunch; always when the game data is missing or
// the launcher is asked for (--launcher, Shift); otherwise as show_launcher says.
StartDecision DecideLauncher(const StartInputs& in);

}
