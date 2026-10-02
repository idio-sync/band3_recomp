#pragma once
#include <string>
#include <vector>

// Restarting band3, for RB3Enhanced's rb3e_relaunch_game (Rock Band 3 Deluxe
// relaunches after clearing the song cache and after some settings).

namespace band3::relaunch {

// Starts band3 again with this run's command line, less any --launcher, plus
// extra_args (each one argument, e.g. "--launcher=false"), and its window
// shown the same way (a minimized test run stays minimized). The new one waits
// for this process to exit before it starts, so the caller ends this one next.
// False when it couldn't be started.
bool StartAgain(const std::vector<std::string>& extra_args = {});

// With relaunch_wait_pid set (StartAgain sets it), waits up to 30 s for that
// process to exit, then clears the setting so "Save to config" can't keep it.
// Call before anything opens files or ports.
void WaitForPrevious();

// Whether this run was started by StartAgain, as WaitForPrevious found; the
// launcher doesn't show for a relaunch.
bool WasRelaunched();

}
