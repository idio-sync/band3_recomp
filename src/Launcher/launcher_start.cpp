#include "launcher_start.h"

namespace band3::launcher {

StartDecision DecideLauncher(const StartInputs& in) {
    // a harness run has no one to press Play unless it asks for the launcher
    // (band3ctl then drives it), and a relaunch already went past it
    if (in.test_port && !in.launcher_flag) return {false, "skipped, test_port is set"};
    if (in.relaunched) return {false, "skipped, band3 relaunched itself"};
    if (!in.game_data_ok) return {true, "shown, the game data folder isn't usable"};
    if (in.launcher_flag) return {true, "shown, --launcher"};
    if (in.shift_held) return {true, "shown, Shift was held"};
    if (in.show_launcher) return {true, "shown, show_launcher is on"};
    return {false, "skipped, show_launcher is off"};
}

bool LauncherPossible(const StartInputs& in) {
    return !in.relaunched && (!in.test_port || in.launcher_flag);
}

bool RestartsForInput(const RestartInputs& in) {
    return in.backend_changed && in.saved && !in.test_port;
}

}
