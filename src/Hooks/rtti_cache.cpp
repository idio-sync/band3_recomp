#include <rex/hook.h>
#include <rex/types.h>
#include <cstdint>
#include "generated/band3_init.h"

// RTDynamicCast(inptr, VfDelta, SrcType, TargetType, isReference) is MSVC's
// dynamic_cast: from the object's vfptr it finds the complete object locator,
// then walks the class hierarchy descriptor for the target type
// (FindVITargetTypeInstance for the Hmx::Object family's virtual bases, a
// linear search through every base class with a PMD offset computed at each
// step). RB3 casts this way thousands of times a frame, ~0.06 ms of the game's
// thread on a busy venue frame here (more on a weaker CPU). For a complete
// object of a given class the answer, as an offset from inptr, depends only
// on the vfptr (which names the class and the subobject), VfDelta and the two
// type descriptors, so band3 keeps it. The locator's cdOffset, when not 0,
// makes it read a vtordisp from the object too: those casts aren't kept.

namespace {

struct Entry {
    uint32_t vfptr = 0;  // 0: empty
    uint32_t vf_delta = 0;
    uint32_t src = 0;
    uint32_t target = 0;
    int32_t delta = 0;   // result - inptr
    bool found = false;  // false: the cast fails (null, or bad_cast)
};

// direct-mapped, one per thread (casts run on the game's thread and a few
// others; 24 KB of every thread's TLS): a colliding entry replaces the one
// there
constexpr uint32_t kEntries = 1024;
thread_local Entry t_entries[kEntries];

uint32_t Slot(uint32_t vfptr, uint32_t vf_delta, uint32_t src, uint32_t target) {
    uint64_t h = (uint64_t(vfptr) << 32 | target) * 0x9E3779B97F4A7C15ull;
    h ^= (uint64_t(src) << 32 | vf_delta) * 0xC2B2AE3D27D4EB4Full;
    return uint32_t(h >> 54) & (kEntries - 1);
}

}  // namespace

extern "C" void __imp__RTDynamicCast(PPCContext& ctx, uint8_t* base);

// inptr r3, VfDelta r4, SrcType r5, TargetType r6, isReference r7; the
// result in r3
extern "C" REX_FUNC(RTDynamicCast) {
    const uint32_t inptr = ctx.r3.u32;
    if (!inptr) {
        ctx.r3.u64 = 0;
        return;
    }
    const uint32_t vfptr = REX_LOAD_U32(inptr);
    const uint32_t vf_delta = ctx.r4.u32, src = ctx.r5.u32, target = ctx.r6.u32;
    const bool is_reference = ctx.r7.u32 != 0;
    Entry& e = t_entries[Slot(vfptr, vf_delta, src, target)];
    if (vfptr && e.vfptr == vfptr && e.vf_delta == vf_delta && e.src == src &&
        e.target == target && (e.found || !is_reference)) {
        ctx.r3.u64 = e.found ? uint32_t(inptr + uint32_t(e.delta)) : 0;
        return;
    }
    // the complete object locator's cdOffset (+8)
    const uint32_t locator = vfptr ? REX_LOAD_U32(vfptr - 4) : 0;
    const bool keep = locator && REX_LOAD_U32(locator + 8) == 0;
    __imp__RTDynamicCast(ctx, base);
    if (!keep) return;
    const uint32_t result = ctx.r3.u32;
    e = Entry{vfptr, vf_delta, src, target, int32_t(result - inptr), result != 0};
}
