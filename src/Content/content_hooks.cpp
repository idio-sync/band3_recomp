#include "content_hooks.h"
#include <chrono>
#include <string>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xenumerator.h>
#include <rex/types.h>
#include "generated/band3_init.h"
#include "live_content.h"
#ifdef _WIN32
#include <windows.h>
#endif

// RB3 lists DLC and custom songs through XContentCreateCrossTitleEnumerator,
// opens them through XContentCrossTitleCreate, and opens its saves and closes
// both through these XAM exports. band3 takes them
// over so it can serve packages from content folders (live_content.cpp) and
// hands everything else to the SDK's own.

extern "C" void __imp__XContentCreateCrossTitleEnumerator(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__XContentCrossTitleCreate(PPCContext& ctx, uint8_t* base);

namespace {

using Export = void(PPCContext&, uint8_t*);

// set before the game makes a content call: OnPostSetup aborts if any is missing
Export* g_sdk_create_ex = nullptr;
Export* g_sdk_close = nullptr;
Export* g_sdk_get_creator = nullptr;
Export* g_sdk_create_enumerator = nullptr;

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
    [[maybe_unused]] const uint32_t handle_out = ctx.r9.u32;
    __imp__XContentCreateCrossTitleEnumerator(ctx, base);
    REXLOG_DEBUG("content: cross-title enumerator user {} device {} type {} -> {:#x}", user, device,
                 type, ctx.r3.u32);
#ifdef _WIN32
    // RB3 asks for each type as user 0 and again as any user (0xFF); band3's
    // packages belong to no user, so they go in the second, once each
    using rex::system::xam::DummyDeviceId;
    using rex::system::xam::XCONTENT_AGGREGATE_DATA;
    if (ctx.r3.u32 != 0 || user != 0xFF || handle_out == 0) return;
    if (device != 0 && device != static_cast<uint32_t>(DummyDeviceId::HDD)) return;
    using Enumerator = rex::system::XStaticEnumerator<XCONTENT_AGGREGATE_DATA>;
    auto e = REX_KERNEL_OBJECTS()->LookupObject<Enumerator>(REX_LOAD_U32(handle_out));
    if (!e || e->item_size() != sizeof(XCONTENT_AGGREGATE_DATA)) return;
    size_t n = 0;
    for (const auto& package : band3::content::LivePackages(std::chrono::seconds(15))) {
        if (package.header.content_type != type) continue;
        auto* item = e->AppendItem();
        item->device_id = static_cast<uint32_t>(DummyDeviceId::HDD);
        item->content_type = static_cast<rex::system::XContentType>(type);
        item->title_id = band3::content::kRb3TitleId;
        item->xuid = 0;
        item->set_display_name(package.header.display_name);
        item->set_file_name(package.header.content_id);
        n++;
    }
    if (n > 0) REXLOG_INFO("content: listed {} packages of type {}", n, type);
#endif
}

#ifdef _WIN32

namespace {

using rex::X_HRESULT;
using rex::X_RESULT;
using rex::system::xam::XCONTENT_DATA;

const XCONTENT_DATA* ContentData(uint8_t* base, uint32_t addr) {
    return addr ? reinterpret_cast<const XCONTENT_DATA*>(REX_RAW_ADDR(addr)) : nullptr;
}

std::string GuestString(uint8_t* base, uint32_t addr) {
    return addr ? std::string(reinterpret_cast<const char*>(REX_RAW_ADDR(addr))) : std::string();
}

// the package data names, if it is one of band3's
const band3::content::Package* LivePackage(const XCONTENT_DATA* data) {
    if (!data) return nullptr;
    const auto* package = band3::content::FindLivePackage(data->file_name());
    if (!package) return nullptr;
    return package->header.content_type == static_cast<uint32_t>(data->content_type.get()) ? package
                                                                                             : nullptr;
}

// completes a call as the SDK does: through the overlapped if there is one
u32 Complete(mapped_void overlapped_ptr, X_RESULT result) {
    if (!overlapped_ptr) return result;
    REX_KERNEL_STATE()->CompleteOverlappedImmediateEx(overlapped_ptr.guest_address(), result,
                                                      X_HRESULT_FROM_WIN32(result), 0);
    return X_ERROR_IO_PENDING;
}

// XamContentCreateEx for one of band3's packages, which are read-only
u32 CreateLive(u32 user_index, mapped_string root_name, mapped_void content_data_ptr, u32 flags,
               mapped_u32 disposition_ptr, mapped_u32 license_mask_ptr, u32 cache_size,
               u64 content_size, mapped_void overlapped_ptr) {
    const auto* package = LivePackage(content_data_ptr.as<const XCONTENT_DATA*>());
    const uint32_t mode = flags & 0xF;
    auto run = [package, mode, root = std::string(root_name.value()), disposition_ptr,
                license_mask_ptr](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
        constexpr X_RESULT kErrorFileCorrupt = X_RESULT_FROM_WIN32(0x00000570L);
        constexpr uint32_t kOpen = 2;
        X_RESULT result = X_ERROR_ACCESS_DENIED;
        uint32_t disposition = 0;
        if (mode == 3 || mode == 4) {  // OPEN_EXISTING, OPEN_ALWAYS
            if (band3::content::MountLivePackage(*package, root)) {
                result = X_ERROR_SUCCESS;
                disposition = kOpen;
                if (license_mask_ptr) *license_mask_ptr = package->header.license_mask;
            } else {
                result = kErrorFileCorrupt;
            }
        }
        if (disposition_ptr) *disposition_ptr = disposition;
        REXLOG_DEBUG("content: opened {} as {}: -> {:#x}", package->header.content_id, root,
                     result);
        extended_error = X_HRESULT_FROM_WIN32(result);
        length = disposition;
        return result;
    };
    if (!overlapped_ptr) {
        uint32_t extended_error, length;
        return run(extended_error, length);
    }
    // deferred, as the SDK does it: RB3 only tells its song manager about a
    // mount it saw pending first, and without that a song's files are looked
    // for in the game's own archive instead of the package
    if (disposition_ptr) *disposition_ptr = 0;
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run, overlapped_ptr.guest_address());
    return X_ERROR_IO_PENDING;
}

u32 CloseLive(mapped_string root_name, mapped_void overlapped_ptr) {
    return Complete(overlapped_ptr, X_ERROR_SUCCESS);
}

u32 GetCreatorLive(u32 user_index, mapped_void content_data_ptr, mapped_u32 is_creator_ptr,
                   mapped_u64 creator_xuid_ptr, mapped_void overlapped_ptr) {
    if (is_creator_ptr) *is_creator_ptr = 0;
    if (creator_xuid_ptr) *creator_xuid_ptr = 0;
    return Complete(overlapped_ptr, X_ERROR_SUCCESS);
}

}  // namespace

// RB3 opens what the cross-title enumerator listed through this (recompiled)
// function, which looks XamContentCreateInternal up in the SDK's own registry,
// so band3's packages are opened here; it takes XamContentCreateEx's arguments
extern "C" REX_FUNC(XContentCrossTitleCreate) {
    const auto* data = ContentData(base, ctx.r5.u32);
    if (LivePackage(data)) {
        rex::ppc::HostToGuestFunction<CreateLive>(ctx, base);
        return;
    }
    const std::string root_name = GuestString(base, ctx.r4.u32);
    const std::string file = data ? data->file_name() : std::string();
    const uint32_t flags = ctx.r6.u32;
    // the SDK's own mount won't replace band3's link for the root
    band3::content::UnmountLiveRoot(root_name);
    __imp__XContentCrossTitleCreate(ctx, base);
    REXLOG_DEBUG("content: cross-title create root '{}' file '{}' flags {:#x} -> {:#x}", root_name,
                 file, flags & 0xF, ctx.r3.u32);
}

extern "C" REX_FUNC(__imp__XamContentCreateEnumerator) {
    const uint32_t user = ctx.r3.u32, device = ctx.r4.u32, type = ctx.r5.u32, flags = ctx.r6.u32;
    g_sdk_create_enumerator(ctx, base);
    REXLOG_DEBUG("content: XamContentCreateEnumerator user {} device {} type {} flags {:#x} -> {:#x}",
                 user, device, type, flags, ctx.r3.u32);
}

extern "C" REX_FUNC(__imp__XamContentCreateEx) {
    const uint32_t user = ctx.r3.u32, flags = ctx.r6.u32;
    const std::string root_name = GuestString(base, ctx.r4.u32);
    const auto* data = ContentData(base, ctx.r5.u32);
    const std::string file = data ? data->file_name() : std::string();
    const uint32_t type = data ? static_cast<uint32_t>(data->content_type.get()) : 0;
    if (LivePackage(data)) {
        rex::ppc::HostToGuestFunction<CreateLive>(ctx, base);
        return;
    }
    // the SDK's own mount won't replace band3's link for the root
    band3::content::UnmountLiveRoot(root_name);
    g_sdk_create_ex(ctx, base);
    REXLOG_DEBUG("content: CreateEx user {} root '{}' file '{}' type {} flags {:#x} -> {:#x}", user,
                 root_name, file, type, flags & 0xF, ctx.r3.u32);
}

extern "C" REX_FUNC(__imp__XamContentClose) {
    const std::string root_name = GuestString(base, ctx.r3.u32);
    if (band3::content::UnmountLiveRoot(root_name)) {
        rex::ppc::HostToGuestFunction<CloseLive>(ctx, base);
        REXLOG_DEBUG("content: closed {}", root_name);
        return;
    }
    g_sdk_close(ctx, base);
    REXLOG_DEBUG("content: Close root '{}' -> {:#x}", root_name, ctx.r3.u32);
}

extern "C" REX_FUNC(__imp__XamContentGetCreator) {
    const uint32_t user = ctx.r3.u32;
    const auto* data = ContentData(base, ctx.r4.u32);
    if (LivePackage(data)) {
        rex::ppc::HostToGuestFunction<GetCreatorLive>(ctx, base);
        return;
    }
    const std::string file = data ? data->file_name() : std::string();
    const uint32_t type = data ? static_cast<uint32_t>(data->content_type.get()) : 0;
    g_sdk_get_creator(ctx, base);
    REXLOG_DEBUG("content: GetCreator user {} file '{}' type {} -> {:#x}", user, file, type,
                 ctx.r3.u32);
}

#endif
