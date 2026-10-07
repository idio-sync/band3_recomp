#pragma once

// See crash_trace.cpp (Windows) and crash_trace_posix.cpp (elsewhere).

#include <cstdint>
#include <string>

namespace band3::crash_trace {

#ifdef _WIN32
// Turns on Direct3D 12's Device Removed Extended Data for the devices made
// after it (the dred setting): before the SDK makes its device, which it
// does with the window.
void EnableDred();

// The SDK's Direct3D 12 device (an ID3D12Device*) and its direct queue (an
// ID3D12CommandQueue*, named so in the report), whose DRED the crash trace
// logs when the device was removed; null, null lets them go.
void WatchD3D12Device(void* device, void* direct_queue);

// What names the indexed draw a GPU hang stopped at, from how many indexed
// draws came before it in the hung command list and how many it has
// (dred_report.h's IndexedDrawNamer): the native renderer's last frame
// (GpuRenderer::DescribeIndexedDraw). Null lets it go.
void SetIndexedDrawNamer(std::string (*namer)(uint32_t before, uint32_t total));
#endif

// Logs, for a guest access violation the SDK can't handle, the guest
// functions it happened in (on Windows each frame's guest address, from the
// recompiled function it's in; elsewhere the host frames), innermost first.
// After the runtime's setup, so the SDK's own handlers (MMIO, GPU write
// watches) come first. On Windows it also puts the crash trace's unhandled
// exception filter back ahead of any the SDK set up; elsewhere it has the
// SDK's signal handlers run on the alternate signal stack, for a stack
// overflow.
void WatchGuestFaults();

// If the last run ended in a crash (a fatal exception or signal, abort() or
// std::terminate), tells the player in a message box: where its report is
// and, after a GPU hang with band3's native renderer, that renderer = emulated
// may avoid it (crash_report.h's Notice), with a button that opens the folder.
// Once: it forgets the crash.
void ShowLastCrashNotice();

// With BAND3_CRASH_TEST=abort, terminate, access-violation or stack-overflow
// in the environment, crashes that way, to check the report, the minidump (on
// Windows) and the next start's notice.
void RunCrashTest();

}  // namespace band3::crash_trace
