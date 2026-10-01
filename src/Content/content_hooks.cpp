#include "content_hooks.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xam/content_manager.h>
#include <rex/types.h>
#include <string>
#include "generated/band3_init.h"
#ifdef _WIN32
#include <windows.h>
#endif

// RB3 lists DLC and custom songs through XContentCreateCrossTitleEnumerator,
// and opens them, saves included, through these XAM exports. band3 takes them
// over so it can serve packages from content folders (live_content.cpp) and
// hands everything else to the SDK's own.

extern "C" void __imp__XContentCreateCrossTitleEnumerator(PPCContext& ctx, uint8_t* base);

namespace {

using Export = void(PPCContext&, uint8_t*);

Export* g_sdk_create_ex = nullptr;
Export* g_sdk_close = nullptr;
Export* g_sdk_get_creator = nullptr;
Export* g_sdk_create_enumerator = nullptr;

const rex::system::xam::XCONTENT_DATA* ContentData(uint8_t* base, uint32_t addr) {
    return reinterpret_cast<const rex::system::xam::XCONTENT_DATA*>(REX_RAW_ADDR(addr));
}

}  // namespace

namespace band3::content {

bool ResolveSdkContentExports() {
#ifdef _WIN32
    HMODULE runtime = GetModuleHandleA(BAND3_REXRUNTIME_DLL);
    auto find = [&](const char* name, Export*& out) {
        out = runtime ? reinterpret_cast<Export*>(GetProcAddress(runtime, name)) : nullptr;
        if (!out) REXLOG_ERROR("content: {} has no {}", BAND3_REXRUNTIME_DLL, name);
        return out != nullptr;
    };
    bool ok = find("__imp__XamContentCreateEx", g_sdk_create_ex);
    ok &= find("__imp__XamContentClose", g_sdk_close);
    ok &= find("__imp__XamContentGetCreator", g_sdk_get_creator);
    ok &= find("__imp__XamContentCreateEnumerator", g_sdk_create_enumerator);
    return ok;
#else
    return false;
#endif
}

}  // namespace band3::content

extern "C" REX_FUNC(XContentCreateCrossTitleEnumerator) {
    const uint32_t user = ctx.r3.u32, device = ctx.r4.u32, type = ctx.r5.u32;
    __imp__XContentCreateCrossTitleEnumerator(ctx, base);
    REXLOG_DEBUG("content: cross-title enumerator user {} device {} type {} -> {:#x}", user, device,
                 type, ctx.r3.u32);
}

#ifdef _WIN32

extern "C" REX_FUNC(__imp__XamContentCreateEnumerator) {
    const uint32_t user = ctx.r3.u32, device = ctx.r4.u32, type = ctx.r5.u32, flags = ctx.r6.u32;
    g_sdk_create_enumerator(ctx, base);
    REXLOG_DEBUG("content: XamContentCreateEnumerator user {} device {} type {} flags {:#x} -> {:#x}",
                 user, device, type, flags, ctx.r3.u32);
}

extern "C" REX_FUNC(__imp__XamContentCreateEx) {
    const uint32_t user = ctx.r3.u32, flags = ctx.r6.u32;
    const char* root = reinterpret_cast<const char*>(REX_RAW_ADDR(ctx.r4.u32));
    const std::string root_name = root ? root : "";
    const auto* data = ContentData(base, ctx.r5.u32);
    const std::string file = data->file_name();
    const uint32_t type = static_cast<uint32_t>(data->content_type.get());
    g_sdk_create_ex(ctx, base);
    REXLOG_DEBUG("content: CreateEx user {} root '{}' file '{}' type {} flags {:#x} -> {:#x}", user,
                 root_name, file, type, flags & 0xF, ctx.r3.u32);
}

extern "C" REX_FUNC(__imp__XamContentClose) {
    const char* root = reinterpret_cast<const char*>(REX_RAW_ADDR(ctx.r3.u32));
    const std::string root_name = root ? root : "";
    g_sdk_close(ctx, base);
    REXLOG_DEBUG("content: Close root '{}' -> {:#x}", root_name, ctx.r3.u32);
}

extern "C" REX_FUNC(__imp__XamContentGetCreator) {
    const uint32_t user = ctx.r3.u32;
    const auto* data = ContentData(base, ctx.r4.u32);
    const std::string file = data->file_name();
    const uint32_t type = static_cast<uint32_t>(data->content_type.get());
    g_sdk_get_creator(ctx, base);
    REXLOG_DEBUG("content: GetCreator user {} file '{}' type {} -> {:#x}", user, file, type,
                 ctx.r3.u32);
}

#endif
