#pragma once

// Restarting band3, for RB3Enhanced's rb3e_relaunch_game (Rock Band 3 Deluxe
// relaunches after clearing the song cache and after some settings).

namespace band3::relaunch {

// Starts band3 again with this run's command line, and its window shown the
// same way (a minimized test run stays minimized). The new one waits for this
// process to exit before it starts, so the caller ends this one next. False
// when it couldn't be started.
bool StartAgain();

// With relaunch_wait_pid set (StartAgain sets it), waits up to 30 s for that
// process to exit, then clears the setting so "Save to config" can't keep it.
// Call before anything opens files or ports.
void WaitForPrevious();

}
