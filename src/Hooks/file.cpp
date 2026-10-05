#include <rex/hook.h>
#include <rex/types.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <atomic>
#include <cstring>
#include <string>
#include <string_view>
#include "src/game_writes.h"
#include "src/Hooks/loose_name.h"

using namespace rex::ppc;

extern "C" void __imp__NewFile(PPCContext& ctx, uint8_t* base);

namespace {

using band3::kMaxLooseName;
using band3::LooseName;

// guest buffers for the names NewFile is given in place of the game's, used in
// turn: the game copies the name before the next open can come round to it
constexpr uint32_t kNameSlots = 32;
constexpr uint32_t kNameSlotBytes = 256;
static_assert(kMaxLooseName < kNameSlotBytes);

// a guest copy of name, for NewFile to open; 0 if there's no room for one
uint32_t GuestName(uint8_t* base, const std::string& name) {
    static const uint32_t slots =
        rex::system::kernel_memory()->SystemHeapAlloc(kNameSlots * kNameSlotBytes, 4);
    static std::atomic<uint32_t> next{0};
    if (!slots) return 0;
    const uint32_t slot = slots + (next++ % kNameSlots) * kNameSlotBytes;
    std::memcpy(base + slot, name.c_str(), name.size() + 1);
    return slot;
}

}  // namespace

// NewFile(const char* name, int mode): a file in game:\ at the name's ARK path
// is read from there ("raw", mode 0x10000) instead of from the ARK
extern "C" REX_FUNC(NewFile) {
    const uint32_t name_addr = ctx.r3.u32;
    const uint32_t flags = ctx.r4.u32;

    if (name_addr && name_addr < 0xFFFF0000) {
        const char* name = reinterpret_cast<const char*>(base + name_addr);
        const std::string_view ark_path(name, strnlen(name, 1024));
        // a printable name, and not a device path (dlc0:, game:...), which the
        // game opens as it is
        if (!ark_path.empty() && ark_path[0] >= 0x20 && ark_path[0] <= 0x7E &&
            ark_path.find(':') == std::string_view::npos) {
            const std::string loose = LooseName(ark_path);
            if (band3::GameFileExists(loose)) {
                if (loose.size() > kMaxLooseName) {
                    REXLOG_WARN("NewFile: {} is in the game folder, but its path is over {} "
                                "characters, so the ARK's is read",
                                loose, kMaxLooseName);
                } else if (const uint32_t open = loose == ark_path ? name_addr : GuestName(base, loose)) {
                    // ARK opens are nearly every file, so only loose-file overrides log at info
                    REXLOG_INFO("NewFile: {} [flags={:#x}]", loose, flags);
                    ctx.r3.u64 = open;
                    ctx.r4.u64 = flags | 0x10000;
                }
            } else {
                REXLOG_DEBUG("NewFile: {} (ARK) [flags={:#x}]", loose, flags);
            }
        }
    }

    __imp__NewFile(ctx, base);
}
