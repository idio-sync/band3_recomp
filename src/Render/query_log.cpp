#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

#include "generated/band3_init.h"
#include "src/Render/query_answers.h"
#include "src/settings.h"

// native_query_log: logs what occlusion queries return to the game, to check
// the sync-only GPU's EVENT_WRITE_ZPD answers against the emulated GPU's, and
// band3's own (query_answers.h, applied here first). The packet side is
// SyncCommandProcessor::SetQueryLog.
//
// D3DQuery_GetData (0x82859348): r3 the query, r4 the data buffer, r5 its
// size in bytes; r6 is unused. Type at +4 (D3D9: 8 event, 9 occlusion,
// 10 timestamp); "issued" is bit 0x40 of the byte at +20. Occlusion writes
// passed samples at r4 (three more words if >= 16 bytes) and returns S_OK,
// S_FALSE while the counts hold D3D's 0xFFFFFEED marker, or 0x8876086A
// (D3DERR_NOTAVAILABLE) if never issued.

extern "C" void __imp__D3DQuery_GetData(PPCContext& ctx, uint8_t* base);

namespace {

// every read is logged up to kFirstReads, then one a second at most
constexpr uint64_t kFirstReads = 50;
constexpr int64_t kLogEveryNs = 1'000'000'000;

// reads since native_query_log turned on, and those not logged since the last
// logged one
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

extern "C" REX_FUNC(D3DQuery_GetData) {
    const uint32_t query = ctx.r3.u32;
    const uint32_t data = ctx.r4.u32;
    const uint32_t size = ctx.r5.u32;
    __imp__D3DQuery_GetData(ctx, base);
    // band3's count over the GPU's (query_answers.h)
    const std::optional<uint32_t> gpu =
        band3::render::ApplyQueryAnswer(base, query, data, size, ctx.r3.u32);
    if (!REXCVAR_GET(native_query_log)) {
        if (g_was_on.load(std::memory_order_relaxed)) g_was_on.store(false, std::memory_order_relaxed);
        return;
    }
    // turned on again: restart the first-reads window
    if (!g_was_on.exchange(true, std::memory_order_relaxed)) {
        g_reads.store(0, std::memory_order_relaxed);
        g_left_out.store(0, std::memory_order_relaxed);
    }
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
    if (gpu) line += std::format(" (band3's count; the GPU's {})", *gpu);
    if (const uint64_t left_out = g_left_out.exchange(0, std::memory_order_relaxed))
        line += std::format(" ({} reads not logged since the last)", left_out);
    REXLOG_INFO("{}", line);
}
