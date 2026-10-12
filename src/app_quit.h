#pragma once
#include <functional>

// Closing band3 from anywhere, as the window's close button does: band3_app.h
// sets it once the window and the runtime exist. The SDK's close path ends
// the title and then the process at once; ending the title alone
// (KernelState::TerminateTitle) went on to tear the runtime down, which on
// Linux crashed in the SDK's mouse and keyboard driver
// (MnkInputDriver::~MnkInputDriver, detaching from a window already gone)
// and left a crash report for the next start.

namespace band3::app {

void SetQuit(std::function<void()> quit);
// false when nothing has set it yet
bool Quit();

}
