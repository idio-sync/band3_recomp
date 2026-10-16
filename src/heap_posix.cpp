// rex::memory::BaseHeap::AllocRange, in front of the SDK's on Linux
// (fast_heap_search).
//
// The SDK's (librexruntime.so, 0.10.0.15; Xenia's) finds a top-down range by
// stepping its base down a page-aligned stride at a time from the top of the
// range, past every page that's taken, and when a page inside a free-based
// candidate is taken, to that page less the range. The game's physical
// allocations (XPhysicalAlloc, through PhysicalHeap::AllocRange into its
// parent heap) search 512 MB of 4 KB pages that way, mostly full near the top:
// on a Steam Deck about 40 ms of the game thread in 28 s of a song, in bursts
// (0.4 ms of the frame a song starts in, its slowest).
//
// This finds the same place faster: a taken page's entry gives the first page
// of its region, all of which is taken, so the search moves below the region
// rather than through it, a stride or a page at a time. Every candidate it
// passes over overlaps a taken page, so the first free one it reaches is the
// SDK's, the highest. Each heap also keeps a mark, every page above which is
// taken: allocating only takes pages, Release (also overridden) raises the
// mark over a region before the SDK frees it, and Reset, Restore and Dispose
// forget it. A search starts below the mark, lowered past taken regions to
// the highest free page, so it walks only what's been allocated since. It then
// calls the SDK's own AllocRange with the range narrowed to that place, which
// allocates as it always does (host commit, page entries, counts); if that
// ever fails, with the caller's range. Bottom-up searches go to the SDK's as
// they are. fast_heap_search_check instead runs the SDK's search on the
// caller's range and logs any place that differs from this one's.
//
// The members are the SDK header's (their offsets match the library), but its
// page entries are 16 bytes, not the header's 8: the first qword has the
// region's first page in bits 0-19 and the state in bits 60-61.
//
// The library reaches AllocRange through vtables bound by symbol, so the
// executable's definition is the one they hold, as with memory_posix.cpp; the
// SDK's own is found with dlsym(RTLD_NEXT). Not on Windows.

#ifdef __linux__

#include <dlfcn.h>

#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "src/settings.h"

namespace rex::memory {

namespace {

using AllocRangeFn = bool (*)(BaseHeap*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                              uint32_t, bool, uint32_t*);

AllocRangeFn SdkAllocRange() {
    static const AllocRangeFn fn = reinterpret_cast<AllocRangeFn>(
        dlsym(RTLD_NEXT, "_ZN3rex6memory8BaseHeap10AllocRangeEjjjjjjbPj"));
    return fn;
}

constexpr size_t kEntryBytes = 16;
constexpr uint64_t kStateMask = 0x3000000000000000ull;
constexpr uint64_t kRegionBaseMask = 0xFFFFF;

std::atomic<uint64_t> g_checked{0}, g_mismatched{0};

template <typename Fn>
Fn Sdk(const char* name) {
    return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, name));
}

// Each heap's mark: every page above it is taken (-1: none free). Searches
// start below it rather than at the top, and lower it to the highest free
// page they find. Allocating only takes pages, so it holds; Release raises it
// over the region it frees before the SDK frees it, and Reset, Restore and
// Dispose forget it (the top again). A leaf lock: taken inside the heap's.
std::mutex g_marks_mutex;
std::unordered_map<const BaseHeap*, int64_t> g_marks;

int64_t GetMark(const BaseHeap* heap, int64_t top) {
    std::lock_guard lock(g_marks_mutex);
    const auto it = g_marks.find(heap);
    return it == g_marks.end() || it->second > top ? top : it->second;
}
// lowered to `mark` unless raised since it was `was`
void LowerMark(const BaseHeap* heap, int64_t was, int64_t mark, int64_t top) {
    std::lock_guard lock(g_marks_mutex);
    const auto it = g_marks.find(heap);
    const int64_t now = it == g_marks.end() ? top : it->second;
    if (now == was) g_marks[heap] = mark;
}
void RaiseMark(const BaseHeap* heap, int64_t page) {
    std::lock_guard lock(g_marks_mutex);
    const auto it = g_marks.find(heap);
    if (it != g_marks.end() && it->second < page) it->second = page;
}
void ForgetMark(const BaseHeap* heap) {
    std::lock_guard lock(g_marks_mutex);
    g_marks.erase(heap);
}

}  // namespace

bool BaseHeap::AllocRange(uint32_t low_address, uint32_t high_address, uint32_t size,
                          uint32_t alignment, uint32_t allocation_type, uint32_t protect,
                          bool top_down, uint32_t* out_address) {
    const AllocRangeFn sdk = SdkAllocRange();
    if (!sdk) return false;  // can't happen: the library defines it
    const bool fast = REXCVAR_GET(fast_heap_search);
    const bool check = REXCVAR_GET(fast_heap_search_check);
    if (!top_down || (!fast && !check))
        return sdk(this, low_address, high_address, size, alignment, allocation_type, protect,
                   top_down, out_address);

    // the SDK's arithmetic; anything it would refuse, it refuses itself
    const uint32_t align = alignment ? (alignment + page_size_ - 1) / page_size_ * page_size_
                                     : page_size_;
    const uint32_t pages =
        (size ? (size + page_size_ - 1) / page_size_ * page_size_ : page_size_) >>
        page_size_shift_;
    uint32_t low = (low_address + align - 1) & ~(align - 1);
    if (low < heap_base_) low = heap_base_;
    uint32_t high = (high_address + align - 1) & ~(align - 1);
    if (high >= heap_base_ + heap_size_ - 1) high = heap_base_ + heap_size_ - 1;
    const uint8_t* table = reinterpret_cast<const uint8_t*>(page_table_.data());
    const uint32_t entries = uint32_t(page_table_.size() * sizeof(PageEntry) / kEntryBytes);
    if (high < low || !entries || !pages)
        return sdk(this, low_address, high_address, size, alignment, allocation_type, protect,
                   top_down, out_address);
    uint32_t low_page = (low - heap_base_) >> page_size_shift_;
    uint32_t high_page = (high - heap_base_) >> page_size_shift_;
    if (low_page > entries - 1) low_page = entries - 1;
    if (high_page > entries - 1) high_page = entries - 1;
    const uint32_t stride = align >> page_size_shift_;
    if (high_page < low_page || pages > high_page - low_page + 1 || !stride)
        return sdk(this, low_address, high_address, size, alignment, allocation_type, protect,
                   top_down, out_address);

    // the SDK's locks, in its order; it takes them again (both recursive)
    auto reconcile = AcquireHostPageReconcileLock();
    std::lock_guard heap_lock(heap_mutex_);
    auto entry = [table](uint32_t page) {
        uint64_t v;
        std::memcpy(&v, table + size_t(page) * kEntryBytes, 8);
        return v;
    };
    // strides are powers of two (page-aligned alignments); the SDK divides
    const bool pow2 = (stride & (stride - 1)) == 0;
    auto align_down = [stride, pow2](int64_t page) {
        return pow2 ? page & ~int64_t(stride - 1) : page - page % stride;
    };

    // As the SDK's, a candidate whose base page is taken moves down, and one
    // with a taken page above its base moves below that page, the lowest such
    // (every candidate in between holds it); but each move goes below the
    // taken page's whole region rather than a stride or the page.
    // the highest free page: the mark, lowered past taken regions
    const int64_t was = GetMark(this, entries - 1);
    int64_t mark = was;
    while (mark >= 0 && (entry(uint32_t(mark)) & kStateMask)) {
        int64_t region = int64_t(entry(uint32_t(mark)) & kRegionBaseMask);
        if (region > mark) region = mark;
        mark = region - 1;
    }
    LowerMark(this, was, mark, entries - 1);

    int64_t found = -1;
    int64_t base = align_down(std::min<int64_t>(high_page, mark) - pages + 1);
    while (base >= low_page) {
        int64_t taken = -1;
        for (int64_t p = base; p < base + pages; p++) {
            if (entry(uint32_t(p)) & kStateMask) {
                taken = p;
                break;
            }
        }
        if (taken < 0) {
            found = base;
            break;
        }
        // the region's first page; all of it up to `taken` is taken (if the
        // entry says otherwise, just the page)
        int64_t region = int64_t(entry(uint32_t(taken)) & kRegionBaseMask);
        if (region > taken) region = taken;
        // the base page taken: the next candidate below the region; else
        // the highest that ends below it
        const int64_t next = taken == base ? region - 1 : region - pages;
        if (next < 0) break;
        base = align_down(next);
    }

    if (check) {
        uint32_t address = 0;
        const bool ok = sdk(this, low_address, high_address, size, alignment, allocation_type,
                            protect, top_down, &address);
        const uint32_t predicted =
            found >= 0 ? heap_base_ + (uint32_t(found) << page_size_shift_) : 0;
        const uint64_t n = ++g_checked;
        if ((ok ? address : 0) != predicted) {
            const uint64_t bad = ++g_mismatched;
            if (bad <= 20 || bad % 1000 == 0)
                REXLOG_WARN("fast_heap_search_check: heap {:08X} range {:08X}-{:08X} size {:X} "
                            "align {:X}: SDK {:08X} ({}), fast {:08X}; {} of {} differ",
                            heap_base_, low_address, high_address, size, alignment, address,
                            ok ? "ok" : "failed", predicted, bad, n);
        } else if (n % 10000 == 0) {
            REXLOG_INFO("fast_heap_search_check: {} searches, {} differ", n,
                        g_mismatched.load());
        }
        if (out_address) *out_address = ok ? address : 0;
        return ok;
    }

    if (found >= 0) {
        // the SDK's search, starting at the place found
        const uint32_t start = heap_base_ + (uint32_t(found) << page_size_shift_);
        const uint32_t end = start + ((pages - 1) << page_size_shift_);
        if (sdk(this, start, end, size, alignment, allocation_type, protect, true, out_address))
            return true;
    }
    return sdk(this, low_address, high_address, size, alignment, allocation_type, protect,
               top_down, out_address);
}

bool BaseHeap::Release(uint32_t base_address, uint32_t* out_region_size) {
    using Fn = bool (*)(BaseHeap*, uint32_t, uint32_t*);
    static const Fn sdk = Sdk<Fn>("_ZN3rex6memory8BaseHeap7ReleaseEjPj");
    // over the region it frees, as the SDK finds it, before it's free
    const uint32_t entries = uint32_t(page_table_.size() * sizeof(PageEntry) / kEntryBytes);
    const uint32_t page = (base_address - heap_base_) >> page_size_shift_;
    if (base_address >= heap_base_ && page < entries) {
        uint64_t e;
        std::memcpy(&e, reinterpret_cast<const uint8_t*>(page_table_.data()) +
                            size_t(page) * kEntryBytes, 8);
        const int64_t first = int64_t(e & kRegionBaseMask);
        const int64_t count = int64_t((e >> 32) & 0xFFFFF);
        const int64_t last = count ? first + count - 1 : int64_t(entries) - 1;
        RaiseMark(this, std::max<int64_t>(std::min<int64_t>(last, entries - 1), page));
    } else {
        ForgetMark(this);
    }
    return sdk(this, base_address, out_region_size);
}

void BaseHeap::Reset() {
    using Fn = void (*)(BaseHeap*);
    static const Fn sdk = Sdk<Fn>("_ZN3rex6memory8BaseHeap5ResetEv");
    ForgetMark(this);
    sdk(this);
}

bool BaseHeap::Restore(stream::ByteStream* stream) {
    using Fn = bool (*)(BaseHeap*, stream::ByteStream*);
    static const Fn sdk = Sdk<Fn>("_ZN3rex6memory8BaseHeap7RestoreEPNS_6stream10ByteStreamE");
    ForgetMark(this);
    return sdk(this, stream);
}

void BaseHeap::Dispose() {
    using Fn = void (*)(BaseHeap*);
    static const Fn sdk = Sdk<Fn>("_ZN3rex6memory8BaseHeap7DisposeEv");
    ForgetMark(this);
    sdk(this);
}

}  // namespace rex::memory

#endif
