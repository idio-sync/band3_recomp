// Keeps a few UI resources loaded for the whole session, as the game keeps
// its others (preload_song_start_resources).
//
// At boot the game loads its shared UI resources (most fonts, the track's
// meshes, the overshell lists, ...) into ObjDirPtrs that live in the
// executable's data and never let go of them, so a label that points at one
// later finds it loaded: ObjDirPtr<ObjectDir>::LoadFile with share on returns
// in about 0.01 ms. ui/resource/fonts/instruments_icons_rings.milo isn't among
// them. A menu loads it and lets it go as it closes, and as a song starts
// (GamePanel::StartGame's TrackPanel scripts setting the HUD labels' alt font)
// it's read again: 2.3 ms of the game thread on a Steam Deck, with its texture
// tiled, in the frame that is already the song's slowest (11 ms over a normal
// one), which then missed its paint by 0.5 to 1.8 ms at 1080p.
//
// So once the boot preload has made its first such call, this makes one more
// per file in kResources, with the same arguments, into an ObjDirPtr of its
// own in guest memory that nothing releases. The ObjDirPtr starts as a copy of
// the game's (its vtable, no dir or loader yet) and the FilePath as
// a copy of the game's String {vtable, length, char* buf} pointing at our
// text.

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>

#include <cstdint>
#include <cstring>
#include <string_view>

#include "src/settings.h"

extern "C" void __imp__ObjDirPtr_ObjectDir_LoadFile(PPCContext& ctx, uint8_t* base);

namespace {

constexpr const char* kResources[] = {"ui/resource/fonts/instruments_icons_rings.milo"};

// ObjDirPtr<ObjectDir> (ObjRef): {vtable, dir, loader}; the boot preload's
// are 12 bytes apart
constexpr uint32_t kObjDirPtrSize = 0xC;
constexpr uint32_t kObjDirPtr_Dir = 0x4, kObjDirPtr_Loader = 0x8;
// String: {vtable, length, char* buf}
constexpr uint32_t kStringSize = 0xC;
constexpr uint32_t kString_Length = 0x4, kString_Buf = 0x8;
// the executable's image, where the boot preload's ObjDirPtrs are
constexpr uint32_t kImageBegin = 0x82000000, kImageEnd = 0x84000000;

uint32_t Load32(const uint8_t* base, uint32_t addr) {
    uint32_t v;
    std::memcpy(&v, base + addr, 4);
    return __builtin_bswap32(v);
}
void Store32(uint8_t* base, uint32_t addr, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(base + addr, &v, 4);
}

bool g_done = false;  // tried, game thread only

// the String's text, if it fits `max` bytes
std::string_view GuestText(const uint8_t* base, uint32_t str, size_t max = 256) {
    const uint32_t buf = str ? Load32(base, str + kString_Buf) : 0;
    if (!buf) return {};
    const char* text = reinterpret_cast<const char*>(base + buf);
    return std::string_view(text, strnlen(text, max));
}

// After the game's own LoadFile call `game` returned (`self` its ObjDirPtr
// as it was before, `path` its FilePath): the same call for each resource.
void Preload(PPCContext& ctx, uint8_t* base, const uint8_t (&self)[kObjDirPtrSize],
             uint32_t path, const PPCContext& game) {
    auto* memory = rex::system::kernel_memory();
    for (const char* resource : kResources) {
        const uint32_t len = uint32_t(std::strlen(resource));
        const uint32_t ptr = memory->SystemHeapAlloc(kObjDirPtrSize, 4);
        const uint32_t fp = memory->SystemHeapAlloc(kStringSize, 4);
        const uint32_t text = memory->SystemHeapAlloc(len + 1, 4);
        if (!ptr || !fp || !text) {
            REXLOG_WARN("resource preload: no guest memory for {}", resource);
            return;
        }
        std::memcpy(base + ptr, self, kObjDirPtrSize);
        std::memcpy(base + fp, base + path, kStringSize);
        std::memcpy(base + text, resource, len + 1);
        Store32(base, fp + kString_Length, len);
        Store32(base, fp + kString_Buf, text);
        ctx.r3.u64 = ptr;
        ctx.r4.u64 = fp;
        ctx.r5.u64 = game.r5.u64;
        ctx.r6.u64 = game.r6.u64;
        ctx.r7.u64 = game.r7.u64;
        ctx.r8.u64 = game.r8.u64;
        __imp__ObjDirPtr_ObjectDir_LoadFile(ctx, base);
        const uint32_t dir = Load32(base, ptr + kObjDirPtr_Dir);
        if (dir) REXLOG_INFO("resource preload: {} kept loaded", resource);
        else REXLOG_WARN("resource preload: {} didn't load", resource);
    }
}

}  // namespace

extern "C" REX_FUNC(ObjDirPtr_ObjectDir_LoadFile) {
    const bool want = !g_done && REXCVAR_GET(preload_song_start_resources);
    const uint32_t self = ctx.r3.u32, path = ctx.r4.u32;
    // the boot preload's: a fresh ObjDirPtr of the executable's loading a
    // ui/resource file
    bool boot = false;
    uint8_t before[kObjDirPtrSize] = {};
    if (want && self >= kImageBegin && self < kImageEnd &&
        GuestText(base, path).starts_with("ui/resource/")) {
        std::memcpy(before, base + self, kObjDirPtrSize);
        boot = !Load32(base, self + kObjDirPtr_Dir) && !Load32(base, self + kObjDirPtr_Loader);
    }
    const PPCContext game = ctx;
    __imp__ObjDirPtr_ObjectDir_LoadFile(ctx, base);
    if (!boot) return;
    g_done = true;
    // ours runs as more of the same call; the game's caller sees the
    // registers its own call left
    const PPCContext after = ctx;
    Preload(ctx, base, before, path, game);
    ctx = after;
}
