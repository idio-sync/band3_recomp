#include "Modifiers.h"
#include <rex/hook.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include "Symbol.h"

REX_EXTERN(ModifierMgr__GetModifier);

namespace band3::modifiers {

namespace {

constexpr uint32_t kTheModifierMgrPtr = 0x82DFEC08;  // ModifierMgr*
// Modifier: {DataArray* mData, bool mDefaultEnabled}, the second being
// whether it's on (ModifierMgr::IsModifierActive reads it)
constexpr uint32_t kModifier_Enabled = 0x4;

std::mutex g_symbols_mutex;
std::unordered_map<std::string, uint32_t> g_symbols;

}

uint32_t Intern(PPCContext& ctx, uint8_t* base, const char* name) {
    // the song loader parses on its own thread, so the cache is shared
    std::lock_guard lock(g_symbols_mutex);
    auto it = g_symbols.find(name);
    if (it != g_symbols.end()) return it->second;
    const uint32_t symbol = band3::Symbol(ctx, base, name).value(base);
    if (symbol) g_symbols.emplace(name, symbol);
    return symbol;
}

bool Active(PPCContext& ctx, uint8_t* base, const char* name) {
    const uint32_t mgr = *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, kTheModifierMgrPtr);
    const uint32_t symbol = Intern(ctx, base, name);
    if (!mgr || !symbol) return false;

    // ModifierMgr::GetModifier(ModifierMgr*, Symbol, bool fail), not failing
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - 0x400;
    call.r13 = ctx.r13;
    call.r3.u64 = mgr;
    call.r4.u64 = symbol;
    call.r5.u64 = 0;
    ModifierMgr__GetModifier(call, base);
    const uint32_t modifier = call.r3.u32;
    return modifier &&
           *rex::memory::GuestPtr<uint8_t*>(base, modifier + kModifier_Enabled) != 0;
}

}
