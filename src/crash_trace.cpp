// Experimental: logs a stack trace when the process is about to fast-fail
// through abort(), std::terminate or a CRT invalid parameter, which otherwise
// ends band3 with 0xC0000409 and nothing in the log. Frames are module+RVA;
// resolve them against out/build/<preset>/band3.map.
//
// With the dred setting (EnableDred, before the SDK makes its device) it also
// turns on Direct3D 12's Device Removed Extended Data: auto-breadcrumbs, which
// record each command list's ops and how far the GPU got, and page fault
// reports. When the SDK aborts on a removed device (a GPU hang:
// DXGI_ERROR_DEVICE_HUNG), the trace says which command lists the GPU hadn't
// finished and the op it stopped at (dred_report.h). The breadcrumbs cost the
// GPU a small write per op, so it's off unless asked for; the test harness
// asks.

#ifdef _WIN32

#include "src/crash_trace.h"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <rex/logging.h>

#include "src/dred_report.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <typeinfo>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

// DRED: whether it's on (EnableDred) and the device to read it from
// (WatchD3D12Device). Above g_install, which they must outlive.
const char* const kDredOn = "on";
std::string g_dred_status = "off (--dred=true turns it on)";
std::mutex g_dred_mutex;
ComPtr<ID3D12Device> g_dred_device;
const void* g_dred_queue = nullptr;

// a list's, queue's or allocation's name, or else what it is
std::string DebugName(const char* a, const wchar_t* w, const void* object, const char* what) {
    if (a && *a) return a;
    if (w && *w) {
        char narrow[256];
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, narrow, sizeof(narrow), nullptr,
                                          nullptr);
        if (n > 1) return narrow;
    }
    if (!object) return "";
    char ptr[64];
    std::snprintf(ptr, sizeof(ptr), "%s %p", what, object);
    return ptr;
}

void TurnOnDred() {
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    using GetDebugInterface = HRESULT(WINAPI*)(REFIID, void**);
    const auto get = d3d12 ? reinterpret_cast<GetDebugInterface>(
                                 GetProcAddress(d3d12, "D3D12GetDebugInterface"))
                           : nullptr;
    if (!get) {
        g_dred_status = "unavailable (no d3d12.dll)";
        return;
    }
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> settings;
    const HRESULT hr = get(IID_PPV_ARGS(&settings));
    if (FAILED(hr)) {
        char why[96];
        std::snprintf(why, sizeof(why), "unavailable (D3D12GetDebugInterface 0x%08lX)", hr);
        g_dred_status = why;
        return;
    }
    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    g_dred_status = kDredOn;
}

// The watched device's DRED, if it was removed. try_lock: an abort while
// WatchD3D12Device holds the lock on the same thread mustn't deadlock.
void AppendDred(std::string& text) {
    std::unique_lock lock(g_dred_mutex, std::try_to_lock);
    if (!lock || !g_dred_device) return;
    const HRESULT reason = g_dred_device->GetDeviceRemovedReason();
    if (reason == S_OK) return;
    char head[160];
    std::snprintf(head, sizeof(head), "DRED (%s): the Direct3D 12 device was removed, 0x%08lX\n",
                  g_dred_status.c_str(), reason);
    text += head;
    if (g_dred_status != kDredOn) return;
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(g_dred_device.As(&dred))) {
        text += "DRED: the device has no extended data\n";
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs))) {
        std::vector<band3::dred::CommandList> lists;
        for (auto* n = crumbs.pHeadAutoBreadcrumbNode; n; n = n->pNext) {
            band3::dred::CommandList l;
            l.queue = n->pCommandQueue == g_dred_queue
                          ? "the SDK's direct queue"
                          : DebugName(n->pCommandQueueDebugNameA, n->pCommandQueueDebugNameW,
                                      n->pCommandQueue, "queue");
            l.list = DebugName(n->pCommandListDebugNameA, n->pCommandListDebugNameW,
                               n->pCommandList, "list");
            l.done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            if (n->pCommandHistory)
                for (UINT i = 0; i < n->BreadcrumbCount; i++)
                    l.ops.push_back(uint32_t(n->pCommandHistory[i]));
            lists.push_back(std::move(l));
        }
        text += band3::dred::FormatBreadcrumbs(lists);
    } else {
        text += "DRED: no breadcrumbs\n";
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault))) {
        band3::dred::PageFault pf;
        pf.va = fault.PageFaultVA;
        // the first few of each: the lists can run to thousands
        auto take = [](const D3D12_DRED_ALLOCATION_NODE* a, std::vector<band3::dred::Allocation>& out) {
            for (; a && out.size() < 16; a = a->pNext)
                out.push_back({DebugName(a->ObjectNameA, a->ObjectNameW, nullptr, ""),
                               uint32_t(a->AllocationType)});
        };
        take(fault.pHeadExistingAllocationNode, pf.existing);
        take(fault.pHeadRecentFreedAllocationNode, pf.freed);
        text += band3::dred::FormatPageFault(pf);
    }
}

void DumpStack(const char* why) {
    void* frames[64];
    const USHORT n = RtlCaptureStackBackTrace(1, 64, frames, nullptr);
    std::string text = std::string("[crash-trace] ") + why + " on thread " +
                       std::to_string(GetCurrentThreadId()) + "\n";
    for (USHORT i = 0; i < n; i++) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(frames[i]), &mod);
        if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
        const char* base = std::strrchr(name, '\\');
        char line[512];
        std::snprintf(line, sizeof(line), "  #%02u %s+0x%llX\n", i, base ? base + 1 : name,
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) -
                                                      reinterpret_cast<uintptr_t>(mod)));
        text += line;
    }
    AppendDred(text);
    REXLOG_CRITICAL("{}", text);
    if (auto logger = rex::GetLogger()) logger->flush();
    if (FILE* f = std::fopen("band3_crash_trace.txt", "a")) {
        std::fputs(text.c_str(), f);
        std::fclose(f);
    }
}

void OnAbort(int) { DumpStack("abort()"); }

void OnTerminate() {
    std::string what = "std::terminate";
    if (auto e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            what += std::string(" (uncaught ") + typeid(ex).name() + ": " + ex.what() + ")";
        } catch (...) {
            what += " (uncaught non-std exception)";
        }
    }
    DumpStack(what.c_str());
    std::abort();
}

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    DumpStack("CRT invalid parameter");
}

struct Install {
    Install() {
        std::signal(SIGABRT, OnAbort);
        std::set_terminate(OnTerminate);
        _set_invalid_parameter_handler(OnInvalidParameter);
    }
} g_install;

}  // namespace

namespace band3::crash_trace {

void EnableDred() {
    TurnOnDred();
    REXLOG_INFO("DRED: {}", g_dred_status);
}

void WatchD3D12Device(void* device, void* direct_queue) {
    std::lock_guard lock(g_dred_mutex);
    g_dred_device = static_cast<ID3D12Device*>(device);
    g_dred_queue = direct_queue;
}

}  // namespace band3::crash_trace

#endif
