// Checks the lines band3 logs about the game's heaps (src/Game/heap_report.h):
// how full each is at a song's start and end, and what ran out when an
// allocation fails.

#include <doctest/doctest.h>
#include <vector>
#include "src/Game/heap_report.h"

using band3::heap_report::AllocFailure;
using band3::heap_report::Bytes;
using band3::heap_report::Describe;
using band3::heap_report::HeapStats;

TEST_CASE("sizes in bytes, KB or MB") {
    CHECK(Bytes(0) == "0 bytes");
    CHECK(Bytes(1023) == "1023 bytes");
    CHECK(Bytes(1024) == "1 KB");
    CHECK(Bytes(293916) == "287 KB");
    CHECK(Bytes(702632) == "686 KB");
    CHECK(Bytes(1048575) == "1.0 MB");
    CHECK(Bytes(1048576) == "1.0 MB");
    CHECK(Bytes(2006388) == "1.9 MB");
    CHECK(Bytes(105000000) == "100.1 MB");
}

TEST_CASE("each heap's free space, largest block and blocks") {
    const std::vector<HeapStats> heaps = {
        {"main", 113000000, 2006388, 293916, 358},
        {"char", 32000000, 5242880, 4194304, 3},
    };
    CHECK(Describe(heaps) ==
          "main 1.9 of 107.8 MB free (the largest block 287 KB, 358 blocks); "
          "char 5.0 of 30.5 MB free (the largest block 4.0 MB, 3 blocks)");
}

TEST_CASE("a heap with a single block, or none") {
    const std::vector<HeapStats> one = {{"main", 1048576, 1024, 1024, 1}};
    CHECK(Describe(one) == "main 1 KB of 1.0 MB free (the largest block 1 KB, 1 block)");
    const std::vector<HeapStats> full = {{"main", 1048576, 0, 0, 0}};
    CHECK(Describe(full) == "main 0 bytes of 1.0 MB free");
    CHECK(Describe({}) == "no heaps");
}

TEST_CASE("an allocation failure in the main heap, as in the crash it explains") {
    // MemHeap::Alloc's figures from crash-20261008-210152-11400.dmp, in Rock
    // Band 3 Deluxe's main heap
    CHECK(AllocFailure("main", 0xAB8A8, 342, 15, 293916, 2006388, 113000000) ==
          "Out of memory: the game's main heap (107.8 MB) has no room for 686 KB: "
          "1.9 MB free in 358 blocks, the largest 287 KB. The game crashes next; "
          "a bigger main_heap_size (Advanced, Memory) gives it more room");
}

TEST_CASE("the char heap's own setting, and none for other heaps") {
    // Heap::FreeBlockStats gives the largest block's index as -1 when it has none
    CHECK(AllocFailure("char", 4096, -1, 0, 0, 0, 32000000) ==
          "Out of memory: the game's char heap (30.5 MB) has no room for 4 KB: "
          "0 bytes free in 0 blocks, the largest 0 bytes. The game crashes next; "
          "a bigger char_heap_size (Advanced, Memory) gives it more room");
    CHECK(AllocFailure("fast", 4096, 1, 0, 1024, 2048, 0) ==
          "Out of memory: the game's fast heap has no room for 4 KB: "
          "2 KB free in 2 blocks, the largest 1 KB. The game crashes next");
}
