#include "SongMgr.h"
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <algorithm>
#include <cstring>

// Addresses and struct offsets are RB3Enhanced's Xbox 360 TU5 ones
// (include/ports_xbox360.h, include/rb3/SongMetadata.h), which match this
// project's function map.

REX_EXTERN(BandSongMgr__Data);
REX_EXTERN(BandSongMgr__GetRankedSongs);
REX_EXTERN(BandSongMgr__GetSongIDFromShortname);
REX_EXTERN(BandSongMetadata__HasAlbumArt);
REX_EXTERN(SongMgr__GetAlbumArtPath);

namespace band3::songs {

namespace {

constexpr uint32_t kTheSongMgr = 0x82DFE7B4;  // BandSongMgr object
// SongMgr*, which the Music Library's song nodes pass to GetAlbumArtPath
constexpr uint32_t kTheSongMgrPtr = 0x82C72BA8;

// BandSongMetadata
constexpr uint32_t kSongMetadata_Shortname = 0x2C;  // Symbol
constexpr uint32_t kSongMetadata_Origin = 0x38;     // Symbol
constexpr uint32_t kSongMetadata_Title = 0x4C;      // String {vtable, length, buf}
constexpr uint32_t kSongMetadata_Artist = 0x58;     // String
constexpr uint32_t kSongMetadata_Album = 0x64;      // String
constexpr uint32_t kSongMetadata_Genre = 0x80;      // Symbol
constexpr uint32_t kString_Length = 0x4;
constexpr uint32_t kString_Buf = 0x8;
constexpr uint32_t kVector_Begin = 0x0;
constexpr uint32_t kVector_End = 0x4;

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
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

// the guest vector<int> GetRankedSongs fills, kept for the session as RB3E
// keeps its own: the function clears it first, and its storage is the game's
uint32_t g_ranked_songs = 0;

}

std::optional<Song> Get(PPCContext& ctx, uint8_t* base, int32_t id) {
    // BandSongMgr::Data(BandSongMgr*, int id) -> SongMetadata*, null for a song
    // it doesn't have
    PPCContext call = CallContext(ctx, 0x100);
    call.r3.u64 = kTheSongMgr;
    call.r4.u64 = static_cast<uint32_t>(id);
    BandSongMgr__Data(call, base);
    const uint32_t metadata = call.r3.u32;
    if (!metadata) return std::nullopt;

    Song song;
    song.id = id;
    song.shortname = ReadSymbol(base, metadata + kSongMetadata_Shortname);
    song.origin = ReadSymbol(base, metadata + kSongMetadata_Origin);
    song.genre = ReadSymbol(base, metadata + kSongMetadata_Genre);
    song.title = ReadString(base, metadata + kSongMetadata_Title);
    song.artist = ReadString(base, metadata + kSongMetadata_Artist);
    song.album = ReadString(base, metadata + kSongMetadata_Album);
    return song;
}

std::vector<int32_t> RankedIds(PPCContext& ctx, uint8_t* base) {
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
    std::vector<int32_t> ids;
    if (!begin || end <= begin) return ids;
    const uint32_t count = (end - begin) / 4;
    ids.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        ids.push_back(static_cast<int32_t>(Load32(base, begin + i * 4)));
    }
    return ids;
}

int32_t IdFromShortname(PPCContext& ctx, uint8_t* base, uint32_t symbol) {
    // BandSongMgr::GetSongIDFromShortname(BandSongMgr*, Symbol, bool fail)
    PPCContext call = CallContext(ctx, 0x400);
    call.r3.u64 = kTheSongMgr;
    call.r4.u64 = symbol;
    call.r5.u64 = 0;
    BandSongMgr__GetSongIDFromShortname(call, base);
    return std::max(call.r3.s32, 0);
}

std::string AlbumArtPath(PPCContext& ctx, uint8_t* base, uint32_t symbol) {
    const int32_t id = IdFromShortname(ctx, base, symbol);
    if (!id) return {};
    PPCContext call = CallContext(ctx, 0x100);
    call.r3.u64 = kTheSongMgr;
    call.r4.u64 = static_cast<uint32_t>(id);
    BandSongMgr__Data(call, base);
    const uint32_t metadata = call.r3.u32;
    if (!metadata) return {};

    // BandSongMetadata::HasAlbumArt(BandSongMetadata*) -> bool
    call = CallContext(ctx, 0x100);
    call.r3.u64 = metadata;
    BandSongMetadata__HasAlbumArt(call, base);
    if (!(call.r3.u32 & 0xFF)) return {};

    // SongMgr::GetAlbumArtPath(SongMgr*, Symbol) -> const char*
    const uint32_t song_mgr = Load32(base, kTheSongMgrPtr);
    if (!song_mgr) return {};
    call = CallContext(ctx, 0x400);
    call.r3.u64 = song_mgr;
    call.r4.u64 = symbol;
    SongMgr__GetAlbumArtPath(call, base);
    const char* path = GuestStr(base, call.r3.u32);
    return path ? std::string(path) : std::string();
}

}
