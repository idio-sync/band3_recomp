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

// Never for a relaunch, nor for a test run unless --launcher asks for it (a
// harness that drives the launcher itself); always when the game data is
// missing or the launcher is asked for (--launcher, Shift); otherwise as
// show_launcher says.
StartDecision DecideLauncher(const StartInputs& in);

// Whether DecideLauncher could still show it, from test_port, relaunched and
// launcher_flag alone: a relaunch, or a test run without --launcher, rules it
// out whatever else holds. Band3App's fonts are set up before the game data
// check and Shift are known (OnConfigureFonts), and the launcher's are skipped
// only when this is false, so it never shows without them.
bool LauncherPossible(const StartInputs& in);

struct RestartInputs {
    // input_backend names another backend than the launcher's input system's
    // (input::InputBackendChanged)
    bool backend_changed = false;
    // Play saved band3.toml
    bool saved = false;
    // test_port is set: a test harness run
    bool test_port = false;
    // emulated_gpu asks for another mode than this run's
    // (render::sync_gpu::EmulatedGpuChanged): the runtime's graphics system
    // was chosen before the launcher showed
    bool gpu_changed = false;
};

// Whether Play starts band3 again rather than the game, for a new input
// backend or emulated_gpu: neither the input system nor the graphics system
// can be swapped while band3 runs (input_system.h, Band3App::OnPreSetup), and
// a relaunch skips the launcher. The new run reads band3.toml, so only once
// it's saved; and never under the test harness, which follows this process.
// Otherwise the change applies at the next start.
bool RestartsForInput(const RestartInputs& in);

}
