#include "sdk_export.h"
#include <rex/logging.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace band3 {

SdkFunction* SdkExport(const char* name) {
#ifdef _WIN32
    HMODULE runtime = GetModuleHandleA(BAND3_REXRUNTIME_DLL);
    auto* fn = runtime ? reinterpret_cast<SdkFunction*>(GetProcAddress(runtime, name)) : nullptr;
#else
    // band3's overrides interpose on the runtime's names, so look in the
    // runtime itself (already loaded: band3 links it) rather than globally
    static void* const runtime = dlopen(BAND3_REXRUNTIME_DLL, RTLD_NOW | RTLD_NOLOAD);
    auto* fn = runtime ? reinterpret_cast<SdkFunction*>(dlsym(runtime, name)) : nullptr;
#endif
    if (!fn) REXLOG_ERROR("sdk: {} has no {}", BAND3_REXRUNTIME_DLL, name);
    return fn;
}

}  // namespace band3
