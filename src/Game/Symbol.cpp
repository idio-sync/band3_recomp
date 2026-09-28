#include "Symbol.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <cstring>

REX_EXTERN(Symbol__Symbol);

// these should be good...
static constexpr size_t kMaxSymbolNameLen = 256;
static constexpr uint32_t kScratchSize = kMaxSymbolNameLen + 8 + 256;

namespace band3 {

Symbol::Symbol(PPCContext& ctx, uint8_t* base, const char* name) {
    if (!name || name[0] == '\0') {
        REXLOG_ERROR("Symbol::Symbol called with null or empty name");
        guest_addr_ = 0;
        return;
    }

	// sanity checks on symbols, realistically this should never hit
	// this function is *not* a hook for the original Symbol constructor but some host-side grease, so the game itself can still exceed this
    size_t name_len = strlen(name) + 1;
    if (name_len > kMaxSymbolNameLen) {
        REXLOG_ERROR("Symbol name too long ({} bytes, max {}): \"{}\"",
                     name_len - 1, kMaxSymbolNameLen - 1, name);
        guest_addr_ = 0;
        return;
    }

	// shitty and unreadable code alert
	// i love host<-->guest interop
    uint32_t scratch = ctx.r1.u32 - kScratchSize;

    uint32_t str_addr = scratch;
    memcpy(base + str_addr, name, name_len);

    guest_addr_ = (str_addr + static_cast<uint32_t>(name_len) + 7) & ~7u;
	
    PPCContext symCtx{};
    symCtx.r1.u64 = scratch - 256;
    symCtx.r13 = ctx.r13;
    symCtx.r3.u64 = guest_addr_;
    symCtx.r4.u64 = str_addr;
    Symbol__Symbol(symCtx, base);

    guest_addr_ = symCtx.r3.u32;
}

uint32_t Symbol::value(uint8_t* base) const {
    // construction failed; don't read guest address 0
    if (!guest_addr_) return 0;
    return __builtin_bswap32(
        *reinterpret_cast<uint32_t*>(base + guest_addr_));
}

}
