// rex::ui::RenderDocAPI::CreateIfConnected, in place of the SDK's on Linux.
//
// RenderDoc's in-app API is meant to be used only when RenderDoc is attached:
// on Windows the library is in the process then and only then. The SDK's
// Linux port loads librenderdoc.so to find out, and where RenderDoc is
// installed (SteamOS ships it, in /usr/lib) that load is what attaches it:
// every run then had RenderDoc's capture hooks wrapping each Vulkan call,
// SDL_gpu's and the presenter's ("RenderDoc API initialized" in the log).
// This asks the loader whether librenderdoc.so is already loaded
// (RTLD_NOLOAD), as when RenderDoc launched band3 or injected itself, and
// only then hands on to the SDK's, which finds it so.
//
// The executable's definition is the one the SDK's runtime library binds to
// (through its GOT; cmake/linux_exports.map exports _Z*).

#ifdef __linux__

#include <dlfcn.h>

#include <rex/platform/dynlib.h>
#include <rex/ui/renderdoc_api.h>

#include <cstdio>
#include <memory>

namespace rex::ui {

std::unique_ptr<RenderDocAPI> RenderDocAPI::CreateIfConnected() {
    void* loaded = dlopen(platform::lib_names::kRenderDoc, RTLD_NOW | RTLD_NOLOAD);
    if (!loaded) return nullptr;
    dlclose(loaded);
    using CreateFn = std::unique_ptr<RenderDocAPI> (*)();
    const auto sdk = reinterpret_cast<CreateFn>(
        dlsym(RTLD_NEXT, "_ZN3rex2ui12RenderDocAPI17CreateIfConnectedEv"));
    return sdk ? sdk() : nullptr;
}

}  // namespace rex::ui

#endif
