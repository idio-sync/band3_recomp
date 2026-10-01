#pragma once
#include <functional>

namespace rex {
class Runtime;
}
namespace rex::ui {
class Window;
class WindowedAppContext;
}

// The test harness: with test_port set, band3 takes commands (test_commands.h)
// on 127.0.0.1:test_port, one client at a time, so a script or tools/band3ctl.py
// can drive the game through the virtual instrument, read its state and take
// screenshots. Off unless test_port is set.

namespace band3::test {

// whether test_port was set when the game started
bool Enabled();

// Call once, after the settings load: with test_port set, connects the virtual
// instrument on player 1 for this session.
void Init();

// Starts listening, once the runtime and window exist. Settings changes and quit
// run on the UI thread through the app context.
void StartServer(rex::Runtime* runtime, rex::ui::WindowedAppContext* app_context,
                 rex::ui::Window* window);
void StopServer();

}
