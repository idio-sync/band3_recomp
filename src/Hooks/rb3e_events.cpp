#include <rex/hook.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <cstdint>
#include "src/Net/events.h"

// Sends game state as RB3Enhanced network events, from the same hook points
// and with the same data as RB3E (source/rb3enhanced.c, source/GameHooks.c).
// Addresses and struct offsets are RB3E's Xbox 360 TU5 ones (include/ports_xbox360.h,
// include/rb3/*.h), which match this project's function map.

extern "C" void __imp__StageKit__SetState(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__Game____ct(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__Game____dt(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__PresenceMgr__UpdatePresence(PPCContext& ctx, uint8_t* base);

REX_EXTERN(MetaPerformer__GetSongShortname);
REX_EXTERN(BandSongMgr__GetSongIDFromShortname);
REX_EXTERN(BandSongMgr__Data);
REX_EXTERN(BandUserMgr__GetBandUserFromSlot);

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

// sends a game String's contents; String::length excludes the terminator
void SendGameString(uint8_t* base, band3::events::EventType type, uint32_t string_addr) {
    uint32_t length = Load32(base, string_addr + kString_Length);
    uint32_t buf = Load32(base, string_addr + kString_Buf);
    if (buf) band3::events::Send(type, GuestStr(base, buf), length);
}

void SendSongInfo(const PPCContext& ctx, uint8_t* base) {
    uint32_t meta_performer = Load32(base, kTheMetaPerformerPtr);
    if (!meta_performer) return;

    // MetaPerformer::GetSongShortname(Symbol* out, MetaPerformer*)
    PPCContext call = CallContext(ctx, 0x100);
    uint32_t out = call.r1.u32 + 0x80;
    call.r3.u64 = out;
    call.r4.u64 = meta_performer;
    MetaPerformer__GetSongShortname(call, base);
    uint32_t shortname = Load32(base, out);
    if (!shortname) return;

    band3::events::SendString(band3::events::kSongShortname, GuestStr(base, shortname));

    // BandSongMgr::GetSongIDFromShortname(BandSongMgr*, Symbol, int fail)
    call = CallContext(ctx, 0x100);
    call.r3.u64 = kTheSongMgr;
    call.r4.u64 = shortname;
    call.r5.u64 = 1;
    BandSongMgr__GetSongIDFromShortname(call, base);
    uint32_t song_id = call.r3.u32;

    // BandSongMgr::Data(BandSongMgr*, int song_id) -> SongMetadata*
    call = CallContext(ctx, 0x100);
    call.r3.u64 = kTheSongMgr;
    call.r4.u64 = song_id;
    BandSongMgr__Data(call, base);
    uint32_t metadata = call.r3.u32;
    if (!metadata) return;

    SendGameString(base, band3::events::kSongName, metadata + kSongMetadata_Title);
    SendGameString(base, band3::events::kSongArtist, metadata + kSongMetadata_Artist);
}

void SendBandInfo(const PPCContext& ctx, uint8_t* base) {
    band3::events::BandInfo info{};
    uint32_t user_mgr = Load32(base, kTheBandUserMgrPtr);
    if (user_mgr) {
        for (uint32_t slot = 0; slot < 4; slot++) {
            // BandUserMgr::GetBandUserFromSlot(BandUserMgr*, int slot) -> BandUser*
            PPCContext call = CallContext(ctx, 0x100);
            call.r3.u64 = user_mgr;
            call.r4.u64 = slot;
            BandUserMgr__GetBandUserFromSlot(call, base);
            uint32_t user = call.r3.u32;
            if (!user) continue;
            info.member_exists[slot] = 1;
            info.difficulty[slot] = static_cast<uint8_t>(Load32(base, user + kBandUser_Difficulty));
            info.track_type[slot] = static_cast<uint8_t>(Load32(base, user + kBandUser_TrackType));
        }
    }
    band3::events::Send(band3::events::kBandInfo, &info, sizeof(info));
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
    if (band3::events::Enabled()) {
        SendSongInfo(ctx, base);
        SendBandInfo(ctx, base);
        SendState(1);
    }
    __imp__Game____ct(ctx, base);
}

extern "C" REX_FUNC(Game____dt)
{
    SendState(0);
    // the game can leave LEDs on after the score screen; turn everything off
    SendStagekit(0x00, 0xFF);
    __imp__Game____dt(ctx, base);
}

// the game updates presence on screen changes; report the new screen's name
extern "C" REX_FUNC(PresenceMgr__UpdatePresence)
{
    if (band3::events::Enabled()) {
        uint32_t screen = Load32(base, kTheBandUI + kBandUI_CurrentScreen);
        uint32_t name = screen ? Load32(base, screen + kUIScreen_Name) : 0;
        if (name) band3::events::SendString(band3::events::kScreenName, GuestStr(base, name));
    }
    __imp__PresenceMgr__UpdatePresence(ctx, base);
}
