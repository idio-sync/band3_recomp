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
#include "src/Net/gocentral.h"
#include "src/Net/gocentral_hooks.h"
#include "src/settings.h"
#ifdef _WIN32
#include <windows.h>
#endif

// GoCentral: RB3Enhanced's way into a fan-run Rock Central. RB3 logs into
// Rock Central through Quazal, behind Xbox Live's secure gateway, as a player
// signed in to Live. These hooks do what RB3Enhanced's xbox360_liveless.c does
// on a console with Live blocked: report players as signed in to Live, have
// Quazal log in without the secure gateway and use plain sockets, and send its
// Rock Central addresses to the GoCentral server.

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

bool g_enabled = false;
// the server's name in guest memory, for Quazal::InetAddress::SetAddress
uint32_t g_server = 0;

std::string_view GuestString(uint8_t* base, uint32_t address, size_t max = 256) {
    if (!address) return {};
    const char* s = reinterpret_cast<const char*>(base + address);
    return {s, strnlen(s, max)};
}

}  // namespace

namespace band3::gocentral {

bool ResolveSdkExports() {
#ifdef _WIN32
    HMODULE runtime = GetModuleHandleA(BAND3_REXRUNTIME_DLL);
    auto find = [&](const char* name, Export*& out) {
        out = runtime ? reinterpret_cast<Export*>(GetProcAddress(runtime, name)) : nullptr;
        if (!out) REXLOG_ERROR("gocentral: {} has no {}", BAND3_REXRUNTIME_DLL, name);
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
    const auto& startup = settings::Startup();
    if (!startup.gocentral) return;
    if (!IsOwnAccountName(settings::Username())) {
        REXLOG_ERROR("gocentral: not connecting as '{}': set username to a name of your own "
                     "first, it's your account on GoCentral", settings::Username());
        return;
    }
    std::string address = startup.gocentral_address;
    if (address.empty()) address = kDefaultAddress;
    auto* memory = rex::system::kernel_memory();
    const auto size = static_cast<uint32_t>(address.size() + 1);
    g_server = memory->SystemHeapAlloc(size, 4);
    if (!g_server) {
        REXLOG_ERROR("gocentral: no guest memory for the server's name");
        return;
    }
    std::memcpy(memory->virtual_membase() + g_server, address.c_str(), size);
    g_enabled = true;
    REXLOG_INFO("gocentral: Rock Central goes to {}, as {}", address, settings::Username());
}

}  // namespace band3::gocentral

// Quazal::InetAddress::SetAddress(InetAddress*, const char* host): where Quazal
// looks up each server it connects to by name
extern "C" REX_FUNC(Quazal__InetAddress__SetAddress) {
    if (g_enabled) {
        const auto host = GuestString(base, ctx.r4.u32);
        if (band3::gocentral::IsRockCentralHost(host)) {
            REXLOG_INFO("gocentral: {} -> {}", host, GuestString(base, g_server));
            ctx.r4.u64 = g_server;
        }
    }
    __imp__Quazal__InetAddress__SetAddress(ctx, base);
}

// GoCentral takes Quazal's packets unsigned
extern "C" REX_FUNC(Inet__UseSecureSockets) {
    if (g_enabled) {
        ctx.r3.u64 = 0;
        return;
    }
    __imp__Inet__UseSecureSockets(ctx, base);
}

// Quazal's login jobs step through these: how far a login got
extern "C" REX_FUNC(Quazal__StepSequenceJob__SetStep) {
    if (REXCVAR_GET(log_net_calls) && ctx.r4.u32) {
        const auto name = GuestString(base, REX_LOAD_U32(ctx.r4.u32 + kStepName), 128);
        if (!name.empty()) REXLOG_INFO("gocentral: Quazal job step {}", name);
    }
    __imp__Quazal__StepSequenceJob__SetStep(ctx, base);
}

// Quazal::XboxClient::Login2 reads whether to bypass the secure gateway into
// r11 (lbz r11 at 0x82A88268); RB3Enhanced makes it li r11,1
void GoCentralBypassSecureGateway(PPCRegister& r11) {
    if (g_enabled) r11.u64 = 1;
}

// RockCentralGateway::Poll only logs in once a flag it reads into r11 (lbz at
// 0x824F7170) is set, which needs Xbox Live; RockCentralGateway's login waits
// on it no longer, as RB3Enhanced's nop of the beq after it
void GoCentralRockCentralLogin(PPCRegister& r11) {
    if (g_enabled) r11.u64 = 1;
}

// Players signed in to the console count as signed in to Live, which RB3
// wants before it goes online
extern "C" REX_FUNC(__imp__XamUserGetSigninState) {
    (*g_sdk_get_signin_state)(ctx, base);
    if (g_enabled && ctx.r3.u32 == kSignedInLocally) ctx.r3.u64 = kSignedInToLive;
}

extern "C" REX_FUNC(__imp__XamUserGetSigninInfo) {
    const uint32_t info = ctx.r5.u32;
    (*g_sdk_get_signin_info)(ctx, base);
    if (g_enabled && ctx.r3.u32 == 0 && info &&
        REX_LOAD_U32(info + kSigninInfoState) == kSignedInLocally) {
        REX_STORE_U32(info + kSigninInfoState, kSignedInToLive);
    }
}
