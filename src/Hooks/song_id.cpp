#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include "generated/band3_init.h"
#include "src/Game/DataArray.h"
#include "src/Game/DataNode.h"
#include "src/Game/Symbol.h"
#include "src/Game/song_id.h"

// A custom song may give song_id as text. RB3 reads the node as an int and
// gets the text's address, so every such song gets a meaningless ID. RB3Enhanced
// turns the text into a number (crc32 of it); so does band3, to agree with it.
// SongMetadata's constructor is corrected in DataNode__Int (src/Game/DataNode.cpp);
// GetSongID, which looks song_id up in a song's data array, is corrected here.

extern "C" void __imp__GetSongID(PPCContext& ctx, uint8_t* base);

namespace band3 {

void LogSongIdCorrection(std::string_view text, int32_t id) {
    static std::mutex mutex;
    static std::set<std::string, std::less<>> seen;
    {
        std::lock_guard lock(mutex);
        if (!seen.emplace(text).second)
            return;
    }
    REXLOG_INFO("song_id: text song ID \"{}\" is {}", text, id);
}

}  // namespace band3

namespace {

// the node's text if it's a symbol or a string, else null
const char* NodeText(uint8_t* base, const band3::DataNode* n) {
    if (n->type == band3::kDataSymbol)
        return reinterpret_cast<const char*>(REX_RAW_ADDR(n->value));
    if (n->type == band3::kDataString) {
        auto* arr = reinterpret_cast<const band3::DataArray*>(REX_RAW_ADDR(n->value));
        return reinterpret_cast<const char*>(REX_RAW_ADDR(arr->mNodes));
    }
    return nullptr;
}

// the value node of the (song_id ...) child in an array, or null
const band3::DataNode* FindSongId(uint8_t* base, uint32_t array_addr, uint32_t sym) {
    auto* arr = reinterpret_cast<const band3::DataArray*>(REX_RAW_ADDR(array_addr));
    auto* nodes = reinterpret_cast<const band3::DataNode*>(REX_RAW_ADDR(arr->mNodes));
    const int size = static_cast<short>(arr->mSize);
    for (int i = 0; i < size; ++i) {
        if (nodes[i].type != band3::kDataArray)
            continue;
        auto* child = reinterpret_cast<const band3::DataArray*>(REX_RAW_ADDR(nodes[i].value));
        if (static_cast<short>(child->mSize) < 2)
            continue;
        auto* cn = reinterpret_cast<const band3::DataNode*>(REX_RAW_ADDR(child->mNodes));
        if (cn[0].type == band3::kDataSymbol && cn[0].value == sym)
            return &cn[1];
    }
    return nullptr;
}

}  // namespace

// int GetSongID(const DataArray* song, const DataArray* missing_data)
extern "C" REX_FUNC(GetSongID) {
    const uint32_t song = ctx.r3.u32;
    const uint32_t missing = ctx.r4.u32;
    __imp__GetSongID(ctx, base);

    static const uint32_t sym = band3::Symbol(ctx, base, "song_id").value(base);
    if (!sym)
        return;

    // same order as the original: song, then missing_data if song had none
    const band3::DataNode* found = FindSongId(base, song, sym);
    if (!found && missing)
        found = FindSongId(base, missing, sym);
    if (!found)
        return;
    const char* text = NodeText(base, found);
    if (!text)
        return;
    const int32_t id = band3::CorrectedSongId(text);
    band3::LogSongIdCorrection(text, id);
    ctx.r3.u64 = static_cast<uint64_t>(static_cast<int64_t>(id));
}
