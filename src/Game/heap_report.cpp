#include "heap_report.h"
#include <algorithm>
#include <cmath>
#include <format>

namespace band3::heap_report {

namespace {

constexpr double kKB = 1024.0;
constexpr double kMB = 1024.0 * 1024.0;

// in MB from where it rounds to 1024 KB
bool InMB(uint64_t bytes) {
    return std::llround(bytes / kKB) >= 1024;
}

std::string Blocks(int64_t blocks) {
    return std::format("{} block{}", blocks, blocks == 1 ? "" : "s");
}

// what gives the heap more room, if band3 has a setting for it
const char* Setting(std::string_view heap) {
    if (heap == "main") return "main_heap_size";
    if (heap == "char") return "char_heap_size";
    return nullptr;
}

}

std::string Bytes(uint64_t bytes) {
    if (bytes < 1024) return std::format("{} bytes", bytes);
    if (!InMB(bytes)) return std::format("{} KB", std::llround(bytes / kKB));
    return std::format("{:.1f} MB", bytes / kMB);
}

std::string Describe(std::span<const HeapStats> heaps) {
    if (heaps.empty()) return "no heaps";
    std::string out;
    for (const auto& heap : heaps) {
        if (!out.empty()) out += "; ";
        // "1.9 of 100.1 MB", the unit once when both are in MB
        const std::string free = InMB(heap.free) && InMB(heap.size)
                                     ? std::format("{:.1f}", heap.free / kMB)
                                     : Bytes(heap.free);
        out += std::format("{} {} of {} free", heap.name, free, Bytes(heap.size));
        if (heap.blocks > 0) {
            out += std::format(" (the largest block {}, {})", Bytes(heap.largest),
                               Blocks(heap.blocks));
        }
    }
    return out;
}

int64_t HeapSize(std::string_view name, int64_t setting, int64_t mem_dta) {
    if (setting > 0) return std::min(setting, kMaxHeapSize);
    if (name == "main") return std::max(mem_dta, kMaxHeapSize);
    return mem_dta;
}

std::string AllocFailure(std::string_view heap, uint64_t wanted, int64_t left, int64_t right,
                         uint64_t largest, uint64_t free, uint64_t size) {
    std::string out = std::format("Out of memory: the game's {} heap", heap);
    if (size) out += std::format(" ({})", Bytes(size));
    out += std::format(" has no room for {}: {} free in {}, the largest {}. The game crashes next",
                       Bytes(wanted), Bytes(free), Blocks(left + 1 + right), Bytes(largest));
    if (size >= uint64_t(kMaxHeapSize)) {
        out += "; the heap is as big as a heap can be already";
    } else if (const char* setting = Setting(heap)) {
        out += std::format("; a bigger {} (Advanced, Memory) gives it more room", setting);
    }
    return out;
}

}
