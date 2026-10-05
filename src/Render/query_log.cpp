#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>

#include "generated/band3_init.h"
#include "src/settings.h"

// native_query_log (Band3 → Debug): what the occlusion queries give the game,
// to check the sync-only GPU's EVENT_WRITE_ZPD answers against the emulated
// GPU's (both report native_query_sample_count / query_occlusion_fake_sample_count
// samples, 1000 by default, as passed). The other half, the sample counts the
// packets find, is the sync-only GPU's (SyncCommandProcessor::SetQueryLog).
//
// D3DQuery_GetData (0x82859348, band3_recomp.68.cpp): r3 the query, r4 where
// its data goes, r5 that buffer's size in bytes; nothing reads r6 (there are
// no flags). The query's type is at +4 (D3D9's numbering: 8 an event, 9 an
// occlusion query, 10 a timestamp), its "issued" bit is 0x40 of the byte at
// +20. An occlusion query writes its passed samples at r4 (with 16 bytes or
// more, three more words) and returns 0 (S_OK) with the result, 1 (S_FALSE)
// while its counts still hold D3D's 0xFFFFFEED marker (data 1), or
// 0x8876086A (D3DERR_NOTAVAILABLE) for a query never issued.

extern "C" void __imp__D3DQuery_GetData(PPCContext& ctx, uint8_t* base);

namespace {

// every read is logged up to kFirstReads, then one a second at most
constexpr uint64_t kFirstReads = 50;
constexpr int64_t kLogEveryNs = 1'000'000'000;

// the reads logged since native_query_log last turned on, and those since the
// last one logged that weren't
std::atomic<bool> g_was_on{false};
std::atomic<uint64_t> g_reads{0};
std::atomic<uint64_t> g_left_out{0};
std::atomic<int64_t> g_last_ns{0};

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const char* Result(uint32_t hr) {
    switch (hr) {
        case 0: return "S_OK";
        case 1: return "S_FALSE, not ready";
        case 0x8876086A: return "D3DERR_NOTAVAILABLE, not issued";
        default: return "?";
    }
}

// whether this read is logged (the first kFirstReads, then one a second)
bool Logged() {
    const uint64_t n = g_reads.fetch_add(1, std::memory_order_relaxed);
    if (n < kFirstReads) return true;
    const int64_t now = NowNs();
    int64_t last = g_last_ns.load(std::memory_order_relaxed);
    if (now - last >= kLogEveryNs &&
        g_last_ns.compare_exchange_strong(last, now, std::memory_order_relaxed))
        return true;
    g_left_out.fetch_add(1, std::memory_order_relaxed);
    return false;
}

}  // namespace

// The read itself, as the game made it; with native_query_log on, then
// logged. Off, it costs a load of the setting and of g_was_on.
extern "C" REX_FUNC(D3DQuery_GetData) {
    if (!REXCVAR_GET(native_query_log)) {
        if (g_was_on.load(std::memory_order_relaxed)) g_was_on.store(false, std::memory_order_relaxed);
        __imp__D3DQuery_GetData(ctx, base);
        return;
    }
    // turned on (again): the first reads are logged again
    if (!g_was_on.exchange(true, std::memory_order_relaxed)) {
        g_reads.store(0, std::memory_order_relaxed);
        g_left_out.store(0, std::memory_order_relaxed);
    }
    const uint32_t query = ctx.r3.u32;
    const uint32_t data = ctx.r4.u32;
    const uint32_t size = ctx.r5.u32;
    __imp__D3DQuery_GetData(ctx, base);
    if (!Logged()) return;
    const uint32_t hr = ctx.r3.u32;
    const uint64_t n = g_reads.load(std::memory_order_relaxed);
    std::string words;
    if (data) {
        for (uint32_t i = 0; i < std::min<uint32_t>(size / 4, 4); i++)
            words += std::format(" {}", Load32(base, data + i * 4));
    }
    std::string line = std::format(
        "query log: D3DQuery_GetData #{}: query {:08X} (type {}), {} bytes: returned {:08X} "
        "({}), data{}",
        n, query, query ? Load32(base, query + 4) : 0, size, hr, Result(hr),
        words.empty() ? " none" : words);
    if (const uint64_t left_out = g_left_out.exchange(0, std::memory_order_relaxed))
        line += std::format(" ({} reads not logged since the last)", left_out);
    REXLOG_INFO("{}", line);
}
