#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <cstring>
#include <rex/logging.h>
#include "src/Game/File.h"
#include "src/Game/SongMgr.h"
#include "src/Game/Symbol.h"
#include "src/Net/album_art.h"
#include "src/Net/http_game.h"

// The game's side of the web server: the same calls RB3Enhanced makes for it
// (source/net_http_server.c, source/MusicLibrary.c), with its Xbox 360 TU5
// addresses and struct offsets (include/ports_xbox360.h, include/rb3/*.h).
// Every function here runs on the game thread, from http::RunGameJobs.

REX_EXTERN(Object__Find_UIPanel_);
REX_EXTERN(MusicLibrary__TryToSetHighlight);
REX_EXTERN(RockCentralGateway__ExecuteConfig);

namespace band3::http::game {

namespace {

// globals
constexpr uint32_t kTheMusicLibraryPtr = 0x82DFD3A8;  // MusicLibrary*
constexpr uint32_t kMainDirPtr = 0x82E054B8;          // ObjectDir::sMainDir
constexpr uint32_t kRockCentralGateway = 0x82CC8F60;  // RockCentralGateway object

// struct offsets
constexpr uint32_t kUIPanel_IsUp = 0x20;

// MusicLibrary's SongNodeType for a song (2 is an artist or origin heading,
// 3 an album)
constexpr uint32_t kSongNode = 4;

// longer than any shortname; Symbol takes up to 255
constexpr size_t kMaxShortname = 255;

// RB3's album art is 43 KB (256x256 DXT1 and its mipmaps); this leaves room
// for a custom song's 1024x1024 DXT5
constexpr size_t kMaxAlbumArt = 2 * 1024 * 1024;

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

// context for calling a guest function from the hook: a stack below the
// hooked function's frame, with the caller's r13 (as band3::Symbol does)
PPCContext CallContext(const PPCContext& ctx, uint32_t reserve) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - reserve;
    call.r13 = ctx.r13;
    return call;
}

SongInfo ToInfo(const songs::Song& song) {
    return {song.shortname, song.title, song.artist, song.album, song.origin};
}

}

std::optional<SongInfo> Song(PPCContext& ctx, uint8_t* base, int32_t id) {
    if (auto song = songs::Get(ctx, base, id)) return ToInfo(*song);
    return std::nullopt;
}

std::vector<SongInfo> RankedSongs(PPCContext& ctx, uint8_t* base) {
    std::vector<SongInfo> out;
    for (const int32_t id : songs::RankedIds(ctx, base)) {
        if (auto song = songs::Get(ctx, base, id)) out.push_back(ToInfo(*song));
    }
    return out;
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

    if (!songs::IdFromShortname(ctx, base, symbol)) return JumpResult::kUnknownSong;

    // MusicLibrary::TryToSetHighlight(MusicLibrary*, Symbol, SongNodeType, bool)
    call = CallContext(ctx, 0x400);
    call.r3.u64 = library;
    call.r4.u64 = symbol;
    call.r5.u64 = kSongNode;
    call.r6.u64 = 1;
    MusicLibrary__TryToSetHighlight(call, base);
    return JumpResult::kJumped;
}

std::optional<std::string> AlbumArtFile(PPCContext& ctx, uint8_t* base,
                                        const std::string& shortname) {
    if (shortname.empty() || shortname.size() > kMaxShortname) return std::nullopt;
    const uint32_t symbol = band3::Symbol(ctx, base, shortname.c_str()).value(base);
    if (!symbol) return std::nullopt;
    const std::string path = songs::AlbumArtPath(ctx, base, symbol);
    if (path.empty()) return std::nullopt;
    const std::string file = XboxBitmapPath(path);
    REXLOG_DEBUG("Web server: album art for {} is {}", shortname, file);
    return files::ReadAll(ctx, base, file, kMaxAlbumArt);
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
