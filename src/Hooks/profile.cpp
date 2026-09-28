#include <rex/hook.h>
#include <rex/logging.h>
#include <cstring>
#include "src/config.h"

extern "C" void __imp__PlatformMgr__GetName(PPCContext& ctx, uint8_t* base);

extern "C" REX_FUNC(PlatformMgr__GetName)
{
    __imp__PlatformMgr__GetName(ctx, base);

    // the name buffer is sized for a gamertag (XUSER_NAME_SIZE = 16 incl. terminator)
    constexpr size_t kMaxNameLen = 15;

    auto& username = band3::GetConfig().username;
    if (!username.empty() && ctx.r3.u32) {
        char* buf = reinterpret_cast<char*>(base + ctx.r3.u32);
        size_t len = username.size();
        if (len > kMaxNameLen) len = kMaxNameLen;
        std::memcpy(buf, username.c_str(), len);
        buf[len] = '\0';
    }
}
