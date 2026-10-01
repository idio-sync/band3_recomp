#include "File.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <cstring>

// File's methods are virtual, and NewFile returns an ArkFile, an AsyncFile or a
// cached file depending on where the file is, so they're called through its
// vtable (the order in the RB3 decomp's system/os/File.h).

REX_EXTERN(NewFile);

namespace band3::files {

namespace {

constexpr uint32_t kOpenRead = 2;  // NewFile's mode for reading

// File's vtable
constexpr uint32_t kFile_DeletingDtor = 0;  // (File*, int flags), 1 frees it
constexpr uint32_t kFile_Read = 2;          // int (File*, void*, int)
constexpr uint32_t kFile_Fail = 10;         // bool (File*)
constexpr uint32_t kFile_Size = 11;         // int (File*)

constexpr size_t kMaxPath = 255;

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

// context for calling a guest function from a hook: a stack below the
// caller's frame, with its r13 (as band3::Symbol does)
PPCContext CallContext(const PPCContext& ctx, uint32_t reserve) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - reserve;
    call.r13 = ctx.r13;
    return call;
}

// calls the file's virtual method `slot`; the arguments after `this` are set in `call`
void CallVirtual(PPCContext& call, uint8_t* base, uint32_t file, uint32_t slot) {
    const uint32_t method = Load32(base, Load32(base, file) + slot * 4);
    call.r3.u64 = file;
    rex::runtime::ResolveIndirectFunction(method)(call, base);
}

}

std::optional<std::string> ReadAll(PPCContext& ctx, uint8_t* base, const std::string& path,
                                   size_t max_size) {
    if (path.empty() || path.size() > kMaxPath) return std::nullopt;

    // the path below this frame, the calls' stack below that
    const uint32_t guest_path = ctx.r1.u32 - 0x200;
    std::memcpy(base + guest_path, path.c_str(), path.size() + 1);
    PPCContext call = CallContext(ctx, 0x400);
    call.r3.u64 = guest_path;
    call.r4.u64 = kOpenRead;
    NewFile(call, base);
    const uint32_t file = call.r3.u32;
    if (!file) return std::nullopt;

    std::optional<std::string> contents;
    call = CallContext(ctx, 0x400);
    CallVirtual(call, base, file, kFile_Fail);
    if (!(call.r3.u32 & 0xFF)) {
        call = CallContext(ctx, 0x400);
        CallVirtual(call, base, file, kFile_Size);
        const int32_t size = call.r3.s32;
        uint32_t buffer = 0;
        if (size > 0 && static_cast<size_t>(size) <= max_size) {
            buffer = rex::system::kernel_memory()->SystemHeapAlloc(static_cast<uint32_t>(size), 16);
        }
        if (buffer) {
            call = CallContext(ctx, 0x400);
            call.r4.u64 = buffer;
            call.r5.u64 = static_cast<uint32_t>(size);
            CallVirtual(call, base, file, kFile_Read);
            if (call.r3.s32 == size) {
                contents.emplace(reinterpret_cast<const char*>(base + buffer),
                                 static_cast<size_t>(size));
            }
            rex::system::kernel_memory()->SystemHeapFree(buffer);
        }
    }

    call = CallContext(ctx, 0x400);
    call.r4.u64 = 1;
    CallVirtual(call, base, file, kFile_DeletingDtor);
    return contents;
}

}
