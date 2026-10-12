#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <cstdint>
#include "src/Game/DataNode.h"
#include "src/Game/Symbol.h"

// Drops a script task that would read the Music Library's highlighted node
// after the library has let go of its song lists.
//
// MusicLibrary::GetHighlightedNode (script: {music_library get_highlighted_node})
// is mNodes[mCurrentHighlightIndex] of the current sort, unchecked. Leaving the
// Music Library for a song (MusicLibrary::OnUnload) frees every sort's nodes
// (SongSortMgr::ClearAllSorts), so a script task still pending then reads
// through a null list and the game crashes. Rock Band 3 Deluxe's Auto Endurance
// (song_select.dta endurance_handler) leaves exactly such a task: a chain of
// kTaskSeconds tasks, nothing cancelling them, whose last one, ~8 s after the
// song list shows, checks {{music_library get_highlighted_node} get_node_type}.
// A song picked any other way in those 8 s left it to run as the song loaded.
//
// So a ScriptTask whose script names get_highlighted_node, polled while the
// highlight isn't a node of the current sort, is deleted unrun, as the game
// deletes a finished one. Returning no node wouldn't do: the script calls
// get_node_type on what it gets.

extern "C" void __imp__ScriptTask__Poll(PPCContext& ctx, uint8_t* base);
REX_EXTERN(SongSortMgr__GetSort);

namespace {

// globals
constexpr uint32_t kTheMusicLibraryPtr = 0x82DFD3A8;  // MusicLibrary*
constexpr uint32_t kTheSongSortMgrPtr = 0x82DFEE5C;   // SongSortMgr*

// struct offsets
constexpr uint32_t kMusicLibrary_HighlightIx = 0xF0;  // mCurrentHighlightIndex
constexpr uint32_t kMusicLibrary_SortType = 0xFC;     // SongSortType of the list shown
constexpr uint32_t kNodeSort_First = 0x40;            // mNodes' begin
constexpr uint32_t kNodeSort_Last = 0x44;             // mNodes' end
constexpr uint32_t kScriptTask_Script = 0x44;         // DataArray* mScript
constexpr uint32_t kDataArray_Nodes = 0x0;
constexpr uint32_t kDataArray_Size = 0x8;             // short

// how far into a script to look, so a huge one can't stall the frame
constexpr int kMaxDepth = 16;
constexpr int kMaxNodes = 4096;

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

int16_t Load16(uint8_t* base, uint32_t addr) {
    return static_cast<int16_t>(*rex::memory::GuestPtr<rex::be<uint16_t>*>(base, addr));
}

// context for calling a guest function from a hook: a stack below the
// caller's frame, with its r13 (as band3::Symbol does)
PPCContext CallContext(const PPCContext& ctx, uint32_t reserve) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - reserve;
    call.r13 = ctx.r13;
    return call;
}

// whether {music_library get_highlighted_node} would read outside the
// current sort's nodes now
bool HighlightGone(PPCContext& ctx, uint8_t* base) {
    const uint32_t library = Load32(base, kTheMusicLibraryPtr);
    const uint32_t sort_mgr = Load32(base, kTheSongSortMgrPtr);
    if (!library || !sort_mgr) return false;

    // SongSortMgr::GetSort(SongSortMgr*, SongSortType) -> NodeSort*
    PPCContext call = CallContext(ctx, 0x400);
    call.r3.u64 = sort_mgr;
    call.r4.u64 = Load32(base, library + kMusicLibrary_SortType);
    SongSortMgr__GetSort(call, base);
    const uint32_t sort = call.r3.u32;
    if (!sort) return true;

    const uint32_t first = Load32(base, sort + kNodeSort_First);
    const uint32_t last = Load32(base, sort + kNodeSort_Last);
    const int32_t ix = static_cast<int32_t>(Load32(base, library + kMusicLibrary_HighlightIx));
    const int64_t count = first ? (static_cast<int64_t>(last) - first) / 4 : 0;
    return ix < 0 || ix >= count;
}

// whether the script array names symbol (a Symbol is its string's address),
// anywhere in its nested arrays and commands
bool Names(uint8_t* base, uint32_t array, uint32_t symbol, int depth, int& budget) {
    if (!array || depth > kMaxDepth) return false;
    const uint32_t nodes = Load32(base, array + kDataArray_Nodes);
    const int16_t size = Load16(base, array + kDataArray_Size);
    if (!nodes) return false;
    for (int16_t i = 0; i < size; ++i) {
        if (--budget < 0) return false;
        const uint32_t node = nodes + i * sizeof(band3::DataNode);
        const uint32_t value = Load32(base, node);
        switch (Load32(base, node + 4)) {
            case band3::kDataSymbol:
                if (value == symbol) return true;
                break;
            case band3::kDataArray:
            case band3::kDataCommand:
            case band3::kDataProperty:
                if (Names(base, value, symbol, depth + 1, budget)) return true;
                break;
            default:
                break;
        }
    }
    return false;
}

}  // namespace

// ScriptTask::Poll(ScriptTask*, float), run by the task timeline each frame
extern "C" REX_FUNC(ScriptTask__Poll) {
    const uint32_t task = ctx.r3.u32;
    const uint32_t script = task ? Load32(base, task + kScriptTask_Script) : 0;
    if (script && HighlightGone(ctx, base)) {
        const uint32_t symbol = band3::Symbol(ctx, base, "get_highlighted_node").value(base);
        int budget = kMaxNodes;
        if (symbol && Names(base, script, symbol, 0, budget)) {
            REXLOG_WARN("Dropped a script task that reads the Music Library's highlight after "
                        "the library let go of its songs (Deluxe's Auto Endurance does this "
                        "when a song is picked by hand)");
            // its scalar deleting destructor, vtable slot 0, as ScriptTask::Poll
            // deletes a finished task
            PPCContext call = CallContext(ctx, 0x400);
            call.r3.u64 = task;
            call.r4.u64 = 1;
            rex::runtime::ResolveIndirectFunction(Load32(base, Load32(base, task)))(call, base);
            return;
        }
    }
    __imp__ScriptTask__Poll(ctx, base);
}
