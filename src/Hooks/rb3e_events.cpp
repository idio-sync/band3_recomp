#include <rex/hook.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include "src/Net/discord.h"
#include "src/Net/events.h"
#include "src/Net/http_server.h"
#include "src/Render/native_view.h"
#include "src/Test/game_state.h"
#include "src/Test/test_server.h"

// Reports game state to the RB3Enhanced network events, Discord presence, the
// native renderer (whether a song is on: native_view.h's InSong), and
// the test harness and web server's /status (band3::test::GameState),
// from the same hook points and with the same data as RB3E (source/rb3enhanced.c,
// source/GameHooks.c), and also from PresenceMgr::SetSongID, for a song that
// starts without a new Game.
// Addresses and struct offsets are RB3E's Xbox 360 TU5 ones (include/ports_xbox360.h,
// include/rb3/*.h), which match this project's function map.

extern "C" void __imp__StageKit__SetState(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__Game____ct(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__Game____dt(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__PresenceMgr__UpdatePresence(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__PresenceMgr__SetSongID(PPCContext& ctx, uint8_t* base);

REX_EXTERN(MetaPerformer__Song);
REX_EXTERN(BandSongMgr__GetSongIDFromShortname);
REX_EXTERN(BandSongMgr__GetShortNameFromSongID);
REX_EXTERN(BandSongMgr__Data);
REX_EXTERN(BandUserMgr__GetUserFromSlot);

namespace {

// globals
constexpr uint32_t kTheBandUI = 0x82DFD2B0;            // BandUI object
constexpr uint32_t kTheSongMgr = 0x82DFE7B4;           // BandSongMgr object
constexpr uint32_t kTheMetaPerformerPtr = 0x82DFE954;  // MetaPerformer*
constexpr uint32_t kTheBandUserMgrPtr = 0x82E023B8;    // BandUserMgr*

// struct offsets
constexpr uint32_t kBandUI_CurrentScreen = 0x2C;       // UIScreen*
constexpr uint32_t kUIScreen_Name = 0x18;              // Symbol (char*)
constexpr uint32_t kSongMetadata_Title = 0x4C;         // String {vtable, length, buf}
constexpr uint32_t kSongMetadata_Artist = 0x58;        // String
constexpr uint32_t kSongMetadata_LengthMs = 0x8C;      // int, as LengthSym reads it
constexpr uint32_t kString_Length = 0x4;
constexpr uint32_t kString_Buf = 0x8;
constexpr uint32_t kBandUser_Difficulty = 0x8;
constexpr uint32_t kBandUser_TrackType = 0x10;

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

const char* GuestStr(uint8_t* base, uint32_t addr) {
    return addr ? rex::memory::GuestPtr<const char*>(base, addr) : nullptr;
}

// context for calling a guest function from inside a hook: a stack below the
// hooked function's frame, with the caller's r13 (as band3::Symbol does)
PPCContext CallContext(const PPCContext& ctx, uint32_t reserve) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - reserve;
    call.r13 = ctx.r13;
    return call;
}

void SendStagekit(uint8_t left, uint8_t right) {
    const uint8_t data[2] = {left, right};
    band3::events::Send(band3::events::kStagekit, data, sizeof(data));
}

void SendState(uint8_t in_game) {
    band3::events::Send(band3::events::kState, &in_game, sizeof(in_game));
}

// copies a game String; String::length excludes the terminator
std::string ReadGameString(uint8_t* base, uint32_t string_addr) {
    uint32_t length = Load32(base, string_addr + kString_Length);
    uint32_t buf = Load32(base, string_addr + kString_Buf);
    if (!buf) return {};
    return std::string(GuestStr(base, buf), std::min<uint32_t>(length, 1024));
}

struct SongInfo {
    int32_t id = 0;
    std::string shortname;
    std::string title;
    std::string artist;
    int32_t length_ms = 0;
};

// the song last reported, so GamePanel::Enter's SetSongID (below) only reports
// one Game's constructor hasn't; 0 while none is
int32_t g_reported_song = 0;

// title, artist and length from BandSongMgr::Data(BandSongMgr*, int song_id) -> SongMetadata*
void ReadSongMetadata(const PPCContext& ctx, uint8_t* base, SongInfo& song) {
    PPCContext call = CallContext(ctx, 0x100);
    call.r3.u64 = kTheSongMgr;
    call.r4.u64 = static_cast<uint32_t>(song.id);
    BandSongMgr__Data(call, base);
    uint32_t metadata = call.r3.u32;
    if (!metadata) return;

    song.title = ReadGameString(base, metadata + kSongMetadata_Title);
    song.artist = ReadGameString(base, metadata + kSongMetadata_Artist);
    song.length_ms = static_cast<int32_t>(Load32(base, metadata + kSongMetadata_LengthMs));
}

// the song MetaPerformer has picked
SongInfo ReadSongInfo(const PPCContext& ctx, uint8_t* base) {
    SongInfo song;
    uint32_t meta_performer = Load32(base, kTheMetaPerformerPtr);
    if (!meta_performer) return song;

    // MetaPerformer::Song(Symbol* out, MetaPerformer*)
    PPCContext call = CallContext(ctx, 0x100);
    uint32_t out = call.r1.u32 + 0x80;
    call.r3.u64 = out;
    call.r4.u64 = meta_performer;
    MetaPerformer__Song(call, base);
    uint32_t shortname = Load32(base, out);
    if (!shortname) return song;
    song.shortname = GuestStr(base, shortname);

    // BandSongMgr::GetSongIDFromShortname(BandSongMgr*, Symbol, int fail)
    call = CallContext(ctx, 0x100);
    call.r3.u64 = kTheSongMgr;
    call.r4.u64 = shortname;
    call.r5.u64 = 1;
    BandSongMgr__GetSongIDFromShortname(call, base);
    song.id = call.r3.s32;

    ReadSongMetadata(ctx, base, song);
    return song;
}

// the song with ID `id`; empty when the song manager doesn't know it
SongInfo ReadSongInfoById(const PPCContext& ctx, uint8_t* base, int32_t id) {
    SongInfo song;
    song.id = id;

    // BandSongMgr::GetShortNameFromSongID(Symbol* out, BandSongMgr*, int id, bool fail),
    // not failing: an unknown ID gives the empty symbol
    PPCContext call = CallContext(ctx, 0x100);
    uint32_t out = call.r1.u32 + 0x80;
    call.r3.u64 = out;
    call.r4.u64 = kTheSongMgr;
    call.r5.u64 = static_cast<uint32_t>(id);
    call.r6.u64 = 0;
    BandSongMgr__GetShortNameFromSongID(call, base);
    const char* shortname = GuestStr(base, Load32(base, out));
    if (!shortname || !*shortname) return song;
    song.shortname = shortname;

    ReadSongMetadata(ctx, base, song);
    return song;
}

void SendSong(const SongInfo& song) {
    using namespace band3::events;
    if (!song.shortname.empty()) SendString(kSongShortname, song.shortname.c_str());
    if (!song.title.empty()) Send(kSongName, song.title.data(), song.title.size());
    if (!song.artist.empty()) Send(kSongArtist, song.artist.data(), song.artist.size());
}

void RecordSong(const SongInfo& song) {
    band3::test::GameState::Get().SetSong(song.title, song.artist, song.shortname,
                                          song.length_ms);
}

// GameState is kept for the test harness and the web server's /status
bool Recording() { return band3::test::Enabled() || band3::http::Enabled(); }

void RecordBand(const band3::events::BandInfo& info) {
    std::array<band3::test::BandMember, 4> band{};
    for (size_t i = 0; i < band.size(); i++) {
        band[i] = {info.member_exists[i] != 0, info.difficulty[i], info.track_type[i]};
    }
    band3::test::GameState::Get().SetBand(band);
}

band3::events::BandInfo ReadBandInfo(const PPCContext& ctx, uint8_t* base) {
    band3::events::BandInfo info{};
    uint32_t user_mgr = Load32(base, kTheBandUserMgrPtr);
    if (!user_mgr) return info;
    for (uint32_t slot = 0; slot < 4; slot++) {
        // BandUserMgr::GetUserFromSlot(BandUserMgr*, int slot) -> BandUser*
        PPCContext call = CallContext(ctx, 0x100);
        call.r3.u64 = user_mgr;
        call.r4.u64 = slot;
        BandUserMgr__GetUserFromSlot(call, base);
        uint32_t user = call.r3.u32;
        if (!user) continue;
        info.member_exists[slot] = 1;
        info.difficulty[slot] = static_cast<uint8_t>(Load32(base, user + kBandUser_Difficulty));
        info.track_type[slot] = static_cast<uint8_t>(Load32(base, user + kBandUser_TrackType));
    }
    return info;
}

}

// StageKit::SetState(left, right): left = LED pattern, right = colour/strobe/fog command
extern "C" REX_FUNC(StageKit__SetState)
{
    SendStagekit(static_cast<uint8_t>(ctx.r3.u32), static_cast<uint8_t>(ctx.r4.u32));
    __imp__StageKit__SetState(ctx, base);
}

extern "C" REX_FUNC(Game____ct)
{
    // for the native renderer's residency, harness or not
    band3::render::SetInSong(true);
    bool events = band3::events::Enabled();
    bool discord = band3::discord::Enabled();
    bool record = Recording();
    if (events || discord || record) {
        SongInfo song = ReadSongInfo(ctx, base);
        band3::events::BandInfo band = ReadBandInfo(ctx, base);

        if (record) {
            RecordSong(song);
            RecordBand(band);
            band3::test::GameState::Get().SetInGame(true);
        }

        if (events) {
            using namespace band3::events;
            SendSong(song);
            Send(kBandInfo, &band, sizeof(band));
            SendState(1);
        }
        band3::discord::SetPlaying(song.title, song.artist, band);
        g_reported_song = song.id;
    }
    __imp__Game____ct(ctx, base);
}

extern "C" REX_FUNC(Game____dt)
{
    SendState(0);
    band3::test::GameState::Get().SetInGame(false);
    band3::render::SetInSong(false);
    // the game can leave LEDs on after the score screen; turn everything off
    SendStagekit(0x00, 0xFF);
    band3::discord::SetMenus();
    g_reported_song = 0;
    __imp__Game____dt(ctx, base);
}

// PresenceMgr::SetSongID(this, int id), which GamePanel::Enter calls with the
// song being entered (the Xbox 360 build only). Game's constructor has reported
// it already when the song got a Game of its own; this reports any it hasn't.
extern "C" REX_FUNC(PresenceMgr__SetSongID)
{
    const int32_t id = ctx.r4.s32;
    const bool events = band3::events::Enabled();
    const bool discord = band3::discord::Enabled();
    const bool record = Recording();
    // song IDs below 1 are "any", "random" and "invalid"
    if ((events || discord || record) && id > 0 && id != g_reported_song) {
        SongInfo song = ReadSongInfoById(ctx, base, id);
        if (!song.shortname.empty()) {
            if (events) SendSong(song);
            if (record) RecordSong(song);
            band3::discord::SetPlaying(song.title, song.artist, ReadBandInfo(ctx, base));
            g_reported_song = id;
        }
    }
    __imp__PresenceMgr__SetSongID(ctx, base);
}

// the game updates presence on screen changes; report the new screen's name
extern "C" REX_FUNC(PresenceMgr__UpdatePresence)
{
    const bool events = band3::events::Enabled();
    const bool record = Recording();
    if (events || record) {
        uint32_t screen = Load32(base, kTheBandUI + kBandUI_CurrentScreen);
        uint32_t name = screen ? Load32(base, screen + kUIScreen_Name) : 0;
        if (name && events) {
            band3::events::SendString(band3::events::kScreenName, GuestStr(base, name));
        }
        if (name && record) band3::test::GameState::Get().SetScreen(GuestStr(base, name));
    }
    __imp__PresenceMgr__UpdatePresence(ctx, base);
}
