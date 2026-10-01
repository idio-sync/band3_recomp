#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <algorithm>
#include <cstring>
#include "src/Game/Symbol.h"
#include "src/Net/http_game.h"

// The game's side of the web server: the same calls RB3Enhanced makes for it
// (source/net_http_server.c, source/MusicLibrary.c), with its Xbox 360 TU5
// addresses and struct offsets (include/ports_xbox360.h, include/rb3/*.h).
// Every function here runs on the game thread, from http::RunGameJobs.

REX_EXTERN(BandSongMgr__Data);
REX_EXTERN(BandSongMgr__GetRankedSongs);
REX_EXTERN(BandSongMgr__GetSongIDFromShortname);
REX_EXTERN(Object__Find_UIPanel_);
REX_EXTERN(MusicLibrary__TryToSetHighlight);
REX_EXTERN(RockCentralGateway__ExecuteConfig);

namespace band3::http::game {

namespace {

// globals
constexpr uint32_t kTheSongMgr = 0x82DFE7B4;          // BandSongMgr object
constexpr uint32_t kTheMusicLibraryPtr = 0x82DFD3A8;  // MusicLibrary*
constexpr uint32_t kMainDirPtr = 0x82E054B8;          // ObjectDir::sMainDir
constexpr uint32_t kRockCentralGateway = 0x82CC8F60;  // RockCentralGateway object

// struct offsets
constexpr uint32_t kSongMetadata_Shortname = 0x2C;  // Symbol
constexpr uint32_t kSongMetadata_Origin = 0x38;     // Symbol
constexpr uint32_t kSongMetadata_Title = 0x4C;      // String {vtable, length, buf}
constexpr uint32_t kSongMetadata_Artist = 0x58;     // String
constexpr uint32_t kSongMetadata_Album = 0x64;      // String
constexpr uint32_t kString_Length = 0x4;
constexpr uint32_t kString_Buf = 0x8;
constexpr uint32_t kUIPanel_IsUp = 0x20;
constexpr uint32_t kVector_Begin = 0x0;
constexpr uint32_t kVector_End = 0x4;

// MusicLibrary's SongNodeType for a song (2 is an artist or origin heading,
// 3 an album)
constexpr uint32_t kSongNode = 4;

// longer than any shortname; Symbol takes up to 255
constexpr size_t kMaxShortname = 255;

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

const char* GuestStr(uint8_t* base, uint32_t addr) {
    return addr ? rex::memory::GuestPtr<const char*>(base, addr) : nullptr;
}

// context for calling a guest function from the hook: a stack below the
// hooked function's frame, with the caller's r13 (as band3::Symbol does)
PPCContext CallContext(const PPCContext& ctx, uint32_t reserve) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - reserve;
    call.r13 = ctx.r13;
    return call;
}

std::string ReadSymbol(uint8_t* base, uint32_t symbol_addr) {
    const char* str = GuestStr(base, Load32(base, symbol_addr));
    return str ? std::string(str) : std::string();
}

// copies a game String; String::length excludes the terminator
std::string ReadString(uint8_t* base, uint32_t string_addr) {
    const uint32_t length = Load32(base, string_addr + kString_Length);
    const uint32_t buf = Load32(base, string_addr + kString_Buf);
    if (!buf) return {};
    return std::string(GuestStr(base, buf), std::min<uint32_t>(length, 1024));
}

// BandSongMgr::Data(BandSongMgr*, int id) -> SongMetadata*, null for a song it
// doesn't have
std::optional<SongInfo> ReadSong(PPCContext& ctx, uint8_t* base, int32_t id) {
    PPCContext call = CallContext(ctx, 0x100);
    call.r3.u64 = kTheSongMgr;
    call.r4.u64 = static_cast<uint32_t>(id);
    BandSongMgr__Data(call, base);
    const uint32_t metadata = call.r3.u32;
    if (!metadata) return std::nullopt;

    SongInfo song;
    song.shortname = ReadSymbol(base, metadata + kSongMetadata_Shortname);
    song.origin = ReadSymbol(base, metadata + kSongMetadata_Origin);
    song.title = ReadString(base, metadata + kSongMetadata_Title);
    song.artist = ReadString(base, metadata + kSongMetadata_Artist);
    song.album = ReadString(base, metadata + kSongMetadata_Album);
    return song;
}

// the guest vector<int> GetRankedSongs fills, kept for the session as RB3E
// keeps its own: the function clears it first, and its storage is the game's
uint32_t g_ranked_songs = 0;

}

std::optional<SongInfo> Song(PPCContext& ctx, uint8_t* base, int32_t id) {
    return ReadSong(ctx, base, id);
}

std::vector<SongInfo> RankedSongs(PPCContext& ctx, uint8_t* base) {
    if (!g_ranked_songs) {
        g_ranked_songs = rex::system::kernel_memory()->SystemHeapAlloc(12, 4);
        if (!g_ranked_songs) return {};
        std::memset(base + g_ranked_songs, 0, 12);
    }

    // BandSongMgr::GetRankedSongs(BandSongMgr*, vector<int>*, bool demos, bool restricted)
    PPCContext call = CallContext(ctx, 0x100);
    call.r3.u64 = kTheSongMgr;
    call.r4.u64 = g_ranked_songs;
    call.r5.u64 = 0;
    call.r6.u64 = 0;
    BandSongMgr__GetRankedSongs(call, base);

    const uint32_t begin = Load32(base, g_ranked_songs + kVector_Begin);
    const uint32_t end = Load32(base, g_ranked_songs + kVector_End);
    std::vector<SongInfo> songs;
    if (!begin || end <= begin) return songs;
    const uint32_t count = (end - begin) / 4;
    songs.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        const auto id = static_cast<int32_t>(Load32(base, begin + i * 4));
        if (auto song = ReadSong(ctx, base, id)) songs.push_back(std::move(*song));
    }
    return songs;
}

JumpResult JumpToSong(PPCContext& ctx, uint8_t* base, const std::string& shortname) {
    const uint32_t library = Load32(base, kTheMusicLibraryPtr);
    const uint32_t main_dir = Load32(base, kMainDirPtr);
    if (!library || !main_dir) return JumpResult::kNotInLibrary;

    // Object::Find<UIPanel>(ObjectDir*, const char* name, bool fail), not failing:
    // RB3E only jumps while the song select panel is up, since selecting
    // otherwise crashes
    constexpr char kPanel[] = "song_select_panel";
    const uint32_t panel_name = ctx.r1.u32 - 0x100;
    std::memcpy(base + panel_name, kPanel, sizeof(kPanel));
    PPCContext call = CallContext(ctx, 0x200);
    call.r3.u64 = main_dir;
    call.r4.u64 = panel_name;
    call.r5.u64 = 0;
    Object__Find_UIPanel_(call, base);
    const uint32_t panel = call.r3.u32;
    if (!panel || Load32(base, panel + kUIPanel_IsUp) != 1) return JumpResult::kNotInLibrary;

    if (shortname.empty() || shortname.size() > kMaxShortname) return JumpResult::kUnknownSong;
    const uint32_t symbol = band3::Symbol(ctx, base, shortname.c_str()).value(base);
    if (!symbol) return JumpResult::kUnknownSong;

    // BandSongMgr::GetSongIDFromShortname(BandSongMgr*, Symbol, bool fail)
    call = CallContext(ctx, 0x400);
    call.r3.u64 = kTheSongMgr;
    call.r4.u64 = symbol;
    call.r5.u64 = 0;
    BandSongMgr__GetSongIDFromShortname(call, base);
    if (call.r3.s32 <= 0) return JumpResult::kUnknownSong;

    // MusicLibrary::TryToSetHighlight(MusicLibrary*, Symbol, SongNodeType, bool)
    call = CallContext(ctx, 0x400);
    call.r3.u64 = library;
    call.r4.u64 = symbol;
    call.r5.u64 = kSongNode;
    call.r6.u64 = 1;
    MusicLibrary__TryToSetHighlight(call, base);
    return JumpResult::kJumped;
}

void ExecuteScript(PPCContext& ctx, uint8_t* base, const std::string& script) {
    auto* memory = rex::system::kernel_memory();
    const auto size = static_cast<uint32_t>(script.size() + 1);
    const uint32_t text = memory->SystemHeapAlloc(size, 4);
    if (!text) return;
    std::memcpy(base + text, script.c_str(), size);

    // RockCentralGateway::ExecuteConfig(RockCentralGateway*, const char* dta)
    PPCContext call = CallContext(ctx, 0x100);
    call.r3.u64 = kRockCentralGateway;
    call.r4.u64 = text;
    RockCentralGateway__ExecuteConfig(call, base);

    memory->SystemHeapFree(text);
}

}
