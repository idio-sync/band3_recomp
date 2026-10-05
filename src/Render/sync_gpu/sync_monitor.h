#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "src/Render/sync_gpu/sync_cp.h"

// Experimental (N7): what the sync-only GPU's threads (sync_graphics_system.h)
// decide and say about the command processor, kept apart from the SDK so the
// unit tests can check it: the vertical blank's period, the watchdog's line
// for a wait that doesn't end, and the log's packet summary.

namespace band3::render::sync_gpu {

// the vblank's period at the guest's refresh rate (VdQueryVideoMode's, the
// console's 60 when it's unset or nonsense); free-running, 1 ms, what the
// emulated GPU's vblank thread does with vsync off, for while the frame cap
// paces the game (frame_pacing.h)
inline constexpr int64_t kFreeRunningVblankNs = 1'000'000;
int64_t VblankPeriodNs(double refresh_hz, bool free_running);

// a type-3 opcode's name (xenos_defs.h's pm4), or its number in hex
std::string Pm4OpcodeName(uint32_t opcode);
// WAIT_REG_MEM's compare function (wait_info & 7): never, <, <=, ==, !=, >=,
// >, always
const char* WaitFunctionName(uint32_t function);

// The command processor's counters between two readings, for the log's
// summary and the test harness. Each count is now's less before's;
// wait_max_ms is the longest wait since the processor started (a maximum
// can't be taken apart).
struct SyncCpDelta {
    uint64_t packets = 0, type0 = 0, type1 = 0, type3 = 0;
    uint64_t primary_buffers = 0, indirect_buffers = 0;
    uint64_t draws_skipped = 0, predicated_skipped = 0;
    uint64_t waits = 0, stalled_waits = 0;
    double wait_ms = 0, wait_max_ms = 0;
    uint64_t interrupts = 0, swaps = 0, fences = 0, sample_count_writes = 0;
    uint64_t register_writes = 0;
    uint64_t unknown_opcodes = 0, unknown_registers = 0, out_of_range_registers = 0;
    uint64_t bad_packets = 0, bad_addresses = 0;
    // every opcode with packets, by count, most first (ties by opcode)
    std::vector<std::pair<uint32_t, uint64_t>> opcodes;
};
SyncCpDelta DeltaOf(const SyncCpStats& now, const SyncCpStats& before);

// The log's line every so often: `seconds` of `d`, the `top` opcodes by
// name, and `vblanks` raised in that time.
std::string SummaryLine(const SyncCpDelta& d, double seconds, uint64_t vblanks, size_t top = 8);

// A wait that doesn't end (a fence nobody writes, a flag nobody sets) leaves
// the game blocked behind it; the watchdog says so, once a wait has gone on
// for kStallAfter, and once more when it ends. Never aborts: a game paused in
// a debugger, or a long load, waits too. Checked about once a second.
class StallWatch {
public:
    static constexpr std::chrono::seconds kStallAfter{2};

    // `wait`: the processor's wait now (SyncCommandProcessor::current_wait);
    // `read`, `write`: its ring indices. The lines to log, usually none.
    std::vector<std::string> Check(const CurrentWait& wait, uint32_t read, uint32_t write);

private:
    bool reported_ = false;
    // the wait being watched, to tell a new one from the same one going on
    CurrentWait watched_;
};

// the watchdog's line for `wait`, which has gone on for wait.elapsed
std::string DescribeStall(const CurrentWait& wait, uint32_t read, uint32_t write);

}  // namespace band3::render::sync_gpu
