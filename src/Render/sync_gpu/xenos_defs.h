/**
 * Parts of this file follow Xenia's src/xenia/gpu/xenos.h (GpuSwap) and
 * src/xenia/base/memory.h (load, store, store_and_swap):
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
#pragma once

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>

// Xenos constants for sync_cp.h, copied rather than included because the SDK's
// rex/graphics headers pull in the runtime and the unit tests build without
// the SDK. Line citations are to .rexglue-sdk/include/rex/graphics/.

namespace band3::render::sync_gpu {

// the host-order loads/stores below assume little-endian
static_assert(std::endian::native == std::endian::little);

// Register indices, in dwords (an MMIO address's (addr & 0xFFFF) / 4).
namespace reg {
// register_file.h:40 (RegisterFile::kRegisterCount)
inline constexpr uint32_t kCount = 0x5003;

inline constexpr uint32_t CP_RB_WPTR = 0x01C5;     // register_table.inc:3030
inline constexpr uint32_t SCRATCH_UMSK = 0x01DC;   // :44
inline constexpr uint32_t SCRATCH_ADDR = 0x01DD;   // :43
inline constexpr uint32_t SCRATCH_REG0 = 0x0578;   // :60
inline constexpr uint32_t SCRATCH_REG7 = 0x057F;   // :67
inline constexpr uint32_t COHER_STATUS_HOST = 0x0A31;         // :95
inline constexpr uint32_t PA_SC_VIZ_QUERY_STATUS_0 = 0x0C44;  // :99
inline constexpr uint32_t PA_SC_VIZ_QUERY_STATUS_1 = 0x0C45;  // :101
inline constexpr uint32_t D1GRPH_PRIMARY_SURFACE_ADDRESS = 0x1844;  // :295
inline constexpr uint32_t DC_LUT_RW_MODE = 0x1921;        // :427
inline constexpr uint32_t DC_LUT_RW_INDEX = 0x1922;       // :437
inline constexpr uint32_t DC_LUT_SEQ_COLOR = 0x1923;      // :444
inline constexpr uint32_t DC_LUT_PWL_DATA = 0x1924;       // :452
inline constexpr uint32_t DC_LUT_30_COLOR = 0x1925;       // :456
inline constexpr uint32_t DC_LUT_WRITE_EN_MASK = 0x1927;  // :465
inline constexpr uint32_t VGT_EVENT_INITIATOR = 0x21F9;   // :536
// aka RB_SAMPLE_COUNT_BASE: xe_gpu_depth_sample_counts, filled by
// EVENT_WRITE_ZPD
inline constexpr uint32_t RB_SAMPLE_COUNT_ADDR = 0x2325;  // :614

// Answered with constants by the MMIO read path (Xenia's
// GraphicsSystem::ReadRegister); not in register_table.inc.
inline constexpr uint32_t RB_EDRAM_TIMING = 0x0F00;
inline constexpr uint32_t RB_BC_CONTROL = 0x0F01;
inline constexpr uint32_t D1MODE_V_COUNTER = 0x194C;
inline constexpr uint32_t D1MODE_INTERRUPT_STATUS = 0x1951;
inline constexpr uint32_t D1MODE_VIEWPORT_SIZE = 0x1961;
}  // namespace reg

// Type-3 packet opcodes (xenos.h:1556-1617, enum Type3Opcode).
namespace pm4 {
inline constexpr uint32_t REG_RMW = 0x21;
inline constexpr uint32_t DRAW_INDX = 0x22;
inline constexpr uint32_t VIZ_QUERY = 0x23;
inline constexpr uint32_t WAIT_FOR_IDLE = 0x26;
inline constexpr uint32_t IM_LOAD = 0x27;
inline constexpr uint32_t IM_LOAD_IMMEDIATE = 0x2B;
inline constexpr uint32_t SET_CONSTANT = 0x2D;
inline constexpr uint32_t LOAD_ALU_CONSTANT = 0x2F;
inline constexpr uint32_t DRAW_INDX_BIN = 0x34;
inline constexpr uint32_t DRAW_INDX_2_BIN = 0x35;
inline constexpr uint32_t DRAW_INDX_2 = 0x36;
inline constexpr uint32_t INDIRECT_BUFFER_PFD = 0x37;
inline constexpr uint32_t INVALIDATE_STATE = 0x3B;
inline constexpr uint32_t WAIT_REG_MEM = 0x3C;
inline constexpr uint32_t MEM_WRITE = 0x3D;
inline constexpr uint32_t REG_TO_MEM = 0x3E;
inline constexpr uint32_t INDIRECT_BUFFER = 0x3F;
inline constexpr uint32_t NOP = 0x10;
inline constexpr uint32_t COND_WRITE = 0x45;
inline constexpr uint32_t EVENT_WRITE = 0x46;
inline constexpr uint32_t ME_INIT = 0x48;
inline constexpr uint32_t SET_BIN_BASE_OFFSET = 0x4B;
inline constexpr uint32_t SET_BIN_MASK = 0x50;
inline constexpr uint32_t SET_BIN_SELECT = 0x51;
inline constexpr uint32_t WAIT_REG_EQ = 0x52;
inline constexpr uint32_t WAIT_REG_GTE = 0x53;
inline constexpr uint32_t INTERRUPT = 0x54;
inline constexpr uint32_t SET_CONSTANT2 = 0x55;
inline constexpr uint32_t SET_SHADER_CONSTANTS = 0x56;
inline constexpr uint32_t EVENT_WRITE_SHD = 0x58;
inline constexpr uint32_t EVENT_WRITE_CFL = 0x59;
inline constexpr uint32_t EVENT_WRITE_EXT = 0x5A;
inline constexpr uint32_t EVENT_WRITE_ZPD = 0x5B;
inline constexpr uint32_t CONTEXT_UPDATE = 0x5E;
inline constexpr uint32_t SET_BIN_MASK_LO = 0x60;
inline constexpr uint32_t SET_BIN_MASK_HI = 0x61;
inline constexpr uint32_t SET_BIN_SELECT_LO = 0x62;
inline constexpr uint32_t SET_BIN_SELECT_HI = 0x63;
// Xenia/ReXGlue only: written by VdSwap
inline constexpr uint32_t XE_SWAP = 0x64;
}  // namespace pm4

// VGT_EVENT_INITIATOR's events VIZ_QUERY writes (xenos.h:1533-1534)
inline constexpr uint32_t kEventVizQueryStart = 7;
inline constexpr uint32_t kEventVizQueryEnd = 8;

// xenos.h:1130, kTexture2DCubeMaxWidthHeight: EVENT_WRITE_EXT's fake extents
inline constexpr uint32_t kTexture2DCubeMaxWidthHeight = 1 << 13;

// The low 2 bits of packet addresses (xenos.h:192).
enum class Endian : uint32_t { kNone = 0, k8in16 = 1, k8in32 = 2, k16in32 = 3 };

// xenos.h:1039, GpuSwap(uint32_t, Endian)
inline uint32_t GpuSwap(uint32_t value, Endian endianness) {
    switch (endianness) {
        default:
        case Endian::kNone: return value;
        case Endian::k8in16: return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);
        case Endian::k8in32: return std::byteswap(value);
        case Endian::k16in32: return ((value >> 16) & 0xFFFF) | (value << 16);
    }
}

// Xenia's xe::load / xe::store: host order, unaligned
inline uint32_t LoadHost32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}
inline void StoreHost32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, sizeof(v)); }

// xe::load_and_swap / store_and_swap: big-endian
inline uint32_t LoadBE32(const uint8_t* p) { return std::byteswap(LoadHost32(p)); }
inline void StoreBE32(uint8_t* p, uint32_t v) { StoreHost32(p, std::byteswap(v)); }

// For words other threads write (guest CPU, MMIO path); p must be 4-aligned
inline uint32_t LoadHost32Acquire(const uint8_t* p) {
    return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(p)))
        .load(std::memory_order_acquire);
}
inline void StoreHost32Release(uint8_t* p, uint32_t v) {
    std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(p)).store(v, std::memory_order_release);
}

}  // namespace band3::render::sync_gpu
