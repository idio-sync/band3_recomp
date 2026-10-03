#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <atomic>
#include <cstring>
#include <string>
#include <string_view>
#include "generated/band3_init.h"
#include "src/Net/local_address.h"
#include "src/Net/native_socket.h"
#include "src/Net/online.h"
#include "src/Net/online_hooks.h"
#include "src/settings.h"
#ifdef _WIN32
#include <windows.h>
#endif

// RB3 online without Xbox Live, as RB3Enhanced's xbox360_liveless.c has it on
// a console with Live blocked. RB3 goes online through Quazal, behind Live's
// secure gateway, as a player signed in to Live. For either GoCentral (Rock
// Central's online features) or Liveless (online play, liveless.cpp), these
// report players as signed in to Live and have Quazal skip the secure gateway
// and use plain sockets. GoCentral then sends Rock Central's addresses to the
// GoCentral server, and Liveless the game to join to the player's address.

extern "C" void __imp__Quazal__InetAddress__SetAddress(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__Inet__UseSecureSockets(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__Quazal__StepSequenceJob__SetStep(PPCContext& ctx, uint8_t* base);

namespace {

using Export = void(PPCContext&, uint8_t*);

// set before the game runs: OnPostSetup aborts if any is missing
Export* g_sdk_get_signin_state = nullptr;
Export* g_sdk_get_signin_info = nullptr;

// XUSER_SIGNIN_STATE
constexpr uint32_t kSignedInLocally = 1;
constexpr uint32_t kSignedInToLive = 2;
// XUSER_SIGNIN_INFO's UserSigninState
constexpr uint32_t kSigninInfoState = 0x0C;
// StepSequenceJobStep's name, on the Xbox 360
constexpr uint32_t kStepName = 0x08;

bool g_live = false;
bool g_gocentral = false;
bool g_liveless = false;
// names in guest memory for Quazal::InetAddress::SetAddress: the GoCentral
// server, and the Liveless game to join, which an invite (SetLivelessJoin)
// changes while the game runs
uint32_t g_gocentral_server = 0;
std::atomic<uint32_t> g_liveless_join{0};
std::atomic<uint16_t> g_liveless_join_port{band3::online::kGamePort};
uint16_t g_liveless_port = band3::online::kGamePort;
uint32_t g_liveless_external = 0;

std::string_view GuestString(uint8_t* base, uint32_t address, size_t max = 256) {
    if (!address) return {};
    const char* s = reinterpret_cast<const char*>(base + address);
    return {s, strnlen(s, max)};
}

uint32_t GuestCopy(const std::string& text) {
    auto* memory = rex::system::kernel_memory();
    const auto size = static_cast<uint32_t>(text.size() + 1);
    const uint32_t address = memory->SystemHeapAlloc(size, 4);
    if (address) std::memcpy(memory->virtual_membase() + address, text.c_str(), size);
    return address;
}

std::string Ipv4(uint32_t address) {
    // network order
    return std::to_string(address & 0xFF) + "." + std::to_string((address >> 8) & 0xFF) + "." +
           std::to_string((address >> 16) & 0xFF) + "." + std::to_string(address >> 24);
}

bool StartGoCentral() {
    const auto& startup = band3::settings::Startup();
    if (!startup.gocentral) return false;
    if (!band3::online::IsOwnAccountName(band3::settings::Username())) {
        REXLOG_ERROR("gocentral: not connecting as '{}': set username to a name of your own "
                     "first, it's your account on GoCentral", band3::settings::Username());
        return false;
    }
    std::string address = startup.gocentral_address;
    if (address.empty()) address = band3::online::kDefaultGoCentral;
    g_gocentral_server = GuestCopy(address);
    if (!g_gocentral_server) return false;
    REXLOG_INFO("gocentral: Rock Central goes to {}, as {}", address, band3::settings::Username());
    return true;
}

bool StartLiveless() {
    const auto& startup = band3::settings::Startup();
    if (!startup.liveless) return false;
    const auto join = band3::online::ParseJoinAddress(
        startup.liveless_connect, static_cast<uint16_t>(startup.liveless_port));
    if (!join) {
        REXLOG_ERROR("liveless: liveless_connect '{}' isn't an address (host or host:port)",
                     startup.liveless_connect);
        return false;
    }
    g_liveless_join = GuestCopy(join->host);
    if (!g_liveless_join) return false;
    g_liveless_join_port = join->port;
    g_liveless_port = static_cast<uint16_t>(startup.liveless_port);
    // the address told to whoever joins: the setting's, or this PC's on the
    // local network
    std::string external = startup.liveless_external_ip;
    if (external.empty()) external = band3::net::LocalAddress();
    const auto found = band3::net::ResolveIPv4(external);
    g_liveless_external = found.empty() ? 0 : found.front();
    REXLOG_INFO("liveless: playing online on port {} as {}, joining {}:{}", g_liveless_port,
                g_liveless_external ? Ipv4(g_liveless_external) : std::string("(no address)"),
                join->host, join->port);
    return true;
}

}  // namespace

namespace band3::online {

bool ResolveSdkExports() {
#ifdef _WIN32
    HMODULE runtime = GetModuleHandleA(BAND3_REXRUNTIME_DLL);
    auto find = [&](const char* name, Export*& out) {
        out = runtime ? reinterpret_cast<Export*>(GetProcAddress(runtime, name)) : nullptr;
        if (!out) REXLOG_ERROR("online: {} has no {}", BAND3_REXRUNTIME_DLL, name);
        return out != nullptr;
    };
    bool ok = find("__imp__XamUserGetSigninState", g_sdk_get_signin_state);
    ok &= find("__imp__XamUserGetSigninInfo", g_sdk_get_signin_info);
    return ok;
#else
    return false;
#endif
}

void Start() {
#ifndef _WIN32
    // the overrides that make Quazal's networking work are Windows only
    // (net_calls.cpp), as are the sign-in ones below
    if (settings::Startup().gocentral || settings::Startup().liveless) {
        REXLOG_ERROR("online: GoCentral and Liveless only work on Windows for now");
    }
#else
    g_gocentral = StartGoCentral();
    g_liveless = StartLiveless();
    g_live = g_gocentral || g_liveless;
#endif
}

bool LiveSpoofed() { return g_live; }
bool GoCentral() { return g_gocentral; }
bool Liveless() { return g_liveless; }
uint16_t LivelessJoinPort() { return g_liveless_join_port; }
uint16_t LivelessPort() { return g_liveless_port; }
uint32_t LivelessExternalAddress() { return g_liveless_external; }

bool SetLivelessJoin(const std::string& host, uint16_t port) {
    // the old name stays allocated: Quazal may be looking it up as it changes
    const uint32_t name = GuestCopy(host);
    if (!name) return false;
    g_liveless_join_port = port;
    g_liveless_join = name;
    REXLOG_INFO("liveless: the game to join is now {}:{}", host, port);
    return true;
}

}  // namespace band3::online

// Quazal::InetAddress::SetAddress(InetAddress*, const char* host): where Quazal
// looks up each address it connects to by name
extern "C" REX_FUNC(Quazal__InetAddress__SetAddress) {
    const auto host = GuestString(base, ctx.r4.u32);
    if (g_gocentral && band3::online::IsRockCentralHost(host)) {
        REXLOG_INFO("gocentral: {} -> {}", host, GuestString(base, g_gocentral_server));
        ctx.r4.u64 = g_gocentral_server;
    } else if (g_liveless && host == band3::online::kJoinStandInName) {
        const uint32_t join = g_liveless_join;
        REXLOG_INFO("liveless: joining {}", GuestString(base, join));
        ctx.r4.u64 = join;
    }
    __imp__Quazal__InetAddress__SetAddress(ctx, base);
}

// Quazal's packets go unsigned, as GoCentral and RB3Enhanced's take them
extern "C" REX_FUNC(Inet__UseSecureSockets) {
    if (g_live) {
        ctx.r3.u64 = 0;
        return;
    }
    __imp__Inet__UseSecureSockets(ctx, base);
}

// Quazal's jobs (its logins among them) step through these: how far one got
extern "C" REX_FUNC(Quazal__StepSequenceJob__SetStep) {
    if (REXCVAR_GET(log_net_calls) && ctx.r4.u32) {
        const auto name = GuestString(base, REX_LOAD_U32(ctx.r4.u32 + kStepName), 128);
        if (!name.empty()) REXLOG_INFO("online: Quazal job step {}", name);
    }
    __imp__Quazal__StepSequenceJob__SetStep(ctx, base);
}

// Quazal::XboxClient::Login2 reads whether to bypass the secure gateway into
// r11 (lbz r11 at 0x82A88268); RB3Enhanced makes it li r11,1
void GoCentralBypassSecureGateway(PPCRegister& r11) {
    if (g_live) r11.u64 = 1;
}

// RockCentralGateway::Poll only logs in once a flag it reads into r11 (lbz at
// 0x824F7170) is set, which needs Xbox Live; with GoCentral it logs in
// regardless, as RB3Enhanced's nop of the beq after it
void GoCentralRockCentralLogin(PPCRegister& r11) {
    if (g_gocentral) r11.u64 = 1;
}

#ifdef _WIN32
// Players signed in to the console count as signed in to Live, which RB3
// wants before it goes online. These hand on to the SDK's exports in its DLL,
// so they're Windows only; elsewhere the game calls the SDK's own.
extern "C" REX_FUNC(__imp__XamUserGetSigninState) {
    (*g_sdk_get_signin_state)(ctx, base);
    if (g_live && ctx.r3.u32 == kSignedInLocally) ctx.r3.u64 = kSignedInToLive;
}

extern "C" REX_FUNC(__imp__XamUserGetSigninInfo) {
    const uint32_t info = ctx.r5.u32;
    (*g_sdk_get_signin_info)(ctx, base);
    if (g_live && ctx.r3.u32 == 0 && info &&
        REX_LOAD_U32(info + kSigninInfoState) == kSignedInLocally) {
        REX_STORE_U32(info + kSigninInfoState, kSignedInToLive);
    }
}
#endif

// XUserCheckPrivilege(user, privilege, BOOL* result): online play, voice,
// content from others all allowed, as RB3Enhanced has it (it calls XAM's
// export through this guest wrapper)
extern "C" void __imp__XUserCheckPrivilege(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(XUserCheckPrivilege) {
    if (g_live && ctx.r5.u32) {
        REX_STORE_U32(ctx.r5.u32, 1);
        ctx.r3.u64 = 0;
        return;
    }
    __imp__XUserCheckPrivilege(ctx, base);
}
