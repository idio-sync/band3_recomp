#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// The lines band3 logs about the game's heaps (src/Hooks/heap_report.cpp): how
// full each is as a song starts and ends, so a log shows whether a heap runs
// low over a session, and, when the game can't allocate, what ran out and the
// setting that gives it more room. RB3 itself carries on with the null block it
// got and crashes reading it.

namespace band3::heap_report {

struct HeapStats {
    std::string name;
    uint64_t size = 0;     // bytes
    uint64_t free = 0;     // bytes, in all its free blocks
    uint64_t largest = 0;  // the largest free block, bytes
    int64_t blocks = 0;    // free blocks
};

// "287 KB", "1.9 MB": bytes, KB or MB (1024s), whichever reads best
std::string Bytes(uint64_t bytes);

// each heap's free space, its largest free block and how many blocks it's in
std::string Describe(std::span<const HeapStats> heaps);

// MemHeap::Alloc's failure, from the figures its own message has
// (Heap::FreeBlockStats'): `left` the free blocks before the largest (-1 when
// there are none), `right` those after it. `size` is the heap's, 0 if unknown.
std::string AllocFailure(std::string_view heap, uint64_t wanted, int64_t left, int64_t right,
                         uint64_t largest, uint64_t free, uint64_t size);

}
