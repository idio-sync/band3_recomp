#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "src/game_writes.h"
#include "src/Hooks/loose_name.h"
#include "src/settings.h"

// RB3Enhanced's game origin icons (source/SetlistHooks.c): the Music Library
// fills a song row's game_origin_icon slot with an icon for the game or pack the
// song came from (its songs.dta game_origin), ui/resource/game_origins/<origin>.png.
// band3 ships neither: Rock Band 3 Deluxe has the icons, and the slot is in
// RB3E's edit of the song list (ui/resource/list/gen/
// list_song_select_browser.milo_xbox), read as a loose file. Without both,
// nothing changes.
//
// As on RB3E, the origins are gathered as songs load, each one with an icon gets
// a material when the Music Library is entered, and they're freed when it
// unloads. The textures are loaded as RB3E does, through the game's DynamicTex,
// but freed by its own destructor, and the material takes its texture through
// the game's ObjPtr setter. Addresses are RB3E's Xbox 360 TU5 ones
// (include/ports_xbox360.h), which match this project's function map; layouts
// are its include/rb3 headers', checked against the RB3 decomp.

extern "C" void __imp__MusicLibrary__Mat(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__MusicLibrary__OnEnter(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__MusicLibrary__OnUnload(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__SongMetadata____ct(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__SongMetadata__Load(PPCContext& ctx, uint8_t* base);
REX_EXTERN(SongSortMgr__GetSort);
REX_EXTERN(MusicLibrary__GetNodeByIndex);
REX_EXTERN(FileExists);
REX_EXTERN(operator_new);
REX_EXTERN(DynamicTex____ct);
REX_EXTERN(DynamicTex__sdtor);
REX_EXTERN(RndTex__SetBitmap3);
REX_EXTERN(ObjRefConcrete_Hmx_Object_ObjectDir__SetObjConcrete);

namespace {

constexpr uint32_t kTheSongSortMgrPtr = 0x82DFEE5C;  // SongSortMgr*

// struct offsets
constexpr uint32_t kMusicLibrary_SortType = 0xFC;     // SongSortType of the list shown
constexpr uint32_t kUIListSlot_MatchNameBuf = 0x6C;   // String mMatchName's buffer
constexpr uint32_t kSortNode_Record = 0x40;           // a song node's SongRecord*
constexpr uint32_t kSongRecord_Metadata = 0x108;      // SongMetadata*
constexpr uint32_t kSongMetadata_Origin = 0x38;       // Symbol
constexpr uint32_t kDynamicTex_Size = 0x20;
constexpr uint32_t kDynamicTex_Tex = 0x4;             // RndTex*
constexpr uint32_t kDynamicTex_Mat = 0x14;            // RndMat*
constexpr uint32_t kDynamicTex_Loader = 0x18;         // FileLoader*
constexpr uint32_t kRndMat_DiffuseTex = 0x8C;         // ObjPtr<RndTex>

// SortNode's vtable: GetType() after Hmx::Object's 21 methods
constexpr uint32_t kSortNode_GetType = 21;
constexpr int32_t kNodeSong = 4;

constexpr const char* kSlot = "game_origin_icon";
// RB3E keeps up to 100
constexpr size_t kMaxOrigins = 256;

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

void Store32(uint8_t* base, uint32_t addr, uint32_t value) {
    *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr) = value;
}

const char* GuestStr(uint8_t* base, uint32_t addr) {
    return addr ? rex::memory::GuestPtr<const char*>(base, addr) : nullptr;
}

// context for calling a guest function from a hook: a stack below the
// caller's frame, with its r13 (as band3::Symbol does)
PPCContext CallContext(const PPCContext& ctx, uint32_t reserve) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - reserve;
    call.r13 = ctx.r13;
    return call;
}

std::mutex g_mutex;
// the origins songs have, as their Symbols' strings (a Symbol is the same string
// wherever it's made, so its address names it), in the order they were found
std::vector<uint32_t> g_origins;
// each origin with an icon: its DynamicTex, made when the Music Library is entered
std::unordered_map<uint32_t, uint32_t> g_icons;
// origins already looked for and found without an icon, so each is logged once
std::set<std::string> g_missing;
// the slot names the list asks for, logged once each
std::set<std::string> g_slots_seen;

void AddOrigin(uint8_t* base, uint32_t metadata) {
    if (!metadata) return;
    const uint32_t origin = Load32(base, metadata + kSongMetadata_Origin);
    const char* name = GuestStr(base, origin);
    if (!name || !*name) return;
    std::lock_guard lock(g_mutex);
    if (g_origins.size() >= kMaxOrigins) return;
    for (uint32_t known : g_origins) {
        if (known == origin) return;
    }
    g_origins.push_back(origin);
}

// whether the game has the icon for an origin: in its ARK, or loose in game:\ (file.cpp)
bool IconExists(PPCContext& ctx, uint8_t* base, const std::string& path) {
    if (band3::GameFileExists(band3::LooseName(path))) return true;
    // FileExists(const char* path, int flags), the path below this frame
    const uint32_t guest_path = ctx.r1.u32 - 0x200;
    std::memcpy(base + guest_path, path.c_str(), path.size() + 1);
    PPCContext call = CallContext(ctx, 0x400);
    call.r3.u64 = guest_path;
    call.r4.u64 = 0;
    FileExists(call, base);
    return (call.r3.u32 & 0xFF) != 0;
}

void FreeIcon(PPCContext& ctx, uint8_t* base, uint32_t dynamic_tex) {
    // DynamicTex's deleting destructor: its material, loader and texture, its
    // name, then the DynamicTex
    PPCContext call = CallContext(ctx, 0x400);
    call.r3.u64 = dynamic_tex;
    call.r4.u64 = 1;
    DynamicTex__sdtor(call, base);
}

// a DynamicTex showing ui/resource/game_origins/<origin>.png, or 0 if the game
// has no such icon or it didn't load
uint32_t MakeIcon(PPCContext& ctx, uint8_t* base, const std::string& origin) {
    // the ARK path the game is given, and the file it reads for it
    const std::string path = "ui/resource/game_origins/" + origin + ".png";
    if (!IconExists(ctx, base, "ui/resource/game_origins/gen/" + origin + ".png_xbox")) {
        return 0;
    }

    PPCContext call = CallContext(ctx, 0x400);
    call.r3.u64 = kDynamicTex_Size;
    operator_new(call, base);
    const uint32_t dynamic_tex = call.r3.u32;
    if (!dynamic_tex) return 0;

    // DynamicTex(DynamicTex*, const char* path, const char* mat_name,
    // bool new_mat, bool z_buffer), the strings below this frame: it copies both
    const uint32_t guest_path = ctx.r1.u32 - 0x300;
    const uint32_t guest_name = ctx.r1.u32 - 0x200;
    std::memcpy(base + guest_path, path.c_str(), path.size() + 1);
    std::memcpy(base + guest_name, origin.c_str(), origin.size() + 1);
    call = CallContext(ctx, 0x400);
    call.r3.u64 = dynamic_tex;
    call.r4.u64 = guest_path;
    call.r5.u64 = guest_name;
    call.r6.u64 = 1;
    call.r7.u64 = 0;
    DynamicTex____ct(call, base);

    const uint32_t tex = Load32(base, dynamic_tex + kDynamicTex_Tex);
    const uint32_t mat = Load32(base, dynamic_tex + kDynamicTex_Mat);
    const uint32_t loader = Load32(base, dynamic_tex + kDynamicTex_Loader);
    if (!tex || !mat || !loader) {
        REXLOG_WARN("origin icons: couldn't load {}", path);
        FreeIcon(ctx, base, dynamic_tex);
        return 0;
    }

    // RndTex::SetBitmap(RndTex*, FileLoader*) waits for the file and deletes the
    // loader, so the DynamicTex mustn't delete it again
    call = CallContext(ctx, 0x400);
    call.r3.u64 = tex;
    call.r4.u64 = loader;
    RndTex__SetBitmap3(call, base);
    Store32(base, dynamic_tex + kDynamicTex_Loader, 0);

    // the material's diffuse texture, through ObjPtr's setter, which keeps the
    // texture's references (the material, freed first, lets go of it)
    call = CallContext(ctx, 0x400);
    call.r3.u64 = mat + kRndMat_DiffuseTex;
    call.r4.u64 = tex;
    ObjRefConcrete_Hmx_Object_ObjectDir__SetObjConcrete(call, base);
    return dynamic_tex;
}

// the icon's material for list row idx, or 0 for a row that isn't a song or a
// song whose origin has none
uint32_t RowMaterial(PPCContext& ctx, uint8_t* base, uint32_t library, int32_t idx) {
    const uint32_t sort_mgr = Load32(base, kTheSongSortMgrPtr);
    if (!sort_mgr) return 0;

    // SongSortMgr::GetSort(SongSortMgr*, SongSortType) -> NodeSort*
    PPCContext call = CallContext(ctx, 0x400);
    call.r3.u64 = sort_mgr;
    call.r4.u64 = Load32(base, library + kMusicLibrary_SortType);
    SongSortMgr__GetSort(call, base);
    const uint32_t sort = call.r3.u32;
    if (!sort) return 0;

    // NodeSort::GetNode(NodeSort*, int) -> SortNode*
    call = CallContext(ctx, 0x400);
    call.r3.u64 = sort;
    call.r4.u64 = static_cast<uint32_t>(idx);
    MusicLibrary__GetNodeByIndex(call, base);
    const uint32_t node = call.r3.u32;
    if (!node) return 0;

    // SortNode::GetType(SortNode*), virtual
    const uint32_t get_type = Load32(base, Load32(base, node) + kSortNode_GetType * 4);
    call = CallContext(ctx, 0x400);
    call.r3.u64 = node;
    rex::runtime::ResolveIndirectFunction(get_type)(call, base);
    if (call.r3.s32 != kNodeSong) return 0;

    const uint32_t record = Load32(base, node + kSortNode_Record);
    const uint32_t metadata = record ? Load32(base, record + kSongRecord_Metadata) : 0;
    if (!metadata) return 0;
    const uint32_t origin = Load32(base, metadata + kSongMetadata_Origin);

    std::lock_guard lock(g_mutex);
    const auto icon = g_icons.find(origin);
    return icon == g_icons.end() ? 0 : Load32(base, icon->second + kDynamicTex_Mat);
}

}  // namespace

// MusicLibrary::Mat(MusicLibrary*, int, int idx, UIListMesh* slot) -> RndMat*,
// the material for a list row's mesh slot
extern "C" REX_FUNC(MusicLibrary__Mat) {
    const uint32_t library = ctx.r3.u32;
    const int32_t idx = ctx.r5.s32;
    const uint32_t slot = ctx.r6.u32;
    if (library && slot && REXCVAR_GET(game_origin_icons)) {
        const char* match = GuestStr(base, Load32(base, slot + kUIListSlot_MatchNameBuf));
        if (match) {
            {
                std::lock_guard lock(g_mutex);
                // which song list is loaded shows in which slots it has
                if (g_slots_seen.insert(match).second) {
                    REXLOG_DEBUG("origin icons: the song list has a slot named {}", match);
                }
            }
            if (std::strcmp(match, kSlot) == 0) {
                if (const uint32_t mat = RowMaterial(ctx, base, library, idx)) {
                    ctx.r3.u64 = mat;
                    return;
                }
            }
        }
    }
    __imp__MusicLibrary__Mat(ctx, base);
}

// entering the Music Library: a material for each origin found since that has an icon
extern "C" REX_FUNC(MusicLibrary__OnEnter) {
    if (REXCVAR_GET(game_origin_icons)) {
        std::vector<uint32_t> origins;
        {
            std::lock_guard lock(g_mutex);
            origins = g_origins;
        }
        size_t made = 0;
        for (uint32_t origin : origins) {
            {
                std::lock_guard lock(g_mutex);
                if (g_icons.count(origin)) continue;
            }
            const std::string name = GuestStr(base, origin);
            {
                std::lock_guard lock(g_mutex);
                if (g_missing.count(name)) continue;
            }
            if (const uint32_t icon = MakeIcon(ctx, base, name)) {
                std::lock_guard lock(g_mutex);
                g_icons.emplace(origin, icon);
                made++;
            } else {
                std::lock_guard lock(g_mutex);
                g_missing.insert(name);
                REXLOG_INFO("origin icons: no icon for {}", name);
            }
        }
        if (made) {
            REXLOG_INFO("origin icons: loaded {} of {} origins' icons", made, origins.size());
        }
    }
    __imp__MusicLibrary__OnEnter(ctx, base);
}

// the Music Library's panel unloading: its icons go with it
extern "C" REX_FUNC(MusicLibrary__OnUnload) {
    __imp__MusicLibrary__OnUnload(ctx, base);
    std::unordered_map<uint32_t, uint32_t> icons;
    {
        std::lock_guard lock(g_mutex);
        icons.swap(g_icons);
    }
    for (const auto& [origin, icon] : icons) FreeIcon(ctx, base, icon);
}

// SongMetadata(SongMetadata*, DataArray*, DataArray*, bool on_disc): a song read
// from its songs.dta
extern "C" REX_FUNC(SongMetadata____ct) {
    const uint32_t metadata = ctx.r3.u32;
    __imp__SongMetadata____ct(ctx, base);
    AddOrigin(base, metadata);
}

// SongMetadata::Load(SongMetadata*, BinStream*): a song read from the song cache
extern "C" REX_FUNC(SongMetadata__Load) {
    const uint32_t metadata = ctx.r3.u32;
    __imp__SongMetadata__Load(ctx, base);
    AddOrigin(base, metadata);
}
