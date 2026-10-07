/**
 * The packet handling in this file follows Xenia's
 * src/xenia/gpu/command_processor.cc and graphics_system.cc line by line:
 *
 * Copyright (c) 2015, Ben Vanik.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the project nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL BEN VANIK BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "src/Render/sync_gpu/sync_cp.h"

#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <thread>

// "cp.cc:N" / "gs.cc:N": Xenia's command_processor.cc / graphics_system.cc at
// master (fetched 2026-10-05). ReXGlue's xenos plugin source isn't available;
// where it may differ (its D3D12 EVENT_WRITE_ZPD) this follows Xenia.

namespace band3::render::sync_gpu {

// Xenia's RingBuffer (base/ring_buffer.h) in dwords. read == write is empty,
// so a full `capacity` buffer starts "empty" and is run with a do-while.
struct SyncCommandProcessor::Reader {
    const uint8_t* base;
    uint32_t capacity;  // dwords, nonzero
    uint32_t read = 0, write = 0;

    uint32_t ReadCount() const {
        if (read == write) return 0;
        if (read < write) return write - read;
        return capacity - read + write;
    }
    // ReadAndSwap<uint32_t>
    uint32_t Read() {
        const uint32_t v = LoadBE32(base + size_t(read) * 4);
        read = read + 1 == capacity ? 0 : read + 1;
        return v;
    }
    void Advance(uint32_t n) { read = uint32_t((uint64_t(read) + n) % capacity); }
};

namespace {

// WAIT_REG_MEM's and COND_WRITE's compare (cp.cc:990-1015, 1134-1159)
bool Compare(uint32_t function, uint32_t value, uint32_t ref) {
    switch (function & 7) {
        case 0: return false;  // never
        case 1: return value < ref;
        case 2: return value <= ref;
        case 3: return value == ref;
        case 4: return value != ref;
        case 5: return value >= ref;
        case 6: return value > ref;
        default: return true;  // always
    }
}

// SET_CONSTANT's and LOAD_ALU_CONSTANT's register block by type
// (cp.cc:1437-1457): ALU, fetch, bool, loop constants, registers
bool ConstantBase(uint32_t type, uint32_t& base) {
    static constexpr uint32_t kBases[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
    if (type >= std::size(kBases)) return false;
    base = kBases[type];
    return true;
}

// DC_LUT_30_COLOR's fields (rex/graphics/registers.h:951-958)
constexpr uint32_t kBlueShift = 0, kGreenShift = 10, kRedShift = 20;

void SetField10(uint32_t& entry, uint32_t shift, uint32_t value) {
    entry = (entry & ~(0x3FFu << shift)) | ((value & 0x3FF) << shift);
}

// index in bits 0..7 (registers.h:911-918), the rest preserved
uint32_t WithRwIndex(uint32_t reg_value, uint32_t index) {
    return (reg_value & ~0xFFu) | (index & 0xFF);
}

}  // namespace

SyncCommandProcessor::SyncCommandProcessor(GuestMemory& memory, SyncCpHooks hooks,
                                           SyncCpConfig config)
    : memory_(memory),
      hooks_(std::move(hooks)),
      sleep_in_waits_(config.sleep_in_waits),
      fake_sample_count_(config.fake_sample_count) {
    // Xenia's initial linear ramps (cp.cc:56-70)
    for (uint32_t i = 0; i < 256; ++i) {
        const uint32_t value = i * 0x3FF / 0xFF;
        gamma_table_[i] = value << kBlueShift | value << kGreenShift | value << kRedShift;
    }
    for (uint32_t i = 0; i < 128; ++i) {
        const uint32_t base = (i * 0xFFFF / 0x7F) & ~UINT32_C(0x3F);
        const uint32_t delta = i < 0x7F ? 0x200 : 0;
        for (uint32_t j = 0; j < 3; ++j) gamma_pwl_[i][j] = base | delta << 16;
    }
}

void SyncCommandProcessor::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
    // cp.cc:309-313; size_log2 counts qwords. Resetting the write index is
    // band3's (see the declaration).
    read_index_.store(0, std::memory_order_release);
    write_index_.store(kNoWriteIndex, std::memory_order_release);
    primary_buffer_ptr_ = ptr;
    primary_buffer_size_ = size_log2 + 3 < 32 ? uint32_t(1) << (size_log2 + 3) : 0;
}

void SyncCommandProcessor::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
    // cp.cc:315-324; the update frequency is unused, as in Xenia
    read_ptr_writeback_ptr_ = ptr;
    read_ptr_update_freq_ = block_size_log2 < 32 ? uint32_t(1) << block_size_log2 >> 2 : 0;
}

void SyncCommandProcessor::UpdateWritePointer(uint32_t value) {
    write_index_.store(value, std::memory_order_release);
    if (hooks_.write_pointer_updated) hooks_.write_pointer_updated();
}

bool SyncCommandProcessor::ExecutePending() {
    // cp.cc:209-246, without the spin
    if (!running()) return false;
    const uint32_t write = write_index();
    const uint32_t read = read_index();
    if (write == kNoWriteIndex || read == write) return false;
    read_index_.store(ExecutePrimaryBuffer(read, write), std::memory_order_release);
    WriteBackReadPointer();
    return true;
}

void SyncCommandProcessor::WriteBackReadPointer() {
    // cp.cc:243-246. Release: the guest reads the packets' writes after it.
    if (!read_ptr_writeback_ptr_) return;
    uint8_t* p = Translate(read_ptr_writeback_ptr_, 4);
    if (!p) return;
    const uint32_t be = std::byteswap(read_index());
    if (reinterpret_cast<std::uintptr_t>(p) % 4 == 0) {
        StoreHost32Release(p, be);
    } else {
        std::atomic_thread_fence(std::memory_order_release);
        StoreHost32(p, be);
    }
}

uint32_t SyncCommandProcessor::ExecutePrimaryBuffer(uint32_t read_index, uint32_t write_index) {
    // cp.cc:530-573
    stats_.primary_buffers.add();
    const uint32_t capacity = primary_buffer_size_ / 4;
    const uint8_t* base = capacity ? Translate(primary_buffer_ptr_, primary_buffer_size_) : nullptr;
    if (!base) {
        LogLimited(stats_.bad_packets,
                   std::format("sync gpu: no primary ring at {:08X} ({} bytes); dropped "
                               "{:X}..{:X}",
                               primary_buffer_ptr_, primary_buffer_size_, read_index,
                               write_index));
        return write_index;
    }
    Reader r{base, capacity, read_index % capacity, write_index % capacity};
    // Xenia asserts read != write; a write a whole ring ahead reads nothing
    if (r.ReadCount()) ExecuteBuffer(r, "primary ring");
    return write_index;
}

void SyncCommandProcessor::ExecuteIndirectBuffer(uint32_t ptr, uint32_t count) {
    // cp.cc:575-593. Empty is skipped: Xenia's would divide by zero.
    stats_.indirect_buffers.add();
    if (!count) return;
    const uint8_t* base = Translate(ptr, count * 4);
    if (!base) return;
    Reader r{base, count, 0, 0};
    ExecuteBuffer(r, "indirect buffer");
}

void SyncCommandProcessor::ExecuteBuffer(Reader& r, const char* what) {
    // cp.cc:559-566 and 583-590: a bad packet or RequestStop drops the rest
    do {
        if (!ExecutePacket(r)) {
            if (running()) {
                LogLimited(stats_.bad_packets,
                           std::format("sync gpu: {} in the {}; dropped the rest",
                                       bad_packet_why_.empty() ? "a packet failed"
                                                               : bad_packet_why_,
                                       what));
            }
            bad_packet_why_.clear();
            break;
        }
    } while (r.ReadCount() && running());
}

bool SyncCommandProcessor::ExecutePacket(Reader& r) {
    // cp.cc:608-634
    const uint32_t packet = r.Read();
    stats_.packets.add();
    if (packet == 0) return true;
    if (packet == 0xCDCDCDCD) {
        std::lock_guard lock(log_mutex_);
        if (!logged_cdcdcdcd_) {
            logged_cdcdcdcd_ = true;
            if (hooks_.log)
                hooks_.log("sync gpu: packet CDCDCDCD, probably uninitialized memory");
        }
    }
    switch (packet >> 30) {
        case 0: return ExecutePacketType0(r, packet);
        case 1: return ExecutePacketType1(r, packet);
        case 2: stats_.type2.add(); return true;  // no-op (cp.cc:677-683)
        default: return ExecutePacketType3(r, packet);
    }
}

bool SyncCommandProcessor::ExecutePacketType0(Reader& r, uint32_t packet) {
    // cp.cc:636-661: count registers from base, or count writes of one
    stats_.type0.add();
    const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
    if (r.ReadCount() < count) {
        bad_packet_why_ = std::format("type-0 packet {:08X} overflows it ({} of {} dwords)",
                                      packet, r.ReadCount(), count);
        return false;
    }
    const uint32_t base_index = packet & 0x7FFF;
    const bool write_one_reg = (packet >> 15) & 1;
    for (uint32_t m = 0; m < count; m++) {
        const uint32_t reg_data = r.Read();
        WriteRegister(write_one_reg ? base_index : base_index + m, reg_data);
    }
    return true;
}

bool SyncCommandProcessor::ExecutePacketType1(Reader& r, uint32_t packet) {
    // cp.cc:663-675. Unchecked against the buffer's end, as in Xenia.
    stats_.type1.add();
    const uint32_t reg_index_1 = packet & 0x7FF;
    const uint32_t reg_index_2 = (packet >> 11) & 0x7FF;
    const uint32_t reg_data_1 = r.Read();
    const uint32_t reg_data_2 = r.Read();
    WriteRegister(reg_index_1, reg_data_1);
    WriteRegister(reg_index_2, reg_data_2);
    return true;
}

bool SyncCommandProcessor::ExecutePacketType3(Reader& r, uint32_t packet) {
    // cp.cc:685-878
    stats_.type3.add();
    const uint32_t opcode = (packet >> 8) & 0x7F;
    const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;
    if (r.ReadCount() < count) {
        bad_packet_why_ =
            std::format("type-3 packet {:08X} (opcode {:02X}) overflows it ({} of {} dwords)",
                        packet, opcode, r.ReadCount(), count);
        return false;
    }
    stats_.opcodes[opcode].add();
    const uint32_t body_end = uint32_t((uint64_t(r.read) + count) % r.capacity);

    // Bit 0 predicates the packet on the bins (cp.cc:705-715); a predicated
    // swap is never valid
    if (packet & 1) {
        const bool any_pass = (bin_select_ & bin_mask_) != 0;
        if (!any_pass || opcode == pm4::XE_SWAP) {
            stats_.predicated_skipped.add();
            r.Advance(count);
            return true;
        }
    }

    bool result = true;
    switch (opcode) {
        // not needed without drawing (cp.cc:880-898, 1528-1599)
        case pm4::ME_INIT:
        case pm4::NOP:
        case pm4::IM_LOAD:
        case pm4::IM_LOAD_IMMEDIATE:
        case pm4::INVALIDATE_STATE:
        // Xenia reads one word, logs and asserts on it (cp.cc:829-844)
        case pm4::CONTEXT_UPDATE:
        case pm4::WAIT_FOR_IDLE:
        // unknown to Xenia; nothing waits on them
        case pm4::SET_BIN_BASE_OFFSET:
        case pm4::EVENT_WRITE_CFL: break;

        // Xenia's ExecutePacketType3Draw (cp.cc:1284) also writes
        // VGT_DRAW_INITIATOR and VGT_DMA_BASE/SIZE, which nothing waits on
        case pm4::DRAW_INDX:
        case pm4::DRAW_INDX_2:
        case pm4::DRAW_INDX_BIN:
        case pm4::DRAW_INDX_2_BIN: stats_.draws_skipped.add(); break;

        case pm4::INTERRUPT: Interrupt(r); break;
        case pm4::XE_SWAP: XeSwap(r, count); break;
        case pm4::INDIRECT_BUFFER:
        case pm4::INDIRECT_BUFFER_PFD: IndirectBuffer(r); break;
        case pm4::WAIT_REG_MEM: result = WaitRegMem(r); break;
        case pm4::REG_RMW: RegRmw(r); break;
        case pm4::REG_TO_MEM: RegToMem(r); break;
        case pm4::MEM_WRITE: MemWrite(r, count); break;
        case pm4::COND_WRITE: CondWrite(r); break;
        case pm4::EVENT_WRITE: EventWrite(r); break;
        case pm4::EVENT_WRITE_SHD: EventWriteShd(r); break;
        case pm4::EVENT_WRITE_EXT: EventWriteExt(r); break;
        case pm4::EVENT_WRITE_ZPD: EventWriteZpd(r); break;
        case pm4::SET_CONSTANT: SetConstant(r, count); break;
        case pm4::SET_CONSTANT2:
        case pm4::SET_SHADER_CONSTANTS: SetConstantRun(r, count); break;
        case pm4::LOAD_ALU_CONSTANT: LoadAluConstant(r); break;
        case pm4::VIZ_QUERY: VizQuery(r); break;

        // cp.cc:793-828
        case pm4::SET_BIN_MASK_LO:
            bin_mask_ = (bin_mask_ & 0xFFFFFFFF00000000ull) | r.Read();
            break;
        case pm4::SET_BIN_MASK_HI:
            bin_mask_ = (bin_mask_ & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32);
            break;
        case pm4::SET_BIN_SELECT_LO:
            bin_select_ = (bin_select_ & 0xFFFFFFFF00000000ull) | r.Read();
            break;
        case pm4::SET_BIN_SELECT_HI:
            bin_select_ = (bin_select_ & 0xFFFFFFFFull) | (uint64_t(r.Read()) << 32);
            break;
        case pm4::SET_BIN_MASK: {
            const uint64_t hi = r.Read();
            const uint64_t lo = r.Read();
            bin_mask_ = (hi << 32) | lo;
        } break;
        case pm4::SET_BIN_SELECT: {
            const uint64_t hi = r.Read();
            const uint64_t lo = r.Read();
            bin_select_ = (hi << 32) | lo;
        } break;

        // cp.cc:846-851. WAIT_REG_EQ/GTE too, as in Xenia and ReXGlue: their
        // layout is undocumented and a misread wait would block the ring.
        default: {
            stats_.unknown_opcodes.add();
            std::lock_guard lock(log_mutex_);
            if (!logged_opcodes_[opcode]) {
                logged_opcodes_[opcode] = true;
                if (hooks_.log)
                    hooks_.log(std::format("sync gpu: unknown PM4 opcode {:02X} ({} dwords), "
                                           "skipped",
                                           opcode, count));
            }
        } break;
    }
    // Handlers may read less than the body; skip to its end so a short or
    // malformed packet can't shift the stream (cp.cc:874-876).
    r.read = body_end;
    return result;
}

void SyncCommandProcessor::Interrupt(Reader& r) {
    // cp.cc:900-913: source 1 on each of the 6 hardware threads in the mask
    const uint32_t cpu_mask = r.Read();
    for (uint32_t n = 0; n < 6; n++) {
        if (cpu_mask & (1u << n)) {
            stats_.interrupts.add();
            if (hooks_.interrupt) hooks_.interrupt(1, n);
        }
    }
}

void SyncCommandProcessor::XeSwap(Reader& r, uint32_t count) {
    // cp.cc:915-941; unlike Xenia, keeps every word for the hook
    swap_body_.resize(count);
    for (uint32_t i = 0; i < count; i++) swap_body_[i] = r.Read();
    SwapPacket swap;
    swap.magic = count > 0 ? swap_body_[0] : 0;
    swap.frontbuffer_ptr = count > 1 ? swap_body_[1] : 0;
    swap.width = count > 2 ? swap_body_[2] : 0;
    swap.height = count > 3 ? swap_body_[3] : 0;
    swap.body = swap_body_;
    swap.index = stats_.swaps.get();
    if (swap.index == 0) {
        std::string words;
        for (uint32_t i = 0; i < count && i < 16; i++) words += std::format(" {:08X}", swap_body_[i]);
        {
            std::lock_guard lock(log_mutex_);
            first_swap_body_ = swap_body_;
        }
        Log(std::format("sync gpu: first XE_SWAP, {} dwords:{}{}", count, words,
                        count > 16 ? " ..." : ""));
    }
    if (hooks_.swap) hooks_.swap(swap);
    IncrementCounter();
    stats_.swaps.add();
}

void SyncCommandProcessor::IndirectBuffer(Reader& r) {
    // cp.cc:943-953; nesting limited here, unlike Xenia
    const uint32_t list_ptr = r.Read() & 0x1FFFFFFF;
    const uint32_t list_length = r.Read() & 0xFFFFF;
    if (indirect_depth_ >= 8) {
        LogLimited(stats_.bad_packets,
                   std::format("sync gpu: indirect buffer {:08X} nested too deep, skipped",
                               list_ptr));
        return;
    }
    ++indirect_depth_;
    ExecuteIndirectBuffer(list_ptr, list_length);
    --indirect_depth_;
}

bool SyncCommandProcessor::WaitRegMem(Reader& r) {
    // cp.cc:955-1040
    const uint32_t wait_info = r.Read();
    const uint32_t poll_reg_addr = r.Read();
    const uint32_t ref = r.Read();
    const uint32_t mask = r.Read();
    const uint32_t wait = r.Read();
    stats_.waits.add();

    const bool is_memory = (wait_info & 0x10) != 0;
    const uint8_t* mem = nullptr;
    if (is_memory) {
        mem = Translate(poll_reg_addr & ~uint32_t(3), 4);
        if (!mem) return true;
    } else if (poll_reg_addr >= reg::kCount) {
        // Xenia asserts, then reads past its register file
        stats_.out_of_range_registers.add();
        LogLimited(stats_.bad_packets,
                   std::format("sync gpu: WAIT_REG_MEM on register {:X}, past the file; skipped",
                               poll_reg_addr));
        return true;
    }
    const auto endianness = static_cast<Endian>(poll_reg_addr & 3);
    const int band = WaitBandOf(wait);

    bool stalled = false;
    for (;;) {
        uint32_t value;
        if (is_memory) {
            value = GpuSwap(LoadHost32Acquire(mem), endianness);
        } else {
            if (poll_reg_addr == reg::COHER_STATUS_HOST) MakeCoherent();
            value = LoadReg(poll_reg_addr);
        }
        if (Compare(wait_info, value & mask, ref)) break;
        if (!stalled) {
            stalled = true;
            std::lock_guard lock(wait_mutex_);
            wait_ = CurrentWait{true,          pm4::WAIT_REG_MEM, is_memory, poll_reg_addr, ref,
                                mask,          wait_info & 7,     wait,      value & mask, {}};
            wait_start_ = std::chrono::steady_clock::now();
        } else {
            std::lock_guard lock(wait_mutex_);
            wait_.last_value = value & mask;
        }
        // Xenia checks for a stop only after a long sleep
        if (!running()) break;
        stats_.polls_by_band[band].add();
        Wait(wait);
    }
    if (stalled) {
        std::chrono::steady_clock::time_point start;
        {
            std::lock_guard lock(wait_mutex_);
            wait_.active = false;
            start = wait_start_;
        }
        const auto ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count());
        stats_.stalled_waits.add();
        stats_.wait_ns_total.add(ns);
        stats_.wait_ns_max.raise_to(ns);
        stats_.stalled_by_band[band].add();
        stats_.wait_ns_by_band[band].add(ns);
        CountWaitValue(wait);
    }
    // a stop ends the buffer (Xenia's "short-circuited exit")
    return running();
}

void SyncCommandProcessor::CountWaitValue(uint32_t wait) {
    // CP thread only. Relaxed: a reader racing a new value may misread it
    // once.
    const uint64_t n = stats_.wait_value_count.get();
    for (uint64_t i = 0; i < n; i++) {
        if (stats_.wait_values[i].get() == wait) {
            stats_.stalled_by_value[i].add();
            return;
        }
    }
    if (n >= SyncCpStats::kWaitValues) return;
    stats_.wait_values[n].set(wait);
    stats_.stalled_by_value[n].add();
    stats_.wait_value_count.set(n + 1);
}

void SyncCommandProcessor::Wait(uint32_t wait) {
    if (hooks_.wait) {
        hooks_.wait(wait);
        return;
    }
    // cp.cc:1018-1035; SyncMemory is the acquire of the next poll's load
    if (wait >= 0x100 && sleep_in_waits_)
        std::this_thread::sleep_for(std::chrono::milliseconds(wait / 0x100));
    else
        std::this_thread::yield();
}

void SyncCommandProcessor::RegRmw(Reader& r) {
    // cp.cc:1042-1067: bit 31 ands with a register, bit 30 ors with one
    const uint32_t rmw_info = r.Read();
    const uint32_t and_mask = r.Read();
    const uint32_t or_mask = r.Read();
    uint32_t value = LoadReg(rmw_info & 0x1FFF);
    value &= ((rmw_info >> 31) & 1) ? LoadReg(and_mask & 0x1FFF) : and_mask;
    value |= ((rmw_info >> 30) & 1) ? LoadReg(or_mask & 0x1FFF) : or_mask;
    WriteRegister(rmw_info & 0x1FFF, value);
}

void SyncCommandProcessor::RegToMem(Reader& r) {
    // cp.cc:1069-1090
    const uint32_t reg_addr = r.Read();
    uint32_t mem_addr = r.Read();
    if (reg_addr >= reg::kCount) {
        stats_.out_of_range_registers.add();
        return;
    }
    const auto endianness = static_cast<Endian>(mem_addr & 3);
    mem_addr &= ~uint32_t(3);
    if (uint8_t* p = Translate(mem_addr, 4)) StoreHost32(p, GpuSwap(LoadReg(reg_addr), endianness));
}

void SyncCommandProcessor::MemWrite(Reader& r, uint32_t count) {
    // cp.cc:1092-1108; the address keeps its endian bits as it advances
    uint32_t write_addr = r.Read();
    for (uint32_t i = 0; i + 1 < count; i++) {
        const uint32_t write_data = r.Read();
        const auto endianness = static_cast<Endian>(write_addr & 3);
        if (uint8_t* p = Translate(write_addr & ~uint32_t(3), 4))
            StoreHost32(p, GpuSwap(write_data, endianness));
        write_addr += 4;
    }
}

void SyncCommandProcessor::CondWrite(Reader& r) {
    // cp.cc:1110-1175: bit 4 of wait_info polls memory, bit 8 writes it
    const uint32_t wait_info = r.Read();
    uint32_t poll_reg_addr = r.Read();
    const uint32_t ref = r.Read();
    const uint32_t mask = r.Read();
    uint32_t write_reg_addr = r.Read();
    uint32_t write_data = r.Read();
    uint32_t value;
    if (wait_info & 0x10) {
        const auto endianness = static_cast<Endian>(poll_reg_addr & 3);
        poll_reg_addr &= ~uint32_t(3);
        const uint8_t* p = Translate(poll_reg_addr, 4);
        if (!p) return;
        value = GpuSwap(LoadHost32(p), endianness);
    } else {
        if (poll_reg_addr >= reg::kCount) {
            stats_.out_of_range_registers.add();
            return;
        }
        value = LoadReg(poll_reg_addr);
    }
    if (!Compare(wait_info, value & mask, ref)) return;
    if (wait_info & 0x100) {
        const auto endianness = static_cast<Endian>(write_reg_addr & 3);
        write_reg_addr &= ~uint32_t(3);
        if (uint8_t* p = Translate(write_reg_addr, 4))
            StoreHost32(p, GpuSwap(write_data, endianness));
    } else {
        WriteRegister(write_reg_addr, write_data);
    }
}

void SyncCommandProcessor::EventWrite(Reader& r) {
    // cp.cc:1177-1192
    const uint32_t initiator = r.Read();
    WriteRegister(reg::VGT_EVENT_INITIATOR, initiator & 0x3F);
}

void SyncCommandProcessor::EventWriteShd(Reader& r) {
    // cp.cc:1194-1217: the fence; initiator bit 31 writes counter() instead
    const uint32_t initiator = r.Read();
    uint32_t address = r.Read();
    const uint32_t value = r.Read();
    WriteRegister(reg::VGT_EVENT_INITIATOR, initiator & 0x3F);
    const uint32_t data_value = ((initiator >> 31) & 1) ? counter() : value;
    const auto endianness = static_cast<Endian>(address & 3);
    address &= ~uint32_t(3);
    stats_.fences.add();
    if (uint8_t* p = Translate(address, 4)) {
        // release: earlier packets' writes are visible before the fence
        const uint32_t swapped = GpuSwap(data_value, endianness);
        if (reinterpret_cast<std::uintptr_t>(p) % 4 == 0) {
            StoreHost32Release(p, swapped);
        } else {
            std::atomic_thread_fence(std::memory_order_release);
            StoreHost32(p, swapped);
        }
    }
}

void SyncCommandProcessor::EventWriteExt(Reader& r) {
    // cp.cc:1219-1247: extents faked as the whole 8192x8192 surface: min/max
    // x, min/max y (8-pixel units), min/max z, as big-endian 16-bit words
    // regardless of endian bits (Xenia asserts 8in16)
    const uint32_t initiator = r.Read();
    uint32_t address = r.Read();
    WriteRegister(reg::VGT_EVENT_INITIATOR, initiator & 0x3F);
    address &= ~uint32_t(3);
    static constexpr uint16_t kExtents[] = {
        0 >> 3, kTexture2DCubeMaxWidthHeight >> 3, 0 >> 3, kTexture2DCubeMaxWidthHeight >> 3,
        0,      1,
    };
    uint8_t* p = Translate(address, sizeof(kExtents));
    if (!p) return;
    for (size_t i = 0; i < std::size(kExtents); i++) {
        p[i * 2] = uint8_t(kExtents[i] >> 8);
        p[i * 2 + 1] = uint8_t(kExtents[i]);
    }
}

void SyncCommandProcessor::EventWriteZpd(Reader& r) {
    // cp.cc:1249-1282: query begin or end. D3D marks an end by writing
    // 0xFFFFFEED (big-endian) to ZPass_A/B (older D3Ds: ZFail_A/B). Counts
    // are cleared either way; an end reports the fake count as passed and
    // total. xe_gpu_depth_sample_counts (xenos.h:1509-1521): 8 little-endian
    // words, Total_A/B, ZFail_A/B, ZPass_A/B, StencilFail_A/B.
    const uint32_t initiator = r.Read();
    WriteRegister(reg::VGT_EVENT_INITIATOR, initiator & 0x3F);
    const int32_t fake_sample_count = fake_sample_count_.load(std::memory_order_relaxed);
    if (fake_sample_count < 0) return;
    const uint32_t address = LoadReg(reg::RB_SAMPLE_COUNT_ADDR);
    uint8_t* counts = Translate(address, 32);
    if (!counts) return;
    const uint32_t kQueryFinished = std::byteswap(UINT32_C(0xFFFFFEED));
    auto field = [&](int i) { return LoadHost32(counts + i * 4); };
    const bool is_end_via_z_pass = field(4) == kQueryFinished && field(5) == kQueryFinished;
    const bool is_end_via_z_fail = field(2) == kQueryFinished && field(3) == kQueryFinished;
    // CAS so a concurrent SetQueryLog(false) can't make it underflow
    uint32_t left = query_log_left_.load(std::memory_order_relaxed);
    while (left && !query_log_left_.compare_exchange_weak(left, left - 1,
                                                          std::memory_order_relaxed)) {
    }
    if (left) {
        std::string words;
        for (int i = 0; i < 8; i++) words += std::format(" {:08X}", field(i));
        Log(std::format("sync gpu: query log: EVENT_WRITE_ZPD #{} (initiator {:02X}) at {:08X}, "
                        "little-endian words before:{}; {}",
                        stats_.sample_count_writes.get(), initiator & 0x3F, address, words,
                        is_end_via_z_pass   ? std::format("an end (via ZPass), {} samples written",
                                                          fake_sample_count)
                        : is_end_via_z_fail ? std::format("an end (via ZFail), {} samples written",
                                                          fake_sample_count)
                                            : std::string("a begin, cleared")));
    }
    std::memset(counts, 0, 32);
    if (is_end_via_z_pass || is_end_via_z_fail) {
        StoreHost32(counts + 4 * 4, uint32_t(fake_sample_count));  // ZPass_A
        StoreHost32(counts + 0 * 4, uint32_t(fake_sample_count));  // Total_A
    }
    stats_.sample_count_writes.add();
}

void SyncCommandProcessor::SetConstant(Reader& r, uint32_t count) {
    // cp.cc:1428-1463
    const uint32_t offset_type = r.Read();
    uint32_t index = offset_type & 0x7FF;
    uint32_t base;
    if (!ConstantBase((offset_type >> 16) & 0xFF, base)) return;  // body skipped
    index += base;
    for (uint32_t n = 0; n + 1 < count; n++, index++) WriteRegister(index, r.Read());
}

void SyncCommandProcessor::SetConstantRun(Reader& r, uint32_t count) {
    // cp.cc:1465-1475 and 1517-1526
    const uint32_t offset_type = r.Read();
    uint32_t index = offset_type & 0xFFFF;
    for (uint32_t n = 0; n + 1 < count; n++, index++) WriteRegister(index, r.Read());
}

void SyncCommandProcessor::LoadAluConstant(Reader& r) {
    // cp.cc:1477-1515
    const uint32_t address = r.Read() & 0x3FFFFFFF;
    const uint32_t offset_type = r.Read();
    const uint32_t size_dwords = r.Read() & 0xFFF;
    uint32_t base;
    if (!ConstantBase((offset_type >> 16) & 0xFF, base)) return;
    uint32_t index = (offset_type & 0x7FF) + base;
    for (uint32_t n = 0; n < size_dwords; n++, index++) {
        const uint8_t* p = Translate(address + n * 4, 4);
        if (!p) return;
        WriteRegister(index, LoadBE32(p));
    }
}

void SyncCommandProcessor::VizQuery(Reader& r) {
    // cp.cc:1601-1634: an end reports the query as visible
    const uint32_t dword0 = r.Read();
    const uint32_t id = dword0 & 0x3F;
    if (!(dword0 & 0x100)) {
        WriteRegister(reg::VGT_EVENT_INITIATOR, kEventVizQueryStart);
        return;
    }
    WriteRegister(reg::VGT_EVENT_INITIATOR, kEventVizQueryEnd);
    const uint32_t status = id < 32 ? reg::PA_SC_VIZ_QUERY_STATUS_0 : reg::PA_SC_VIZ_QUERY_STATUS_1;
    StoreReg(status, LoadReg(status) | uint32_t(1) << (id % 32));
}

void SyncCommandProcessor::MakeCoherent() {
    // cp.cc:486-524; no caches to flush
    if (LoadReg(reg::COHER_STATUS_HOST)) StoreReg(reg::COHER_STATUS_HOST, 0);
}

void SyncCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
    // cp.cc:331-484
    if (index >= reg::kCount) {
        stats_.out_of_range_registers.add();
        std::lock_guard lock(log_mutex_);
        if (!logged_out_of_range_) {
            logged_out_of_range_ = true;
            if (hooks_.log)
                hooks_.log(std::format("sync gpu: write of register {:X} past the file ({:08X})",
                                       index, value));
        }
        return;
    }
    stats_.register_writes.add();
    StoreReg(index, value);
    if (has_known_registers_ && !known_registers_[index]) NoteUnknownRegister(index, true, value);

    if (index >= reg::SCRATCH_REG0 && index <= reg::SCRATCH_REG7) {
        // scratch write-back, per SCRATCH_UMSK
        const uint32_t scratch_reg = index - reg::SCRATCH_REG0;
        if ((1u << scratch_reg) & LoadReg(reg::SCRATCH_UMSK)) {
            const uint32_t mem_addr = LoadReg(reg::SCRATCH_ADDR) + scratch_reg * 4;
            if (uint8_t* p = Translate(mem_addr, 4)) StoreBE32(p, value);
        }
        return;
    }
    switch (index) {
        case reg::COHER_STATUS_HOST:
            // dirty: the next WAIT_REG_MEM on it makes it coherent
            StoreReg(index, value | UINT32_C(0x80000000));
            break;
        case reg::DC_LUT_RW_INDEX:
        case reg::DC_LUT_SEQ_COLOR:
        case reg::DC_LUT_PWL_DATA:
        case reg::DC_LUT_30_COLOR: WriteGammaRegister(index, value); break;
        default: break;
    }
}

void SyncCommandProcessor::WriteGammaRegister(uint32_t index, uint32_t value) {
    // cp.cc:363-481. Xenia bumps DC_LUT_RW_INDEX through WriteRegister,
    // which also resets the component; done in place here, under the lock.
    std::lock_guard lock(gamma_mutex_);
    const uint32_t rw_index_reg = LoadReg(reg::DC_LUT_RW_INDEX);
    const uint32_t rw_index = rw_index_reg & 0xFF;
    const uint32_t write_enable_mask = LoadReg(reg::DC_LUT_WRITE_EN_MASK);
    switch (index) {
        case reg::DC_LUT_RW_INDEX:
            // M56 DC_LUT_SEQ_COLOR
            gamma_rw_component_ = 0;
            break;

        case reg::DC_LUT_SEQ_COLOR: {
            // components in R, G, B order; the enable mask is B, G, R. Bits
            // 0..5 of the 16-bit value are hardwired to zero.
            if (write_enable_mask & (UINT32_C(1) << (2 - gamma_rw_component_))) {
                static constexpr uint32_t kShifts[] = {kRedShift, kGreenShift, kBlueShift};
                SetField10(gamma_table_[rw_index], kShifts[gamma_rw_component_],
                           (value & 0xFFFF) >> 6);
            }
            if (++gamma_rw_component_ >= 3) {
                gamma_rw_component_ = 0;
                StoreReg(reg::DC_LUT_RW_INDEX, WithRwIndex(rw_index_reg, rw_index + 1));
            }
        } break;

        case reg::DC_LUT_PWL_DATA: {
            // likely R, G, B order as SEQ_COLOR; index bit 7 ignored; base
            // and delta keep bits 6..15
            const uint32_t pwl_index = rw_index & 0x7F;
            if (write_enable_mask & (UINT32_C(1) << (2 - gamma_rw_component_))) {
                const uint32_t base = value & 0xFFFF & ~UINT32_C(0x3F);
                const uint32_t delta = (value >> 16) & ~UINT32_C(0x3F);
                gamma_pwl_[pwl_index][gamma_rw_component_] = base | delta << 16;
            }
            if (++gamma_rw_component_ >= 3) {
                gamma_rw_component_ = 0;
                StoreReg(reg::DC_LUT_RW_INDEX,
                         WithRwIndex(rw_index_reg, (rw_index & ~UINT32_C(0x7F)) |
                                                       ((pwl_index + 1) & 0x7F)));
            }
        } break;

        case reg::DC_LUT_30_COLOR: {
            // a whole entry; mask bit 0 blue, 1 green, 2 red
            const uint32_t mask = write_enable_mask & 0b111;
            uint32_t& entry = gamma_table_[rw_index];
            if (mask & 0b001) SetField10(entry, kBlueShift, value >> kBlueShift);
            if (mask & 0b010) SetField10(entry, kGreenShift, value >> kGreenShift);
            if (mask & 0b100) SetField10(entry, kRedShift, value >> kRedShift);
            gamma_rw_component_ = 0;
            StoreReg(reg::DC_LUT_RW_INDEX, WithRwIndex(rw_index_reg, rw_index + 1));
        } break;
    }
}

uint32_t SyncCommandProcessor::ReadRegister(uint32_t index) const {
    return index < reg::kCount ? LoadReg(index) : 0;
}

void SyncCommandProcessor::MmioWrite(uint32_t addr, uint32_t value) {
    // gs.cc:207-223
    const uint32_t r = (addr & 0xFFFF) / 4;
    switch (r) {
        case reg::CP_RB_WPTR:
            StoreReg(r, value);
            UpdateWritePointer(value);
            break;
        case reg::D1GRPH_PRIMARY_SURFACE_ADDRESS: StoreReg(r, value); break;
        default: WriteRegister(r, value); break;
    }
}

uint32_t SyncCommandProcessor::MmioRead(uint32_t addr) {
    // gs.cc:181-205
    const uint32_t r = (addr & 0xFFFF) / 4;
    switch (r) {
        case reg::RB_EDRAM_TIMING: return 0x08100748;
        case reg::RB_BC_CONTROL: return 0x0000200E;
        case reg::D1MODE_V_COUNTER: return 0x000002D0;
        case reg::D1MODE_INTERRUPT_STATUS: return 1;  // vblank
        case reg::D1MODE_VIEWPORT_SIZE: return 0x050002D0;  // 1280x720
        default: break;
    }
    if (has_known_registers_ && !known_registers_[r]) NoteUnknownRegister(r, false, 0);
    return LoadReg(r);
}

void SyncCommandProcessor::SetKnownRegisters(std::span<const uint32_t> indices) {
    known_registers_.reset();
    for (uint32_t i : indices)
        if (i < reg::kCount) known_registers_[i] = true;
    has_known_registers_ = true;
}

void SyncCommandProcessor::NoteUnknownRegister(uint32_t index, bool write, uint32_t value) {
    (write ? stats_.unknown_register_writes : stats_.unknown_register_reads).add();
    std::lock_guard lock(log_mutex_);
    if (logged_registers_[index]) return;
    logged_registers_[index] = true;
    if (!hooks_.log) return;
    if (write)
        hooks_.log(std::format("sync gpu: write of unknown register {:04X} = {:08X}", index, value));
    else
        hooks_.log(std::format("sync gpu: read of unknown register {:04X}", index));
}

GammaRamp SyncCommandProcessor::DisplayGamma() const {
    GammaRamp g;
    {
        std::lock_guard lock(gamma_mutex_);
        std::memcpy(g.table, gamma_table_, sizeof(g.table));
        std::memcpy(g.pwl, gamma_pwl_, sizeof(g.pwl));
    }
    g.mode = (LoadReg(reg::DC_LUT_RW_MODE) & 1) ? GammaRamp::kPwl : GammaRamp::kTable;
    return g;
}

void SyncCommandProcessor::SetQueryLog(bool on) {
    query_log_left_.store(on ? kQueryLogLines : 0, std::memory_order_relaxed);
}

CurrentWait SyncCommandProcessor::current_wait() const {
    std::lock_guard lock(wait_mutex_);
    CurrentWait w = wait_;
    if (w.active) w.elapsed = std::chrono::steady_clock::now() - wait_start_;
    return w;
}

std::vector<uint32_t> SyncCommandProcessor::first_swap_body() const {
    std::lock_guard lock(log_mutex_);
    return first_swap_body_;
}

uint32_t SyncCommandProcessor::LoadReg(uint32_t index) const {
    // MMIO writes from guest threads while the CP thread polls
    return std::atomic_ref<uint32_t>(const_cast<uint32_t&>(regs_.values[index]))
        .load(std::memory_order_acquire);
}

void SyncCommandProcessor::StoreReg(uint32_t index, uint32_t value) {
    std::atomic_ref<uint32_t>(regs_.values[index]).store(value, std::memory_order_release);
}

uint8_t* SyncCommandProcessor::Translate(uint32_t address, uint32_t size) {
    uint8_t* p = memory_.TranslatePhysical(address, size);
    if (!p)
        LogLimited(stats_.bad_addresses,
                   std::format("sync gpu: a packet addressed {:08X} ({} bytes), not guest memory",
                               address, size));
    return p;
}

void SyncCommandProcessor::Log(const std::string& line) {
    if (hooks_.log) hooks_.log(line);
}

void SyncCommandProcessor::LogLimited(StatCounter& counter, const std::string& line) {
    // a broken stream would repeat every frame
    const uint64_t n = counter.get();
    counter.add();
    if (n < 8) Log(line);
}

}  // namespace band3::render::sync_gpu
