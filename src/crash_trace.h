#pragma once

// See crash_trace.cpp.

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

// Logs, for a guest access violation the SDK can't handle, the guest
// functions it happened in (each frame's guest address, from the recompiled
// function it's in), innermost first. After the runtime's setup, so the SDK's
// own handlers (MMIO, GPU write watches) come first.
void WatchGuestFaults();
#endif

}  // namespace band3::crash_trace
