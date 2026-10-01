#pragma once
#include <mutex>

// The SDK's InputSystem has no lock: every GetState/GetCapabilities/SetState
// first re-enumerates the drivers and rebuilds its device list, and RB3 polls
// from more than one thread. Two rebuilds at once corrupt the heap as soon as a
// device's name or guid is too long for std::string's inline buffer (the
// virtual instrument's, and SDL pads' with long names). band3 takes this lock
// around every call into the input system: the game's (src/Hooks/input_lock.cpp)
// and its own (src/Hooks/pro_instruments.cpp).
//
// Recursive: the game's XInput2Sample calls the other locked functions.
// Taken before the game's own XInput critical section, never after.

namespace band3::input {

std::recursive_mutex& InputLock();

}
