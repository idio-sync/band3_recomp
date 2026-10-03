#include <rex/hook.h>
#include <rex/logging.h>
#include <algorithm>
#include <cstring>
#include "src/sdk_export.h"
#include "src/settings.h"

extern "C" void __imp__PlatformMgr__GetName(PPCContext& ctx, uint8_t* base);

namespace {

// the name buffer is sized for a gamertag (XUSER_NAME_SIZE = 16 incl. terminator)
constexpr size_t kMaxNameLen = 15;

void WriteUsername(uint8_t* base, uint32_t buffer, size_t size) {
    const std::string username = band3::settings::Username();
    if (username.empty() || !buffer || size == 0) return;
    char* buf = reinterpret_cast<char*>(base + buffer);
    const size_t len = std::min({username.size(), kMaxNameLen, size - 1});
    std::memcpy(buf, username.c_str(), len);
    buf[len] = '\0';
}

}  // namespace

extern "C" REX_FUNC(PlatformMgr__GetName)
{
    __imp__PlatformMgr__GetName(ctx, base);
    WriteUsername(base, ctx.r3.u32, kMaxNameLen + 1);
}

// XamUserGetName(user index, char* buffer, size): the gamertag, which
// XboxServer::FindPadToLogin logs into Rock Central (GoCentral) with. It hands
// on to the SDK's export.
extern "C" REX_FUNC(__imp__XamUserGetName)
{
    static band3::SdkFunction* const sdk = band3::SdkExport("__imp__XamUserGetName");
    const uint32_t buffer = ctx.r4.u32, size = ctx.r5.u32;
    if (!sdk) {
        ctx.r3.u64 = 0x80004005;  // E_FAIL
        return;
    }
    const uint32_t user = ctx.r3.u32;
    sdk(ctx, base);
    if (ctx.r3.u32 != 0) return;
    WriteUsername(base, buffer, size);
    if (REXCVAR_GET(log_net_calls) && buffer) {
        REXLOG_INFO("profile: XamUserGetName({}) -> {}", user,
                    reinterpret_cast<const char*>(base + buffer));
    }
}
