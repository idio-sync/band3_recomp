#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_set>
#include "generated/band3_init.h"
#include "src/Net/liveless_rooms.h"
#include "src/Net/liveless_rooms_client.h"
#include "src/Net/native_socket.h"
#include "src/Net/online.h"
#include "src/Net/online_hooks.h"
#include "src/Test/game_state.h"
#include "src/settings.h"

// Liveless: RB3 online play without Xbox Live, as RB3Enhanced's
// xbox360_liveless.c has it, so band3 and RB3Enhanced players can play
// together. Live's matchmaking is gone, so searching for an online game finds
// one game, at a stand-in address that online.cpp sends to liveless_connect.
// Live's sessions, secure connections and QoS probes all report success, and
// the game's packets go as plain UDP: each player's game talks straight to the
// others' on liveless_port. RB3 hooks these through its own wrappers of the
// XNet and XSession calls, at the addresses RB3Enhanced patches.

extern "C" void __imp__XNetGetTitleXnAddr(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__band_NetDll_XNetXnAddrToInAddr(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__XNetQosLookup(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__XNetQosRelease(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__XSessionCreate(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__XSessionModify(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__XSessionJoinLocal(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__XSessionSearchEx(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__band_NetDll_XNetConnect(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__band_NetDll_XNetRegisterKey(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__band_NetDll_XNetUnregisterKey(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__band_NetDll_XNetUnregisterInAddr(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__band_NetDll_socket(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__band_NetDll_bind(PPCContext& ctx, uint8_t* base);

namespace {

using band3::online::Liveless;

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorInsufficientBuffer = 122;
constexpr uint32_t kErrorIoPending = 997;

// a call's 9th to 12th arguments, on the caller's stack
constexpr uint32_t kArgument9 = 0x54;
constexpr uint32_t kArgument10 = 0x5C;
constexpr uint32_t kArgument11 = 0x64;
constexpr uint32_t kArgument12 = 0x6C;

// XNADDR: ina, inaOnline, wPortOnline, abEnet[6], abOnline[20]
constexpr uint32_t kXnAddrIna = 0;
constexpr uint32_t kXnAddrInaOnline = 4;
constexpr uint32_t kXnAddrPortOnline = 8;
constexpr uint32_t kXnAddrOnline = 16;
constexpr uint32_t kXnAddrSize = 36;

// XOVERLAPPED: InternalLow (result), InternalHigh, InternalContext, hEvent,
// completion routine and context, dwExtendedError
constexpr uint32_t kOverlappedResult = 0x0;
constexpr uint32_t kOverlappedEvent = 0xC;
constexpr uint32_t kOverlappedExtendedError = 0x18;

void SetEvent(uint32_t event) {
    if (!event) return;
    if (auto e = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(event)) e->Set(0, false);
}

void CompleteOverlapped(uint32_t overlapped, uint8_t* base) {
    REX_STORE_U32(overlapped + kOverlappedResult, kErrorSuccess);
    REX_STORE_U32(overlapped + kOverlappedExtendedError, kErrorSuccess);
    SetEvent(REX_LOAD_U32(overlapped + kOverlappedEvent));
}

void StoreNetworkOrder(uint8_t* base, uint32_t address, uint32_t value) {
    std::memcpy(base + address, &value, 4);
}

// a join, step by step, in the log (log_net_calls)
bool LogJoins() { return REXCVAR_GET(log_net_calls); }

// an async XSession call that RB3Enhanced has succeed at once: its
// XOVERLAPPED, the last argument, completes
void SessionCallSucceeds(PPCContext& ctx, uint8_t* base, uint32_t overlapped, const char* name) {
    REXLOG_INFO("liveless: {}", name);
    if (overlapped) {
        CompleteOverlapped(overlapped, base);
        ctx.r3.u64 = kErrorIoPending;
    } else {
        ctx.r3.u64 = kErrorSuccess;
    }
}

}  // namespace

// XSessionCreate(flags, user, public slots, private slots, u64* nonce,
// XSESSION_INFO*, XOVERLAPPED*, HANDLE* session): the game hosts without Live
extern "C" REX_FUNC(XSessionCreate) {
    if (!Liveless()) return __imp__XSessionCreate(ctx, base);
    SessionCallSucceeds(ctx, base, ctx.r9.u32, "XSessionCreate");
}

// XSessionModify(session, flags, public slots, private slots, XOVERLAPPED*)
extern "C" REX_FUNC(XSessionModify) {
    if (!Liveless()) return __imp__XSessionModify(ctx, base);
    SessionCallSucceeds(ctx, base, ctx.r7.u32, "XSessionModify");
}

// 0x82A69FB0, which RB3Enhanced calls XSessionJoinRemote: (session, count,
// players, BOOL* private slots, XOVERLAPPED*)
extern "C" REX_FUNC(XSessionJoinLocal) {
    if (!Liveless()) return __imp__XSessionJoinLocal(ctx, base);
    SessionCallSucceeds(ctx, base, ctx.r7.u32, "XSessionJoin");
}

// XSessionSearchEx(procedure, user, max results, users, property count,
// context count, XUSER_PROPERTY*, XUSER_CONTEXT*, DWORD* buffer size,
// XSESSION_SEARCHRESULT_HEADER*, XOVERLAPPED*): finds one game, the one to
// join. RVSessionSearcher::StartSearching first asks for the size it needs,
// then searches into a buffer that big.
extern "C" REX_FUNC(XSessionSearchEx) {
    if (!Liveless()) return __imp__XSessionSearchEx(ctx, base);
    // XSESSION_SEARCHRESULT_HEADER (count, results), then one
    // XSESSION_SEARCHRESULT: XSESSION_INFO (XNKID session, XNADDR host,
    // XNKEY key exchange key), the open and filled public and private slots,
    // the property and context counts and arrays
    constexpr uint32_t kHeaderSize = 8;
    constexpr uint32_t kResultSize = 92;
    constexpr uint32_t kNeeded = kHeaderSize + kResultSize;
    const uint32_t property_count = ctx.r7.u32 & 0xFFFF, context_count = ctx.r8.u32 & 0xFFFF;
    const uint32_t properties = ctx.r9.u32, contexts = ctx.r10.u32;
    const uint32_t size = REX_LOAD_U32(ctx.r1.u32 + kArgument9);
    const uint32_t header = REX_LOAD_U32(ctx.r1.u32 + kArgument10);
    const uint32_t overlapped = REX_LOAD_U32(ctx.r1.u32 + kArgument11);
    if (!size) {
        ctx.r3.u64 = 87;  // ERROR_INVALID_PARAMETER
        return;
    }
    if (!header || REX_LOAD_U32(size) < kNeeded) {
        REX_STORE_U32(size, kNeeded);
        ctx.r3.u64 = kErrorInsufficientBuffer;
        return;
    }
    const uint32_t result = header + kHeaderSize;
    std::memset(base + header, 0, kNeeded);
    REX_STORE_U32(header, 1);
    REX_STORE_U32(header + 4, result);
    // the session's ID and key, made up as RB3Enhanced's are
    for (uint32_t i = 0; i < 8; i++) base[result + i] = static_cast<uint8_t>(i + 1);
    const uint32_t host = result + 8;
    StoreNetworkOrder(base, host + kXnAddrInaOnline, band3::online::kJoinStandIn);
    REX_STORE_U16(host + kXnAddrPortOnline, band3::online::LivelessJoinPort());
    const uint32_t key = host + kXnAddrSize;
    for (uint32_t i = 0; i < 16; i++) base[key + i] = static_cast<uint8_t>(i + 1);
    const uint32_t slots = key + 16;
    REX_STORE_U32(slots + 0, 4);   // open public
    REX_STORE_U32(slots + 4, 4);   // open private
    REX_STORE_U32(slots + 8, 1);   // filled public
    REX_STORE_U32(slots + 12, 1);  // filled private
    // what was searched for, as what the game found has
    REX_STORE_U32(slots + 16, property_count);
    REX_STORE_U32(slots + 20, context_count);
    REX_STORE_U32(slots + 24, properties);
    REX_STORE_U32(slots + 28, contexts);
    SessionCallSucceeds(ctx, base, overlapped, "XSessionSearchEx found the game to join");
}

// XNetGetTitleXnAddr(XNADDR*): this game's address as others reach it, on
// liveless_port, which the game hands to who joins it. That's
// liveless_external_ip when it's set, else the public address the Liveless
// Rooms server saw this PC at, else this PC's on the local network.
extern "C" REX_FUNC(XNetGetTitleXnAddr) {
    const uint32_t address = ctx.r3.u32;
    __imp__XNetGetTitleXnAddr(ctx, base);
    if (!Liveless() || !address) return;
    uint32_t external = band3::online::LivelessExternalAddress();
    const char* source = "local network";
    bool from_rooms = false;
    if (!band3::settings::Startup().liveless_external_ip.empty()) {
        source = "setting";
    } else if (const uint32_t rooms = band3::rooms::PublicAddress()) {
        external = rooms;
        source = "rooms server";
        from_rooms = true;
    }
    if (external) StoreNetworkOrder(base, address + kXnAddrInaOnline, external);
    REX_STORE_U16(address + kXnAddrPortOnline, band3::online::LivelessPort());
    for (uint32_t i = 0; i < 20; i++) base[address + kXnAddrOnline + i] = static_cast<uint8_t>(i);
    uint32_t advertised;
    std::memcpy(&advertised, base + address + kXnAddrInaOnline, 4);
    band3::rooms::NoteAdvertised(advertised);
    // the game asks often; say it when it changes
    static std::atomic<uint64_t> last{~uint64_t{0}};
    const uint64_t now = advertised | (uint64_t{from_rooms} << 32);
    if (last.exchange(now) != now) {
        REXLOG_INFO("liveless: advertising {} ({})", band3::rooms::Ipv4Text(advertised), source);
    }
}

// XNetXnAddrToInAddr(const XNADDR*, const XNKID*, IN_ADDR*): the address to
// reach another game at. The game found by searching has none on the local
// network: it's the stand-in for the game to join. Others are where they say
// they're reached.
extern "C" REX_FUNC(band_NetDll_XNetXnAddrToInAddr) {
    if (!Liveless()) return __imp__band_NetDll_XNetXnAddrToInAddr(ctx, base);
    const uint32_t xnaddr = ctx.r3.u32, in_addr = ctx.r5.u32;
    if (!xnaddr || !in_addr) {
        ctx.r3.u64 = 87;  // ERROR_INVALID_PARAMETER
        return;
    }
    const bool stand_in = REX_LOAD_U32(xnaddr + kXnAddrIna) == 0;
    if (stand_in) {
        StoreNetworkOrder(base, in_addr, band3::online::kJoinStandIn);
    } else {
        std::memcpy(base + in_addr, base + xnaddr + kXnAddrInaOnline, 4);
    }
    ctx.r3.u64 = kErrorSuccess;
    if (LogJoins()) {
        uint32_t ina, ina_online, result;
        std::memcpy(&ina, base + xnaddr + kXnAddrIna, 4);
        std::memcpy(&ina_online, base + xnaddr + kXnAddrInaOnline, 4);
        std::memcpy(&result, base + in_addr, 4);
        REXLOG_INFO("liveless: XNetXnAddrToInAddr ina {} inaOnline {} port {} -> {} ({})",
                    band3::rooms::Ipv4Text(ina), band3::rooms::Ipv4Text(ina_online),
                    REX_LOAD_U16(xnaddr + kXnAddrPortOnline), band3::rooms::Ipv4Text(result),
                    stand_in ? "stand-in" : "inaOnline");
    }
}

namespace {

std::mutex g_qos_mutex;
// the XNQOS results band3 made, which XNetQosRelease frees
std::unordered_set<uint32_t> g_qos;
// the one byte of QoS data each result has, as RB3Enhanced's
uint32_t g_qos_data = 0;

}  // namespace

// XNetQosLookup(xnaddr count, XNADDR**, XNKID**, XNKEY**, address count,
// IN_ADDR*, service IDs, probes, bits per second, flags, event, XNQOS**):
// every game probed answers, quickly, so the game takes the one it found
extern "C" REX_FUNC(XNetQosLookup) {
    if (!Liveless()) return __imp__XNetQosLookup(ctx, base);
    // XNQOS: count, pending count, then an XNQOSINFO each: flags, reserved,
    // probes sent and received, data size, data, min and median round trip,
    // up and down bits per second
    constexpr uint32_t kInfoSize = 24;
    constexpr uint8_t kComplete = 0x01, kTargetContacted = 0x02;
    const uint32_t count = ctx.r3.u32 + ctx.r7.u32;
    const uint32_t event = REX_LOAD_U32(ctx.r1.u32 + kArgument11);
    const uint32_t result = REX_LOAD_U32(ctx.r1.u32 + kArgument12);
    if (!result) {
        ctx.r3.u64 = 10022;  // WSAEINVAL
        return;
    }
    auto* memory = rex::system::kernel_memory();
    const uint32_t size = 8 + kInfoSize * std::max(count, 1u);
    const uint32_t qos = memory->SystemHeapAlloc(size, 4);
    if (!qos) {
        ctx.r3.u64 = 10055;  // WSAENOBUFS
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_qos_mutex);
        if (!g_qos_data) {
            g_qos_data = memory->SystemHeapAlloc(4, 4);
            if (g_qos_data) base[g_qos_data] = 'A';
        }
        g_qos.insert(qos);
    }
    std::memset(base + qos, 0, size);
    REX_STORE_U32(qos, count);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t info = qos + 8 + kInfoSize * i;
        base[info] = kComplete | kTargetContacted;
        REX_STORE_U16(info + 2, 4);   // probes sent
        REX_STORE_U16(info + 4, 4);   // received
        REX_STORE_U16(info + 6, 1);   // data size
        REX_STORE_U32(info + 8, g_qos_data);
        REX_STORE_U16(info + 12, 4);  // round trip, ms
        REX_STORE_U16(info + 14, 10);
        REX_STORE_U32(info + 16, 13125);
        REX_STORE_U32(info + 20, 21058);
    }
    REX_STORE_U32(result, qos);
    SetEvent(event);
    ctx.r3.u64 = kErrorSuccess;
}

// XNetQosRelease(XNQOS*)
extern "C" REX_FUNC(XNetQosRelease) {
    const uint32_t qos = ctx.r3.u32;
    bool ours = false;
    {
        std::lock_guard<std::mutex> lock(g_qos_mutex);
        ours = g_qos.erase(qos) > 0;
    }
    if (!ours) return __imp__XNetQosRelease(ctx, base);
    rex::system::kernel_memory()->SystemHeapFree(qos);
    ctx.r3.u64 = kErrorSuccess;
}

// Live's secure connections to other games: there are none to make
extern "C" REX_FUNC(band_NetDll_XNetConnect) {
    if (!Liveless()) return __imp__band_NetDll_XNetConnect(ctx, base);
    ctx.r3.u64 = kErrorSuccess;
}

extern "C" REX_FUNC(band_NetDll_XNetRegisterKey) {
    if (!Liveless()) return __imp__band_NetDll_XNetRegisterKey(ctx, base);
    ctx.r3.u64 = kErrorSuccess;
}

extern "C" REX_FUNC(band_NetDll_XNetUnregisterKey) {
    if (!Liveless()) return __imp__band_NetDll_XNetUnregisterKey(ctx, base);
    ctx.r3.u64 = kErrorSuccess;
}

extern "C" REX_FUNC(band_NetDll_XNetUnregisterInAddr) {
    if (!Liveless()) return __imp__band_NetDll_XNetUnregisterInAddr(ctx, base);
    ctx.r3.u64 = kErrorSuccess;
}

// socket(af, type, protocol): Live's VDP sockets are plain UDP, as
// RB3Enhanced has them
extern "C" REX_FUNC(band_NetDll_socket) {
    constexpr uint32_t kVdp = 254, kUdp = 17;
    if (band3::online::LiveSpoofed() && ctx.r5.u32 == kVdp) ctx.r5.u64 = kUdp;
    __imp__band_NetDll_socket(ctx, base);
}

// bind(socket, sockaddr*, length): the game's online socket goes on
// liveless_port, if that isn't RB3Enhanced's 9103, and its others near it
// (9100, for finding games on the local network) move with it, so a second
// band3 on this PC can play the first. The online socket is noted for Liveless
// Rooms, whose NAT punches go from it.
extern "C" REX_FUNC(band_NetDll_bind) {
    constexpr uint16_t kFirstGamePort = 9100;
    const uint32_t handle = ctx.r3.u32, name = ctx.r4.u32;
    const int shift = band3::online::LivelessPort() - band3::online::kGamePort;
    const uint16_t port = name && ctx.r5.u32 >= 4 ? REX_LOAD_U16(name + 2) : 0;
    if (!Liveless() || shift == 0 || port < kFirstGamePort || port > band3::online::kGamePort) {
        __imp__band_NetDll_bind(ctx, base);
    } else {
        // the caller's sockaddr keeps its port
        REX_STORE_U16(name + 2, static_cast<uint16_t>(port + shift));
        __imp__band_NetDll_bind(ctx, base);
        REX_STORE_U16(name + 2, port);
    }
    if (Liveless() && port == band3::online::kGamePort && ctx.r3.u32 == 0) {
        band3::rooms::NoteGameSocket(handle);
    }
}

// BandMatchmaker::HasCompatibleInstruments's answer for the game found, in
// r3 (before Matchmaker::OnSearchFinished tests it): it knows nothing of who
// plays what in the game to join, so it's taken as compatible, as
// RB3Enhanced's nop of the beq after it
void LivelessAnyInstruments(PPCRegister& r3) {
    if (Liveless()) r3.u64 = 1;
}

// Quazal's QueuingSocket writes and reads VDP's framing around each packet;
// RB3Enhanced's games send plain UDP and skip these calls
bool LivelessPlainPackets() {
    return Liveless();
}

// Joining a game, step by step, logged with log_net_calls: an invite the
// player accepted (XSessionSearcher, JoinInvitePanel), then NetSession's join
// on both sides, the host's verdict on it and the joiner's result. An invite
// goes: XSessionSearcher::OnMsg reads it and probes the game it's for,
// RVSessionSearcher::Poll hands the probe's answer on as an
// InviteAcceptedMsg, JoinInvitePanel::TryJoin joins it.

extern "C" void __imp__XSessionSearcher__OnMsg(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__InviteAcceptedMsg_ct(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__JoinInvitePanel__TryJoin(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__JoinInvitePanel__SetPresence(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__NetSession__Join(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__NetSession__CheckJoinable(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__JoinResultMsg_ct(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__rex_sub_82651248(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__Quazal__DuplicatedObject__IsADuplicationMaster(PPCContext& ctx,
                                                                       uint8_t* base);

namespace {

// the BandUI's "joined through an invite" flag (TheBandUI + 152), which spares
// the joiner the host's checks for private and Quickplay-only games
constexpr uint32_t kJoinedByInvite = 0x82DFD348;

}  // namespace

// XSessionSearcher::OnMsg(const InviteAcceptedMsg&): keeps the pad that
// accepted (this + 152), reads the invite and probes the game it's for;
// returns 0 when the invite is for another title
extern "C" REX_FUNC(XSessionSearcher__OnMsg) {
    const uint32_t self = ctx.r3.u32;
    __imp__XSessionSearcher__OnMsg(ctx, base);
    if (LogJoins()) {
        REXLOG_INFO("liveless: XSessionSearcher::OnMsg(invite accepted), pad {} -> {}",
                    REX_LOAD_U32(self + 152), ctx.r3.u32);
    }
}

// InviteAcceptedMsg(pad, ?, failed)
extern "C" REX_FUNC(InviteAcceptedMsg_ct) {
    if (LogJoins()) {
        REXLOG_INFO("liveless: InviteAcceptedMsg({}, {}, {})", ctx.r4.u32, ctx.r5.u32,
                    ctx.r6.u32);
    }
    __imp__InviteAcceptedMsg_ct(ctx, base);
}

extern "C" REX_FUNC(JoinInvitePanel__TryJoin) {
    if (LogJoins()) REXLOG_INFO("liveless: JoinInvitePanel::TryJoin");
    __imp__JoinInvitePanel__TryJoin(ctx, base);
}

// the presence (and so the step) JoinInvitePanel's join is at
extern "C" REX_FUNC(JoinInvitePanel__SetPresence) {
    if (LogJoins()) REXLOG_INFO("liveless: JoinInvitePanel::SetPresence({})", ctx.r4.u32);
    __imp__JoinInvitePanel__SetPresence(ctx, base);
}

extern "C" REX_FUNC(NetSession__Join) {
    if (LogJoins()) {
        REXLOG_INFO("liveless: NetSession::Join, session state {}", REX_LOAD_U32(ctx.r3.u32 + 104));
    }
    __imp__NetSession__Join(ctx, base);
}

// NetSession::CheckJoinable(this, int* error, int* extra, joiners, data): the
// host's verdict on a join, true to let it in; only a join turned away has
// its error and extra written. A join let in is the harness's `joined`.
extern "C" REX_FUNC(NetSession__CheckJoinable) {
    const uint32_t error = ctx.r4.u32, extra = ctx.r5.u32;
    __imp__NetSession__CheckJoinable(ctx, base);
    const bool joinable = ctx.r3.u32 & 0xFF;
    if (joinable) band3::test::GameState::Get().SetJoined();
    if (!LogJoins()) return;
    if (joinable) {
        REXLOG_INFO("liveless: NetSession::CheckJoinable -> joinable");
    } else {
        REXLOG_INFO("liveless: NetSession::CheckJoinable -> turned away, error {}, extra {}",
                    error ? REX_LOAD_U32(error) : 0, extra ? REX_LOAD_U32(extra) : 0);
    }
}

// JoinResultMsg(error, extra): what the joiner hears of its join, error 0 on
// success, which is the harness's `joined`
extern "C" REX_FUNC(JoinResultMsg_ct) {
    if (ctx.r4.s32 == 0) band3::test::GameState::Get().SetJoined();
    if (LogJoins()) {
        REXLOG_INFO("liveless: JoinResultMsg(error {}, extra {})", ctx.r4.s32, ctx.r5.s32);
    }
    __imp__JoinResultMsg_ct(ctx, base);
}

// the joiner's side of the join's data: whether it came through an invite
// (the BandUI's flag, or r5) decides which of the host's checks apply
extern "C" REX_FUNC(rex_sub_82651248) {
    if (LogJoins()) {
        REXLOG_INFO("liveless: join data, joined by invite {}, r5 {:#x}",
                    REX_LOAD_U8(kJoinedByInvite), ctx.r5.u32);
    }
    __imp__rex_sub_82651248(ctx, base);
}

// NetSession::IsHost asks this of the Quazal session, which is gone once a
// join is turned away: none is no master, rather than a read of address 20
extern "C" REX_FUNC(Quazal__DuplicatedObject__IsADuplicationMaster) {
    if (ctx.r3.u32 == 0) {
        static bool logged = false;
        if (!logged) {
            REXLOG_WARN("liveless: no Quazal session to be the master of; taken as not the host");
            logged = true;
        }
        ctx.r3.u64 = 0;
        return;
    }
    __imp__Quazal__DuplicatedObject__IsADuplicationMaster(ctx, base);
}

// Invites. Live's are gone, so band3 makes one up (FakeInvite, the harness's
// liveless_invite): the game is told the player accepted it, as the console's
// guide tells it, and reads it for the game at the address it was given.

extern "C" void __imp__XInviteGetAcceptedInfo(PPCContext& ctx, uint8_t* base);

namespace {

// XN_LIVE_INVITE_ACCEPTED, its parameter the user who accepted: PlatformMgr::Poll
// turns it into an InviteAcceptedMsg for that pad
constexpr uint32_t kXnLiveInviteAccepted = 0x02000002;
// XSessionSearcher::OnMsg drops invites for other titles
constexpr uint32_t kRb3TitleId = 0x45410914;

// XINVITE_INFO: xuidInviter, xuidInvitee, dwTitleID, XNKID session, XNADDR
// host, XNKEY key exchange key, fFromGameInvite
constexpr uint32_t kInviteTitleId = 16;
constexpr uint32_t kInviteSession = 20;
constexpr uint32_t kInviteHost = 28;
constexpr uint32_t kInviteKey = 64;
constexpr uint32_t kInviteFromGame = 80;
constexpr uint32_t kInviteSize = 84;

struct Invite {
    bool pending = false;
    uint32_t address = 0;  // network order
    uint16_t port = 0;
};

std::mutex g_invite_mutex;
// made up, until the game reads it
Invite g_invite;

}  // namespace

namespace band3::online {

std::string FakeInvite(const std::string& host, uint16_t port, bool force_flag) {
    if (!Liveless()) return "liveless is off";
    auto* kernel = REX_KERNEL_STATE();
    if (!kernel) return "the game isn't running yet";
    const auto found = band3::net::ResolveIPv4(host);
    if (found.empty()) return "no IPv4 address for " + host;
    if (!SetLivelessJoin(host, port)) return "out of guest memory";
    {
        std::lock_guard<std::mutex> lock(g_invite_mutex);
        g_invite = {true, found.front(), port};
    }
    if (force_flag) {
        rex::system::kernel_memory()->virtual_membase()[kJoinedByInvite] = 1;
        REXLOG_INFO("liveless: the BandUI's joined-by-invite flag set");
    }
    kernel->BroadcastNotification(kXnLiveInviteAccepted, 0);
    REXLOG_INFO("liveless: invite notification sent, for player 1 to join {}:{}", host, port);
    return {};
}

}  // namespace band3::online

// XInviteGetAcceptedInfo(user, XINVITE_INFO*): the invite band3 made up, once,
// for the game to join; Live's otherwise
extern "C" REX_FUNC(XInviteGetAcceptedInfo) {
    const uint32_t user = ctx.r3.u32, info = ctx.r4.u32;
    Invite invite;
    if (info) {
        std::lock_guard<std::mutex> lock(g_invite_mutex);
        invite = g_invite;
        g_invite.pending = false;
    }
    if (!invite.pending) {
        __imp__XInviteGetAcceptedInfo(ctx, base);
        if (LogJoins()) REXLOG_INFO("liveless: XInviteGetAcceptedInfo({}) -> {:#x}", user, ctx.r3.u32);
        return;
    }
    std::memset(base + info, 0, kInviteSize);
    REX_STORE_U32(info + kInviteTitleId, kRb3TitleId);
    // the session's ID and key, made up as the search's are
    for (uint32_t i = 0; i < 8; i++) base[info + kInviteSession + i] = static_cast<uint8_t>(i + 1);
    const uint32_t host = info + kInviteHost;
    StoreNetworkOrder(base, host + kXnAddrIna, invite.address);
    StoreNetworkOrder(base, host + kXnAddrInaOnline, invite.address);
    REX_STORE_U16(host + kXnAddrPortOnline, invite.port);
    for (uint32_t i = 0; i < 20; i++) base[host + kXnAddrOnline + i] = static_cast<uint8_t>(i);
    for (uint32_t i = 0; i < 16; i++) base[info + kInviteKey + i] = static_cast<uint8_t>(i + 1);
    REX_STORE_U32(info + kInviteFromGame, 1);
    ctx.r3.u64 = kErrorSuccess;
    REXLOG_INFO("liveless: XInviteGetAcceptedInfo({}) gave the invite, to port {}", user,
                invite.port);
}
