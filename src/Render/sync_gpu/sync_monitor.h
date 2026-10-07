#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "src/Render/sync_gpu/sync_cp.h"

// The sync-only GPU's SDK-free logic, unit-testable: vblank period, stall
// watchdog, the log's packet summary.

namespace band3::render::sync_gpu {

// at the guest's refresh rate (VdQueryVideoMode's; 60 if unset or nonsense);
// free-running is 1 ms, as the emulated GPU's vblank thread does with vsync
// off, for while the frame cap paces the game
inline constexpr int64_t kFreeRunningVblankNs = 1'000'000;
int64_t VblankPeriodNs(double refresh_hz, bool free_running);

// or the opcode in hex
std::string Pm4OpcodeName(uint32_t opcode);
const char* WaitBandName(int band);
// WAIT_REG_MEM's compare function (wait_info & 7)
const char* WaitFunctionName(uint32_t function);

// Counters between two readings; wait_max_ms is since start (a maximum
// can't be differenced).
struct SyncCpDelta {
    uint64_t packets = 0, type0 = 0, type1 = 0, type3 = 0;
    uint64_t primary_buffers = 0, indirect_buffers = 0;
    uint64_t draws_skipped = 0, predicated_skipped = 0;
    uint64_t waits = 0, stalled_waits = 0;
    double wait_ms = 0, wait_max_ms = 0;
    // stalled waits by WaitBand; polls are the non-matching ones
    uint64_t stalled_by_band[kWaitBands] = {};
    double wait_ms_by_band[kWaitBands] = {};
    uint64_t polls_by_band[kWaitBands] = {};
    // stalled waits by wait interval, most first
    std::vector<std::pair<uint32_t, uint64_t>> wait_values;
    uint64_t interrupts = 0, swaps = 0, fences = 0, sample_count_writes = 0;
    uint64_t register_writes = 0;
    uint64_t unknown_opcodes = 0, unknown_registers = 0, out_of_range_registers = 0;
    uint64_t bad_packets = 0, bad_addresses = 0;
    // by count, most first (ties by opcode)
    std::vector<std::pair<uint32_t, uint64_t>> opcodes;
};
SyncCpDelta DeltaOf(const SyncCpStats& now, const SyncCpStats& before);

std::string SummaryLine(const SyncCpDelta& d, double seconds, uint64_t vblanks, size_t top = 8);

// Logs a wait that has lasted kStallAfter (a fence nobody writes), and again
// when it ends. Never aborts: a debugger pause or long load waits too.
// Checked about once a second.
class StallWatch {
public:
    static constexpr std::chrono::seconds kStallAfter{2};

    // `read`, `write`: ring indices. Returns lines to log, usually none.
    std::vector<std::string> Check(const CurrentWait& wait, uint32_t read, uint32_t write);

private:
    bool reported_ = false;
    CurrentWait watched_;
};

std::string DescribeStall(const CurrentWait& wait, uint32_t read, uint32_t write);

}  // namespace band3::render::sync_gpu
