#include "Script.h"
#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <cstring>

REX_EXTERN(RockCentralGateway__ExecuteConfig);

namespace band3 {

namespace {

constexpr uint32_t kRockCentralGateway = 0x82CC8F60;  // RockCentralGateway object

}

void RunScript(PPCContext& ctx, uint8_t* base, const std::string& script) {
    auto* memory = rex::system::kernel_memory();
    const auto size = static_cast<uint32_t>(script.size() + 1);
    const uint32_t text = memory->SystemHeapAlloc(size, 4);
    if (!text) return;
    std::memcpy(base + text, script.c_str(), size);

    // RockCentralGateway::ExecuteConfig(RockCentralGateway*, const char* dta),
    // on a stack below the caller's frame, with its r13
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - 0x100;
    call.r13 = ctx.r13;
    call.r3.u64 = kRockCentralGateway;
    call.r4.u64 = text;
    RockCentralGateway__ExecuteConfig(call, base);

    memory->SystemHeapFree(text);
}

}
