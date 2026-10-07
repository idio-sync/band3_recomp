#include "src/Render/sync_gpu/sync_monitor.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <iterator>

#include "src/Render/sync_gpu/xenos_defs.h"

// See sync_monitor.h.

namespace band3::render::sync_gpu {

int64_t VblankPeriodNs(double refresh_hz, bool free_running) {
    if (free_running) return kFreeRunningVblankNs;
    // VdQueryVideoMode keeps the guest's rate to 24..240 Hz
    if (!std::isfinite(refresh_hz) || refresh_hz < 1 || refresh_hz > 1000) refresh_hz = 60;
    return static_cast<int64_t>(std::llround(1e9 / refresh_hz));
}

std::string Pm4OpcodeName(uint32_t opcode) {
    switch (opcode) {
        case pm4::NOP: return "NOP";
        case pm4::REG_RMW: return "REG_RMW";
        case pm4::DRAW_INDX: return "DRAW_INDX";
        case pm4::VIZ_QUERY: return "VIZ_QUERY";
        case pm4::WAIT_FOR_IDLE: return "WAIT_FOR_IDLE";
        case pm4::IM_LOAD: return "IM_LOAD";
        case pm4::IM_LOAD_IMMEDIATE: return "IM_LOAD_IMMEDIATE";
        case pm4::SET_CONSTANT: return "SET_CONSTANT";
        case pm4::LOAD_ALU_CONSTANT: return "LOAD_ALU_CONSTANT";
        case pm4::DRAW_INDX_BIN: return "DRAW_INDX_BIN";
        case pm4::DRAW_INDX_2_BIN: return "DRAW_INDX_2_BIN";
        case pm4::DRAW_INDX_2: return "DRAW_INDX_2";
        case pm4::INDIRECT_BUFFER_PFD: return "INDIRECT_BUFFER_PFD";
        case pm4::INVALIDATE_STATE: return "INVALIDATE_STATE";
        case pm4::WAIT_REG_MEM: return "WAIT_REG_MEM";
        case pm4::MEM_WRITE: return "MEM_WRITE";
        case pm4::REG_TO_MEM: return "REG_TO_MEM";
        case pm4::INDIRECT_BUFFER: return "INDIRECT_BUFFER";
        case pm4::COND_WRITE: return "COND_WRITE";
        case pm4::EVENT_WRITE: return "EVENT_WRITE";
        case pm4::ME_INIT: return "ME_INIT";
        case pm4::SET_BIN_BASE_OFFSET: return "SET_BIN_BASE_OFFSET";
        case pm4::SET_BIN_MASK: return "SET_BIN_MASK";
        case pm4::SET_BIN_SELECT: return "SET_BIN_SELECT";
        case pm4::WAIT_REG_EQ: return "WAIT_REG_EQ";
        case pm4::WAIT_REG_GTE: return "WAIT_REG_GTE";
        case pm4::INTERRUPT: return "INTERRUPT";
        case pm4::SET_CONSTANT2: return "SET_CONSTANT2";
        case pm4::SET_SHADER_CONSTANTS: return "SET_SHADER_CONSTANTS";
        case pm4::EVENT_WRITE_SHD: return "EVENT_WRITE_SHD";
        case pm4::EVENT_WRITE_CFL: return "EVENT_WRITE_CFL";
        case pm4::EVENT_WRITE_EXT: return "EVENT_WRITE_EXT";
        case pm4::EVENT_WRITE_ZPD: return "EVENT_WRITE_ZPD";
        case pm4::CONTEXT_UPDATE: return "CONTEXT_UPDATE";
        case pm4::SET_BIN_MASK_LO: return "SET_BIN_MASK_LO";
        case pm4::SET_BIN_MASK_HI: return "SET_BIN_MASK_HI";
        case pm4::SET_BIN_SELECT_LO: return "SET_BIN_SELECT_LO";
        case pm4::SET_BIN_SELECT_HI: return "SET_BIN_SELECT_HI";
        case pm4::XE_SWAP: return "XE_SWAP";
        default: return std::format("{:02X}", opcode);
    }
}

const char* WaitBandName(int band) {
    switch (band) {
        case kWaitYield: return "yield";
        case kWaitSleep: return "sleep";
        case kWaitLongSleep: return "long_sleep";
        default: return "?";
    }
}

const char* WaitFunctionName(uint32_t function) {
    static constexpr const char* kNames[] = {"never", "<", "<=", "==", "!=", ">=", ">", "always"};
    return kNames[function & 7];
}

SyncCpDelta DeltaOf(const SyncCpStats& now, const SyncCpStats& before) {
    SyncCpDelta d;
    auto minus = [](const StatCounter& a, const StatCounter& b) {
        return a.get() >= b.get() ? a.get() - b.get() : 0;
    };
    d.packets = minus(now.packets, before.packets);
    d.type0 = minus(now.type0, before.type0);
    d.type1 = minus(now.type1, before.type1);
    d.type3 = minus(now.type3, before.type3);
    d.primary_buffers = minus(now.primary_buffers, before.primary_buffers);
    d.indirect_buffers = minus(now.indirect_buffers, before.indirect_buffers);
    d.draws_skipped = minus(now.draws_skipped, before.draws_skipped);
    d.predicated_skipped = minus(now.predicated_skipped, before.predicated_skipped);
    d.waits = minus(now.waits, before.waits);
    d.stalled_waits = minus(now.stalled_waits, before.stalled_waits);
    d.wait_ms = double(minus(now.wait_ns_total, before.wait_ns_total)) / 1e6;
    d.wait_max_ms = double(now.wait_ns_max.get()) / 1e6;
    for (int b = 0; b < kWaitBands; b++) {
        d.stalled_by_band[b] = minus(now.stalled_by_band[b], before.stalled_by_band[b]);
        d.wait_ms_by_band[b] =
            double(minus(now.wait_ns_by_band[b], before.wait_ns_by_band[b])) / 1e6;
        d.polls_by_band[b] = minus(now.polls_by_band[b], before.polls_by_band[b]);
    }
    // indices match in both (SyncCpStats::wait_values); one new since
    // `before` counts from 0
    const uint64_t values = std::min<uint64_t>(now.wait_value_count.get(), SyncCpStats::kWaitValues);
    for (uint64_t i = 0; i < values; i++) {
        const uint64_t was = i < before.wait_value_count.get() ? before.stalled_by_value[i].get() : 0;
        const uint64_t is = now.stalled_by_value[i].get();
        if (is > was) d.wait_values.emplace_back(uint32_t(now.wait_values[i].get()), is - was);
    }
    std::stable_sort(d.wait_values.begin(), d.wait_values.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    d.interrupts = minus(now.interrupts, before.interrupts);
    d.swaps = minus(now.swaps, before.swaps);
    d.fences = minus(now.fences, before.fences);
    d.sample_count_writes = minus(now.sample_count_writes, before.sample_count_writes);
    d.register_writes = minus(now.register_writes, before.register_writes);
    d.unknown_opcodes = minus(now.unknown_opcodes, before.unknown_opcodes);
    d.unknown_registers = minus(now.unknown_register_writes, before.unknown_register_writes) +
                          minus(now.unknown_register_reads, before.unknown_register_reads);
    d.out_of_range_registers = minus(now.out_of_range_registers, before.out_of_range_registers);
    d.bad_packets = minus(now.bad_packets, before.bad_packets);
    d.bad_addresses = minus(now.bad_addresses, before.bad_addresses);
    for (uint32_t op = 0; op < std::size(now.opcodes); op++) {
        if (const uint64_t n = minus(now.opcodes[op], before.opcodes[op])) d.opcodes.emplace_back(op, n);
    }
    std::stable_sort(d.opcodes.begin(), d.opcodes.end(),
                     [](const auto& a, const auto& b) { return a.second > b.second; });
    return d;
}

namespace {

// stalled waits per wait band (waits, time, polls), then the intervals
std::string StalledWaits(const SyncCpDelta& d) {
    std::string out = "stalled by wait interval:";
    for (int b = 0; b < kWaitBands; b++) {
        out += std::format("{} {} {} ({:.1f} ms, {} polls)", b ? "," : "", WaitBandName(b),
                           d.stalled_by_band[b], d.wait_ms_by_band[b], d.polls_by_band[b]);
    }
    out += "; intervals";
    for (size_t i = 0; i < d.wait_values.size(); i++)
        out += std::format("{} 0x{:X} {}", i ? "," : "", d.wait_values[i].first,
                           d.wait_values[i].second);
    if (d.wait_values.empty()) out += " none";
    return out;
}

}  // namespace

std::string SummaryLine(const SyncCpDelta& d, double seconds, uint64_t vblanks, size_t top) {
    std::string opcodes;
    for (size_t i = 0; i < d.opcodes.size() && i < top; i++) {
        opcodes += std::format("{}{} {}", i ? ", " : "", Pm4OpcodeName(d.opcodes[i].first),
                               d.opcodes[i].second);
    }
    if (d.opcodes.size() > top) opcodes += std::format(" and {} more", d.opcodes.size() - top);
    if (opcodes.empty()) opcodes = "none";
    return std::format(
        "sync gpu: the last {:.0f} s: {} packets (type 0 {}, type 1 {}, type 3 {}) in {} ring "
        "runs and {} indirect buffers; opcodes {}; draws skipped {}; waits {} ({} stalled, "
        "{:.1f} ms in all, longest yet {:.1f} ms); {}; interrupts {}, swaps {}, vblanks {}, "
        "fences {}, ZPD {}; unknown opcodes {}, unknown registers {}, out of range {}, bad "
        "packets {}, bad addresses {}",
        seconds, d.packets, d.type0, d.type1, d.type3, d.primary_buffers, d.indirect_buffers,
        opcodes, d.draws_skipped, d.waits, d.stalled_waits, d.wait_ms, d.wait_max_ms,
        StalledWaits(d), d.interrupts, d.swaps, vblanks, d.fences, d.sample_count_writes,
        d.unknown_opcodes, d.unknown_registers, d.out_of_range_registers, d.bad_packets,
        d.bad_addresses);
}

namespace {

// what a wait polls: a physical address (its endian bits apart) or a register
std::string Polled(const CurrentWait& w) {
    if (w.memory)
        return std::format("memory {:08X} (endian {})", w.poll_addr & ~uint32_t(3),
                           w.poll_addr & 3);
    return std::format("register {:04X}", w.poll_addr);
}

double Seconds(std::chrono::nanoseconds ns) { return std::chrono::duration<double>(ns).count(); }

bool SameWait(const CurrentWait& a, const CurrentWait& b) {
    return a.active && b.active && a.opcode == b.opcode && a.memory == b.memory &&
           a.poll_addr == b.poll_addr && a.ref == b.ref && a.mask == b.mask &&
           a.function == b.function && a.elapsed >= b.elapsed;
}

}  // namespace

std::string DescribeStall(const CurrentWait& w, uint32_t read, uint32_t write) {
    return std::format(
        "sync gpu: watchdog: the command processor has waited {:.1f} s in {} for {} & {:08X} "
        "{} {:08X} (last read {:08X}); ring read index {:X}, write index {:X}; wait interval "
        "0x{:X} ({}). The game waits behind it",
        Seconds(w.elapsed), Pm4OpcodeName(w.opcode), Polled(w), w.mask,
        WaitFunctionName(w.function), w.ref, w.last_value, read, write, w.wait,
        WaitBandName(WaitBandOf(w.wait)));
}

std::vector<std::string> StallWatch::Check(const CurrentWait& wait, uint32_t read,
                                           uint32_t write) {
    std::vector<std::string> lines;
    const bool same = SameWait(wait, watched_);
    if (reported_ && !same) {
        lines.push_back(std::format("sync gpu: watchdog: the wait for {} ended after {:.0f} s or more",
                                    Polled(watched_), Seconds(watched_.elapsed)));
        reported_ = false;
    }
    watched_ = wait;
    if (wait.active && !reported_ && wait.elapsed >= kStallAfter) {
        reported_ = true;
        lines.push_back(DescribeStall(wait, read, write));
    }
    return lines;
}

}  // namespace band3::render::sync_gpu
