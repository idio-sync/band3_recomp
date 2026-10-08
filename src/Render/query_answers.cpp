#include "src/Render/query_answers.h"

#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

#include "generated/band3_init.h"
#include "src/Render/query_coverage.h"
#include "src/settings.h"

// See query_answers.h.
//
// D3DQuery_Issue (sub_828590C0; its only callers are
// DxRndOcclusionQueryMgr::OnBeginQuery and OnEndQuery): r3 the query, r4
// D3DISSUE_BEGIN (2) and/or D3DISSUE_END (1). Type at +4, 9 occlusion (as
// D3DQuery_GetData reads it, query_log.cpp).

extern "C" void __imp__rex_sub_828590C0(PPCContext& ctx, uint8_t* base);

namespace band3::render {
namespace {

constexpr uint32_t kIssueEnd = 1, kIssueBegin = 2;
constexpr uint32_t kQueryType = 4;
constexpr uint32_t kQueryTypeOcclusion = 9;
// TheRnd's screen size, the target DoPointTests draws into with the viewport
// off and RndFlare::CalcRect sizes the area rect by (scene_capture.cpp's
// kDrawModeHolder, kRnd_Width, kRnd_Height)
constexpr uint32_t kTheRndHolder = 0x82C76B68;
constexpr uint32_t kRnd_Width = 0x3c;
constexpr uint32_t kRnd_Height = 0x40;
// RB3 draws a point or a 4-vertex strip; more is someone else's draw
constexpr uint32_t kMaxVertices = 64;
constexpr uint32_t kMaxTargetSide = 16384;

// the query open on this thread, between its begin and end
struct OpenQuery {
    uint32_t query = 0;
    uint64_t pixels = 0;
    bool countable = true;
};
thread_local OpenQuery t_open;

// each query's count at its last end; a begin forgets it. RB3 has at most
// 512 queries (DxRndOcclusionQueryMgr's 256 x 2).
std::mutex g_mutex;
std::unordered_map<uint32_t, uint32_t> g_answers;
std::atomic<bool> g_logged{false};

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

void Store32(uint8_t* base, uint32_t addr, uint32_t value) {
    *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr) = value;
}

void Begin(uint32_t query) {
    {
        std::lock_guard lock(g_mutex);
        g_answers.erase(query);
    }
    t_open = {query, 0, true};
}

void End(uint32_t query) {
    const OpenQuery open = std::exchange(t_open, OpenQuery{});
    std::lock_guard lock(g_mutex);
    if (open.query == query && open.countable)
        g_answers[query] = uint32_t(std::min<uint64_t>(open.pixels, UINT32_MAX));
    else
        g_answers.erase(query);
}

}  // namespace

void NoteQueryIssue(uint8_t* base, uint32_t query, uint32_t flags) {
    if (!query || Load32(base, query + kQueryType) != kQueryTypeOcclusion) return;
    if (flags & kIssueBegin) Begin(query);
    if (flags & kIssueEnd) End(query);
}

bool QueryAnswersOn() { return REXCVAR_GET(native_query_sample_count) < 0; }

void NoteQueryDrawUp(uint8_t* base, uint32_t prim, uint32_t count, uint32_t data,
                     uint32_t stride) {
    OpenQuery& open = t_open;
    if (!open.query || !open.countable) return;
    const uint32_t rnd = Load32(base, kTheRndHolder);
    const uint32_t width = rnd ? Load32(base, rnd + kRnd_Width) : 0;
    const uint32_t height = rnd ? Load32(base, rnd + kRnd_Height) : 0;
    if (!data || stride < 8 || count > kMaxVertices || !width || !height ||
        width > kMaxTargetSide || height > kMaxTargetSide) {
        open.countable = false;
        return;
    }
    // x and y lead each vertex (DoPointTests' FVF 0x4042: x y z w, colour)
    QueryVertex v[kMaxVertices];
    for (uint32_t i = 0; i < count; i++) {
        v[i].x = std::bit_cast<float>(Load32(base, data + i * stride));
        v[i].y = std::bit_cast<float>(Load32(base, data + i * stride + 4));
    }
    const std::optional<uint64_t> pixels = DrawPixels(prim, {v, count}, width, height);
    if (pixels)
        open.pixels += *pixels;
    else
        open.countable = false;
}

void NoteQueryOtherDraw() {
    if (t_open.query) t_open.countable = false;
}

std::optional<uint32_t> ApplyQueryAnswer(uint8_t* base, uint32_t query, uint32_t data,
                                         uint32_t size, uint32_t hr) {
    if (hr != 0 || !query || !data || size < 4 || !QueryAnswersOn()) return std::nullopt;
    uint32_t answer;
    {
        std::lock_guard lock(g_mutex);
        const auto it = g_answers.find(query);
        if (it == g_answers.end()) return std::nullopt;
        answer = it->second;
    }
    const uint32_t gpu = Load32(base, data);
    Store32(base, data, answer);
    if (!g_logged.exchange(true))
        REXLOG_INFO("occlusion queries: answered with the pixels their draws cover, this one "
                    "{} (the GPU's {}); native_query_sample_count 0 or more for the GPU's",
                    answer, gpu);
    return gpu;
}

}  // namespace band3::render

// D3DQuery_Issue
extern "C" REX_FUNC(rex_sub_828590C0) {
    band3::render::NoteQueryIssue(base, ctx.r3.u32, ctx.r4.u32);
    __imp__rex_sub_828590C0(ctx, base);
}
