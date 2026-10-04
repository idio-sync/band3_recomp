#pragma once

// See crash_trace.cpp.

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
#endif

}  // namespace band3::crash_trace
