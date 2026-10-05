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

// Experimental (N7, out/research/n7_design.md section 3): the "sync-only GPU"'s
// command processor. With emulated_gpu = off there is no emulated GPU, but the
// game still writes its PM4 ring and waits on what the GPU does with it: the
// fences D3D's BlockOnFence polls (EVENT_WRITE_SHD), the swap-complete word
// the guest's interrupt handler writes (INTERRUPT), the read pointer
// MakeSpace waits on to reuse the ring, the occlusion queries' sample counts
// (EVENT_WRITE_ZPD), and the display gamma ramp the capture applies (DC_LUT
// registers). This consumes the ring as Xenia's CommandProcessor
// (src/xenia/gpu/command_processor.cc, which ReXGlue's xenos plugin follows)
// does, and acts on only those packets: draws, shaders and state are skipped.
//
// It's the platform-free core: guest memory is reached through GuestMemory,
// interrupts and swaps through hooks, and the thread that runs it, the MMIO
// registration and the interrupt dispatch belong to the graphics system that
// owns it. Its execution entry points (ExecutePending, ExecutePrimaryBuffer)
// run on one thread; the MMIO entry points (MmioRead, MmioWrite) and the
// readers (Stats, CurrentWait, DisplayGamma, counter) may be called from any.

namespace band3::render::sync_gpu {

// Guest physical memory, as the packets address it (big-endian words; the
// low 2 bits of most packets' addresses are an Endian, cleared before this
// is asked). The game backs it with Memory::TranslatePhysical, the tests with
// a byte vector.
class GuestMemory {
public:
    // the host bytes of [address, address + size), or null if they aren't
    // guest memory
    virtual uint8_t* TranslatePhysical(uint32_t address, uint32_t size) = 0;

protected:
    ~GuestMemory() = default;
};

// An XE_SWAP packet (VdSwap's): Xenia reads a "SWAP" fourcc, the front
// buffer's physical address and size, and skips the rest of its 63 words.
// The body is every word of the packet, byte-swapped, valid during the hook.
struct SwapPacket {
    uint32_t magic = 0;  // 'SWAP' (0x53574150) from Xenia's kernel
    uint32_t frontbuffer_ptr = 0, width = 0, height = 0;
    std::span<const uint32_t> body;
    uint64_t index = 0;  // 0 for the first swap
};

struct SyncCpHooks {
    // INTERRUPT: the guest's graphics interrupt, source 1, on each cpu of the
    // packet's mask (Xenia's DispatchInterruptCallback(1, n))
    std::function<void(uint32_t source, uint32_t cpu)> interrupt;
    // XE_SWAP (Xenia's IssueSwap), before the swap bumps counter()
    std::function<void(const SwapPacket&)> swap;
    // the write pointer moved (an MMIO write of CP_RB_WPTR): wake the thread
    // that runs ExecutePending
    std::function<void()> write_pointer_updated;
    // A WAIT_REG_MEM poll that didn't match, `wait` being the packet's wait
    // interval: sleep or yield before the next. Unset: Xenia's, a sleep of
    // wait / 0x100 ms with SyncCpConfig::sleep_in_waits (its vsync cvar)
    // for a wait of 0x100 or more, a yield otherwise.
    std::function<void(uint32_t wait)> wait;
    // a line for the log: the first of each unknown opcode and register, bad
    // packets and addresses, the first swap's words
    std::function<void(const std::string&)> log;
};

struct SyncCpConfig {
    // EVENT_WRITE_ZPD's passed samples for every query, Xenia's
    // query_occlusion_fake_sample_count (1000, what RB3 gets with the
    // emulated GPU's default); negative leaves the queries unanswered
    int32_t fake_sample_count = 1000;
    // the default wait hook sleeps rather than yields (Xenia's vsync cvar)
    bool sleep_in_waits = true;
};

// A counter one thread adds to and any reads: relaxed, as statistics are.
class StatCounter {
public:
    StatCounter() = default;
    StatCounter(const StatCounter& o) : v_(o.get()) {}
    StatCounter& operator=(const StatCounter& o) {
        v_.store(o.get(), std::memory_order_relaxed);
        return *this;
    }
    void add(uint64_t n = 1) { v_.fetch_add(n, std::memory_order_relaxed); }
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

struct SyncCpStats {
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
    StatCounter interrupts, swaps, fences, sample_count_writes;
    StatCounter register_writes;
    StatCounter unknown_register_writes, unknown_register_reads;  // with a known table
    StatCounter out_of_range_registers;  // an index past the register file
    StatCounter bad_packets;    // the rest of a buffer dropped (Xenia's "Failed to execute packet")
    StatCounter bad_addresses;  // memory a packet named that GuestMemory doesn't have
};

// The wait the command processor is blocked in, for a watchdog.
struct CurrentWait {
    bool active = false;
    uint32_t opcode = 0;
    bool memory = false;  // poll_addr is a physical address, else a register
    uint32_t poll_addr = 0, ref = 0, mask = 0, function = 0;
    uint32_t last_value = 0;  // the last value polled (masked)
    std::chrono::nanoseconds elapsed{0};
};

// The register file, as rex/graphics/register_file.h:40-41 (zeroed here, as
// Xenia's RegisterFile constructor does).
struct GpuRegisters {
    alignas(4) uint32_t values[reg::kCount] = {};
};

class SyncCommandProcessor {
public:
    explicit SyncCommandProcessor(GuestMemory& memory, SyncCpHooks hooks = {},
                                  SyncCpConfig config = {});
    SyncCommandProcessor(const SyncCommandProcessor&) = delete;
    SyncCommandProcessor& operator=(const SyncCommandProcessor&) = delete;

    // VdInitializeRingBuffer: the primary ring at physical `ptr`, of
    // 1 << (size_log2 + 3) bytes as Xenia reads size_log2 (in qwords); the
    // read index back to 0. Call before the CP thread runs, or on it.
    void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2);
    // VdEnableRingBufferRPtrWriteBack: where the read index is written back
    void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2);
    // CP_RB_WPTR: the guest's write index, in dwords
    void UpdateWritePointer(uint32_t value);

    // One pass of Xenia's WorkerThreadMain: runs the ring from the read to
    // the write index and writes the read index back. False if there was
    // nothing to run (the caller then waits for write_pointer_updated).
    bool ExecutePending();
    // the packets in [read_index, write_index) of the ring, wrapping;
    // returns write_index, the new read index, as Xenia does even when a bad
    // packet drops the rest
    uint32_t ExecutePrimaryBuffer(uint32_t read_index, uint32_t write_index);
    // `count` dwords of packets at physical `ptr`
    void ExecuteIndirectBuffer(uint32_t ptr, uint32_t count);
    // the read index, big-endian, at the write-back address (if enabled)
    void WriteBackReadPointer();

    uint32_t read_index() const { return read_index_.load(std::memory_order_acquire); }
    uint32_t write_index() const { return write_index_.load(std::memory_order_acquire); }
    uint32_t primary_buffer_ptr() const { return primary_buffer_ptr_; }
    uint32_t primary_buffer_size() const { return primary_buffer_size_; }  // bytes
    uint32_t read_ptr_writeback_ptr() const { return read_ptr_writeback_ptr_; }

    // A register write from a packet (type 0/1, SET_CONSTANT, ...), with
    // Xenia's side effects: scratch write-back, COHER_STATUS_HOST's dirty
    // bit, the DC_LUT gamma ramps.
    void WriteRegister(uint32_t index, uint32_t value);
    uint32_t ReadRegister(uint32_t index) const;
    const GpuRegisters& registers() const { return regs_; }

    // The guest's MMIO window (0x7FC80000, mask 0xFFFF0000): `addr` is the
    // guest address; the register is (addr & 0xFFFF) / 4. A CP_RB_WPTR write
    // moves the write pointer; others go through WriteRegister (n7_design.md
    // section 2: Xenia's GraphicsSystem::WriteRegister stores them raw, without the
    // side effects, which would leave a gamma ramp written by MMIO unseen).
    void MmioWrite(uint32_t addr, uint32_t value);
    // Xenia's GraphicsSystem::ReadRegister: 5 registers read as constants
    // (EDRAM timing, a 720p display and vblank status), the rest the file.
    uint32_t MmioRead(uint32_t addr);

    // The registers the guest may know of (register_table.inc's): after
    // this, a write or read of any other is counted and logged once. Unset,
    // every register is taken as known. Call before the CP thread runs.
    void SetKnownRegisters(std::span<const uint32_t> indices);

    // The ramp the DC_LUT registers wrote, as gamma_ramp.h holds it, mode
    // by DC_LUT_RW_MODE (bit 0: PWL). Before the guest writes one, the
    // linear ramps Xenia starts with.
    GammaRamp DisplayGamma() const;

    // the counter EVENT_WRITE_SHD writes for its counter flag: bumped by each
    // swap and each vblank (Xenia's MarkVblank, IncrementCounter here)
    uint32_t counter() const { return counter_.load(std::memory_order_acquire); }
    void IncrementCounter() { counter_.fetch_add(1, std::memory_order_acq_rel); }

    // stops a wait the processor is blocked in and whatever it's running
    // (shutdown); ExecutePending runs nothing more after
    void RequestStop() { running_.store(false, std::memory_order_release); }
    bool running() const { return running_.load(std::memory_order_acquire); }

    void SetFakeSampleCount(int32_t count) {
        fake_sample_count_.store(count, std::memory_order_relaxed);
    }

    const SyncCpStats& stats() const { return stats_; }
    CurrentWait current_wait() const;
    // the first XE_SWAP's words (empty before one), to check its layout
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
    // memory at a packet's address: null (counted, logged) if there's none
    uint8_t* Translate(uint32_t address, uint32_t size);
    void Log(const std::string& line);
    void LogLimited(StatCounter& counter, const std::string& line);
    void NoteUnknownRegister(uint32_t index, bool write, uint32_t value);
    void Wait(uint32_t wait);

    GuestMemory& memory_;
    SyncCpHooks hooks_;
    bool sleep_in_waits_;
    std::atomic<int32_t> fake_sample_count_;
    std::atomic<bool> running_{true};

    GpuRegisters regs_;
    // DC_LUT_30_COLOR and DC_LUT_PWL_DATA values, and which of red, green,
    // blue the next sequential write is to; written by the CP thread and
    // MMIO, read by the capture
    mutable std::mutex gamma_mutex_;
    uint32_t gamma_table_[256] = {};
    uint32_t gamma_pwl_[128][3] = {};
    uint32_t gamma_rw_component_ = 0;

    uint32_t primary_buffer_ptr_ = 0;
    uint32_t primary_buffer_size_ = 0;  // bytes
    uint32_t read_ptr_update_freq_ = 0;
    uint32_t read_ptr_writeback_ptr_ = 0;
    std::atomic<uint32_t> read_index_{0};
    std::atomic<uint32_t> write_index_{0};
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
