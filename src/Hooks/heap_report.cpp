#include "heap_report.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>
#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>
#include "src/Game/heap_report.h"

// The game's heaps are RB3's MemHeap ones, a table of Heap structs sized by
// mem.dta (and main_heap_size/char_heap_size, src/Hooks/memory.cpp). Addresses
// are TU5's, from MemNumHeaps, MemHeapName, MemHeapSize and MemHeap::Alloc in
// generated/.

extern "C" void __imp__MakeString_char_Ptr_int_int_int_int_int(PPCContext& ctx, uint8_t* base);
REX_EXTERN(MemFreeBlockStats);

namespace {

constexpr uint32_t kNumHeaps = 0x82E06DFC;  // int, MemNumHeaps'
constexpr uint32_t kHeaps = 0x82E06BA8;     // Heap[]
constexpr uint32_t kHeapStride = 36;
constexpr uint32_t kHeap_Name = 0x8;        // const char*
constexpr uint32_t kHeap_SizeWords = 0xC;   // int, in 4-byte words
constexpr int32_t kMaxHeaps = 16;

// MemHeap::Alloc's message when no free block is big enough:
// "Allocation failure, heap \"%s\", want %d bytes\n   lFrags=  %8d\n   rFrags=
// %8d\n   Biggest Block=%8d\n   Free Bytes=   %8d\n"
constexpr uint32_t kAllocFailureFormat = 0x82117460;

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

const char* GuestStr(uint8_t* base, uint32_t addr) {
    return addr ? rex::memory::GuestPtr<const char*>(base, addr) : "";
}

PPCContext CallContext(const PPCContext& ctx, uint32_t reserve) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - reserve;
    call.r13 = ctx.r13;
    return call;
}

uint64_t HeapSize(uint8_t* base, int32_t heap) {
    return uint64_t(Load32(base, kHeaps + heap * kHeapStride + kHeap_SizeWords)) * 4;
}

int32_t NumHeaps(uint8_t* base) {
    return std::clamp(static_cast<int32_t>(Load32(base, kNumHeaps)), 0, kMaxHeaps);
}

}

namespace band3::heap_report {

void LogHeaps(PPCContext& ctx, uint8_t* base, const char* when) {
    std::vector<HeapStats> heaps;
    // MemFreeBlockStats(int heap, int& largest_index, int& after, int& free,
    // int& largest), under the heaps' lock; its answers below this frame
    const uint32_t out = ctx.r1.u32 - 0x40;
    for (int32_t i = 0; i < NumHeaps(base); i++) {
        PPCContext call = CallContext(ctx, 0x100);
        call.r3.u64 = static_cast<uint32_t>(i);
        call.r4.u64 = out;
        call.r5.u64 = out + 4;
        call.r6.u64 = out + 8;
        call.r7.u64 = out + 12;
        MemFreeBlockStats(call, base);
        const auto index = static_cast<int32_t>(Load32(base, out));
        const auto after = static_cast<int32_t>(Load32(base, out + 4));
        heaps.push_back({
            .name = GuestStr(base, Load32(base, kHeaps + i * kHeapStride + kHeap_Name)),
            .size = HeapSize(base, i),
            .free = Load32(base, out + 8),
            .largest = Load32(base, out + 12),
            .blocks = int64_t(index) + 1 + after,
        });
    }
    REXLOG_INFO("Memory ({}): {}", when, Describe(heaps));
}

}

// MakeString(const char*, ...) with five ints: MemHeap::Alloc's failure message
// says which heap ran out and how full it is. RB3 then goes on with the null
// block it got and crashes reading it, so it's said plainly first.
extern "C" REX_FUNC(MakeString_char_Ptr_int_int_int_int_int)
{
    if (ctx.r3.u32 == kAllocFailureFormat) {
        const std::string_view name = GuestStr(base, ctx.r4.u32);
        uint64_t size = 0;
        for (int32_t i = 0; i < NumHeaps(base); i++) {
            if (Load32(base, kHeaps + i * kHeapStride + kHeap_Name) == ctx.r4.u32) {
                size = HeapSize(base, i);
                break;
            }
        }
        REXLOG_ERROR("{}", band3::heap_report::AllocFailure(
                               name, ctx.r5.u32, ctx.r6.s32, ctx.r7.s32, ctx.r8.u32,
                               ctx.r9.u32, size));
    }
    __imp__MakeString_char_Ptr_int_int_int_int_int(ctx, base);
}
