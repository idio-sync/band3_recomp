#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include "src/Game/DataNode.h"

// Gives {elem $x $i} an int 0 when $x isn't an array, instead of crashing.
//
// DataGetElem ({elem $array $i}) takes its first argument's value as a
// DataArray* without checking its type, as the retail build does throughout.
// Rock Band 3 Deluxe's album art shuffles (world_objects.dta's music library
// shot and dx_funcs.dta's roll_splash_art) pick
// {elem {random_elem {find $syscfg missing_song_data}} 0}: random_elem can pick
// index 0, the list's own tag, the symbol missing_song_data, so elem reads that
// symbol's text as an array and the first four bytes, "miss", as its nodes.
// Guest 0x6D697373 isn't mapped here, so the game crashed: about one time in
// 80 that the music library showed outside a song, and one in 180 at the
// splash screen.
//
// So this does what DataGetElem does, and for a value that isn't an array (or
// a command or a property, which hold one) it returns the int 0, as a default
// DataNode, and warns once for each script line. Deluxe's shuffle then looks
// for album art named "0", doesn't find it, and picks again.

REX_EXTERN(DataNode__Int);
REX_EXTERN(DataNode__Evaluate);
REX_EXTERN(DataNode__ct_8274A9D0);

namespace {

// DataArray (src/Game/DataArray.h)
constexpr uint32_t kDataArray_Nodes = 0x0;
constexpr uint32_t kDataArray_File = 0x4;  // Symbol
constexpr uint32_t kDataArray_Line = 0xC;  // short

// DataGetElem's own frame, as the game's code makes it
constexpr uint32_t kFrame = 112;

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

void Store32(uint8_t* base, uint32_t addr, uint32_t value) {
    *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr) = value;
}

std::string_view GuestString(uint8_t* base, uint32_t addr) {
    if (!addr) return {};
    return rex::memory::GuestPtr<const char*>(base, addr);
}

bool HoldsArray(uint32_t type) {
    return type == band3::kDataArray || type == band3::kDataCommand || type == band3::kDataProperty;
}

// once for each {elem} command, by its file and line
void Warn(uint8_t* base, uint32_t command, uint32_t type, uint32_t value) {
    const std::string_view file = GuestString(base, Load32(base, command + kDataArray_File));
    const int line = static_cast<int16_t>(*rex::memory::GuestPtr<rex::be<uint16_t>*>(base, command + kDataArray_Line));
    std::string where = std::string(file.empty() ? "?" : file) + ":" + std::to_string(line);
    static std::mutex mutex;
    static std::set<std::string> warned;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!warned.insert(where).second) return;
    }
    if (type == band3::kDataSymbol || type == band3::kDataString) {
        REXLOG_WARN("Script {}: elem of \"{}\", which isn't an array; it gives 0 instead (warned once)",
                    where, GuestString(base, value));
    } else {
        REXLOG_WARN("Script {}: elem of a node of type {}, which isn't an array; it gives 0 instead (warned once)",
                    where, type);
    }
}

}  // namespace

// DataGetElem(DataNode* result, DataArray* command): {elem $array $i}
extern "C" REX_FUNC(DataGetElem) {
    const uint32_t result = ctx.r3.u32;
    const uint32_t command = ctx.r4.u32;
    const uint32_t nodes = Load32(base, command + kDataArray_Nodes);
    const uint32_t r1 = ctx.r1.u32;
    ctx.r1.u32 = r1 - kFrame;
    Store32(base, ctx.r1.u32, r1);

    // the index first, then the array, in the game's order
    ctx.r3.u64 = nodes + 2 * sizeof(band3::DataNode);
    DataNode__Int(ctx, base);
    const int32_t index = ctx.r3.s32;
    ctx.r3.u64 = nodes + 1 * sizeof(band3::DataNode);
    DataNode__Evaluate(ctx, base);
    const uint32_t evaluated = ctx.r3.u32;
    const uint32_t value = Load32(base, evaluated);
    const uint32_t type = Load32(base, evaluated + 4);

    if (HoldsArray(type) && value) {
        // DataNode(const DataNode&) from the array's node
        ctx.r3.u64 = result;
        ctx.r4.u64 = Load32(base, value + kDataArray_Nodes) + static_cast<uint32_t>(index) * sizeof(band3::DataNode);
        DataNode__ct_8274A9D0(ctx, base);
    } else {
        Warn(base, command, type, value);
        Store32(base, result, 0);
        Store32(base, result + 4, band3::kDataInt);
    }
    ctx.r1.u32 = r1;
    ctx.r3.u64 = result;
}
