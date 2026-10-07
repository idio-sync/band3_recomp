#pragma once

#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "src/Render/gamma_ramp.h"
#include "src/Render/sync_gpu/xenos_defs.h"

// The sync-only GPU's command processor (renderer = native). The game still
// writes its PM4 ring and waits on the GPU's side effects: BlockOnFence's
// fences (EVENT_WRITE_SHD), the swap-complete word written by the interrupt
// handler (INTERRUPT), the read pointer MakeSpace waits on, occlusion sample
// counts (EVENT_WRITE_ZPD) and the gamma ramp (DC_LUT registers). This walks
// the ring as Xenia's command_processor.cc does and acts only on those;
// draws, shaders and state are skipped.
//
// Platform-free: memory via GuestMemory, interrupts and swaps via hooks. The
// execution entry points (ExecutePending, ExecutePrimaryBuffer) run on one
// thread; MmioRead/MmioWrite and the readers (stats, current_wait,
// DisplayGamma, counter) are callable from any.

namespace band3::render::sync_gpu {

// Guest physical memory (big-endian words). Addresses arrive with their
// Endian bits already cleared.
class GuestMemory {
public:
    // null if not guest memory
    virtual uint8_t* TranslatePhysical(uint32_t address, uint32_t size) = 0;

protected:
    ~GuestMemory() = default;
};

// An XE_SWAP packet (VdSwap's, 63 words). `body` is the whole packet,
// byte-swapped, valid only during the hook.
struct SwapPacket {
    uint32_t magic = 0;  // 'SWAP' (0x53574150) from Xenia's kernel
    uint32_t frontbuffer_ptr = 0, width = 0, height = 0;
    std::span<const uint32_t> body;
    uint64_t index = 0;  // 0 for the first swap
};

struct SyncCpHooks {
    // INTERRUPT: source 1 per cpu in the packet's mask (Xenia's
    // DispatchInterruptCallback(1, n))
    std::function<void(uint32_t source, uint32_t cpu)> interrupt;
    // XE_SWAP (Xenia's IssueSwap), before the swap bumps counter()
    std::function<void(const SwapPacket&)> swap;
    // CP_RB_WPTR written: wake the ExecutePending thread
    std::function<void()> write_pointer_updated;
    // A WAIT_REG_MEM poll didn't match. Unset: Xenia's behaviour, sleeping
    // wait / 0x100 ms for wait >= 0x100 when sleep_in_waits, else yielding.
    std::function<void(uint32_t wait)> wait;
    std::function<void(const std::string&)> log;
};

struct SyncCpConfig {
    // Xenia's query_occlusion_fake_sample_count; negative leaves queries
    // unanswered
    int32_t fake_sample_count = 1000;
    // Xenia's vsync cvar
    bool sleep_in_waits = true;
};

// One writer, any readers; relaxed.
class StatCounter {
public:
    StatCounter() = default;
    StatCounter(const StatCounter& o) : v_(o.get()) {}
    StatCounter& operator=(const StatCounter& o) {
        v_.store(o.get(), std::memory_order_relaxed);
        return *this;
    }
    void add(uint64_t n = 1) { v_.fetch_add(n, std::memory_order_relaxed); }
    void set(uint64_t n) { v_.store(n, std::memory_order_relaxed); }
    void raise_to(uint64_t n) {
        uint64_t cur = get();
        while (n > cur && !v_.compare_exchange_weak(cur, n, std::memory_order_relaxed)) {
        }
    }
    uint64_t get() const { return v_.load(std::memory_order_relaxed); }
    operator uint64_t() const { return get(); }

private:
    std::atomic<uint64_t> v_{0};
};

// WAIT_REG_MEM's wait interval (last word) in Xenia's bands (cp.cc:1018-1035):
// < 0x100 yields (spins); >= 0x100 sleeps wait / 0x100 ms; >= 0x1000 is 16+ ms
enum WaitBand : int { kWaitYield, kWaitSleep, kWaitLongSleep, kWaitBands };
inline int WaitBandOf(uint32_t wait) {
    return wait < 0x100 ? kWaitYield : wait < 0x1000 ? kWaitSleep : kWaitLongSleep;
}

struct SyncCpStats {
    // the first kWaitValues distinct stalled wait intervals, in order seen;
    // indices are stable so readings subtract index by index
    static constexpr int kWaitValues = 8;

    StatCounter primary_buffers, indirect_buffers;
    StatCounter packets;  // of any type, zero words included
    StatCounter type0, type1, type2, type3;
    StatCounter opcodes[128];  // type-3 packets by opcode, predicated-off ones included
    StatCounter unknown_opcodes;  // type-3 packets skipped as unknown
    StatCounter draws_skipped;
    StatCounter predicated_skipped;  // type-3 packets the bin predicate dropped
    StatCounter waits;                // WAIT_REG_MEM packets
    StatCounter stalled_waits;        // ...that didn't match at the first poll
    StatCounter wait_ns_total, wait_ns_max;  // the stalled ones' time
    // polls: non-matching ones, each a sleep or yield
    StatCounter stalled_by_band[kWaitBands], wait_ns_by_band[kWaitBands];
    StatCounter polls_by_band[kWaitBands];
    StatCounter wait_value_count;  // of wait_values used, up to kWaitValues
    StatCounter wait_values[kWaitValues], stalled_by_value[kWaitValues];
    StatCounter interrupts, swaps, fences, sample_count_writes;
    StatCounter register_writes;
    StatCounter unknown_register_writes, unknown_register_reads;  // with a known table
    StatCounter out_of_range_registers;  // an index past the register file
    StatCounter bad_packets;    // the rest of a buffer dropped (Xenia's "Failed to execute packet")
    StatCounter bad_addresses;  // memory a packet named that GuestMemory doesn't have
};

struct CurrentWait {
    bool active = false;
    uint32_t opcode = 0;
    bool memory = false;  // poll_addr is a physical address, else a register
    uint32_t poll_addr = 0, ref = 0, mask = 0, function = 0;
    uint32_t wait = 0;        // the packet's wait interval (WaitBandOf)
    uint32_t last_value = 0;  // the last value polled (masked)
    std::chrono::nanoseconds elapsed{0};
};

// As rex/graphics/register_file.h:40-41, zeroed like Xenia's.
struct GpuRegisters {
    alignas(4) uint32_t values[reg::kCount] = {};
};

class SyncCommandProcessor {
public:
    // before the guest writes CP_RB_WPTR for a ring (Xenia's marker,
    // cp.cc:209-246)
    static constexpr uint32_t kNoWriteIndex = 0xBAADF00D;

    explicit SyncCommandProcessor(GuestMemory& memory, SyncCpHooks hooks = {},
                                  SyncCpConfig config = {});
    SyncCommandProcessor(const SyncCommandProcessor&) = delete;
    SyncCommandProcessor& operator=(const SyncCommandProcessor&) = delete;

    // VdInitializeRingBuffer: 1 << (size_log2 + 3) bytes. Resets the write
    // index to kNoWriteIndex so nothing runs until CP_RB_WPTR is written for
    // the new ring (the game re-creates its ring after the splash; the old
    // write index would run garbage). Call before the CP thread runs, or on it.
    void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2);
    // VdEnableRingBufferRPtrWriteBack
    void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2);
    // CP_RB_WPTR, in dwords
    void UpdateWritePointer(uint32_t value);

    // One pass of Xenia's WorkerThreadMain. False if nothing to run (the
    // caller then waits for write_pointer_updated).
    bool ExecutePending();
    // Wraps; returns write_index, as Xenia does even when a bad packet drops
    // the rest
    uint32_t ExecutePrimaryBuffer(uint32_t read_index, uint32_t write_index);
    void ExecuteIndirectBuffer(uint32_t ptr, uint32_t count);
    // big-endian, if write-back is enabled
    void WriteBackReadPointer();

    uint32_t read_index() const { return read_index_.load(std::memory_order_acquire); }
    uint32_t write_index() const { return write_index_.load(std::memory_order_acquire); }
    uint32_t primary_buffer_ptr() const { return primary_buffer_ptr_; }
    uint32_t primary_buffer_size() const { return primary_buffer_size_; }  // bytes
    uint32_t read_ptr_writeback_ptr() const { return read_ptr_writeback_ptr_; }

    // With Xenia's side effects: scratch write-back, COHER_STATUS_HOST's
    // dirty bit, the DC_LUT gamma ramps.
    void WriteRegister(uint32_t index, uint32_t value);
    uint32_t ReadRegister(uint32_t index) const;
    const GpuRegisters& registers() const { return regs_; }

    // MMIO window 0x7FC80000 (mask 0xFFFF0000); register (addr & 0xFFFF) / 4.
    // Non-CP_RB_WPTR writes go through WriteRegister, unlike Xenia's raw
    // store, so a gamma ramp written by MMIO is seen.
    void MmioWrite(uint32_t addr, uint32_t value);
    // Xenia's GraphicsSystem::ReadRegister: 5 constants (EDRAM timing, a
    // 720p display, vblank status), the rest from the file.
    uint32_t MmioRead(uint32_t addr);

    // register_table.inc's; others are counted and logged once. Unset, all
    // are known. Call before the CP thread runs.
    void SetKnownRegisters(std::span<const uint32_t> indices);

    // Mode from DC_LUT_RW_MODE bit 0 (PWL); Xenia's linear ramps until the
    // guest writes one.
    GammaRamp DisplayGamma() const;

    // EVENT_WRITE_SHD's counter: bumped per swap and per vblank (Xenia's
    // MarkVblank)
    uint32_t counter() const { return counter_.load(std::memory_order_acquire); }
    void IncrementCounter() { counter_.fetch_add(1, std::memory_order_acq_rel); }

    // shutdown: breaks any wait; ExecutePending runs nothing more
    void RequestStop() { running_.store(false, std::memory_order_release); }
    bool running() const { return running_.load(std::memory_order_acquire); }

    void SetFakeSampleCount(int32_t count) {
        fake_sample_count_.store(count, std::memory_order_relaxed);
    }
    // native_query_log: the next kQueryLogLines EVENT_WRITE_ZPD packets log
    // their 8 sample-count words before clearing
    static constexpr uint32_t kQueryLogLines = 20;
    void SetQueryLog(bool on);

    const SyncCpStats& stats() const { return stats_; }
    CurrentWait current_wait() const;
    // empty before the first XE_SWAP
    std::vector<uint32_t> first_swap_body() const;

private:
    struct Reader;

    bool ExecutePacket(Reader& r);
    bool ExecutePacketType0(Reader& r, uint32_t packet);
    bool ExecutePacketType1(Reader& r, uint32_t packet);
    bool ExecutePacketType3(Reader& r, uint32_t packet);
    void ExecuteBuffer(Reader& r, const char* what);

    void Interrupt(Reader& r);
    void XeSwap(Reader& r, uint32_t count);
    void IndirectBuffer(Reader& r);
    bool WaitRegMem(Reader& r);
    void RegRmw(Reader& r);
    void RegToMem(Reader& r);
    void MemWrite(Reader& r, uint32_t count);
    void CondWrite(Reader& r);
    void EventWrite(Reader& r);
    void EventWriteShd(Reader& r);
    void EventWriteExt(Reader& r);
    void EventWriteZpd(Reader& r);
    void SetConstant(Reader& r, uint32_t count);
    void SetConstantRun(Reader& r, uint32_t count);  // SET_CONSTANT2, SET_SHADER_CONSTANTS
    void LoadAluConstant(Reader& r);
    void VizQuery(Reader& r);

    void MakeCoherent();
    void WriteGammaRegister(uint32_t index, uint32_t value);
    uint32_t LoadReg(uint32_t index) const;
    void StoreReg(uint32_t index, uint32_t value);
    // null (counted, logged) if unmapped
    uint8_t* Translate(uint32_t address, uint32_t size);
    void Log(const std::string& line);
    void LogLimited(StatCounter& counter, const std::string& line);
    void NoteUnknownRegister(uint32_t index, bool write, uint32_t value);
    void Wait(uint32_t wait);
    void CountWaitValue(uint32_t wait);

    GuestMemory& memory_;
    SyncCpHooks hooks_;
    bool sleep_in_waits_;
    std::atomic<int32_t> fake_sample_count_;
    std::atomic<uint32_t> query_log_left_{0};
    std::atomic<bool> running_{true};

    GpuRegisters regs_;
    // written by the CP thread and MMIO, read by capture;
    // gamma_rw_component_ is the next sequential write's channel
    mutable std::mutex gamma_mutex_;
    uint32_t gamma_table_[256] = {};
    uint32_t gamma_pwl_[128][3] = {};
    uint32_t gamma_rw_component_ = 0;

    uint32_t primary_buffer_ptr_ = 0;
    uint32_t primary_buffer_size_ = 0;  // bytes
    uint32_t read_ptr_update_freq_ = 0;
    uint32_t read_ptr_writeback_ptr_ = 0;
    std::atomic<uint32_t> read_index_{0};
    std::atomic<uint32_t> write_index_{kNoWriteIndex};
    std::atomic<uint32_t> counter_{0};
    uint64_t bin_select_ = 0xFFFFFFFFull;
    uint64_t bin_mask_ = 0xFFFFFFFFull;
    uint32_t indirect_depth_ = 0;

    std::vector<uint32_t> swap_body_;  // scratch for XE_SWAP's words
    std::string bad_packet_why_;       // for the log, when a packet fails

    mutable std::mutex wait_mutex_;
    CurrentWait wait_;  // elapsed unused: from wait_start_
    std::chrono::steady_clock::time_point wait_start_;

    mutable std::mutex log_mutex_;
    bool has_known_registers_ = false;
    std::bitset<reg::kCount> known_registers_;
    std::bitset<reg::kCount> logged_registers_;
    std::bitset<128> logged_opcodes_;
    bool logged_out_of_range_ = false;
    bool logged_cdcdcdcd_ = false;
    std::vector<uint32_t> first_swap_body_;

    SyncCpStats stats_;
};

}  // namespace band3::render::sync_gpu
