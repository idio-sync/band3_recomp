// Experimental: logs a stack trace when the process is about to fast-fail
// through abort(), std::terminate or a CRT invalid parameter, which otherwise
// ends band3 with 0xC0000409 and nothing in the log. Frames are module+RVA;
// tools/symbolize.py resolves band3.exe's against the build's band3.map.
//
// Each run's reports go to their own file in the logs folder beside band3.exe
// (as the SDK's log does), crash-<start>-<pid>.txt, under a header naming the
// build (crash_report.h). One that ends band3 also leaves last_crash.txt
// there, which the next start tells the player about (ShowLastCrashNotice).
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
#include <dxgi1_4.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_stdinc.h>

#include <rex/exception_handler.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "src/build_tag.h"
#include "src/crash_report.h"
#include "src/dred_report.h"
#include "src/settings.h"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <typeinfo>
#include <unordered_map>
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
// names the native renderer's draw a hang stopped at (SetIndexedDrawNamer)
std::string (*g_namer)(uint32_t before, uint32_t total) = nullptr;
// the watched device's adapter, for the report's header
std::string g_gpu_name;

// This run's report file and what its header says, fixed when band3 starts
// (SetUpReportFile), so a crash only has to write.
struct ReportFile {
    std::filesystem::path folder;  // <band3.exe's folder>/logs
    std::filesystem::path path;
    band3::crash_report::Utc started;
    uint32_t exe_timestamp = 0;
    bool header_written = false;
    bool fatal_recorded = false;
};
ReportFile g_report;
std::mutex g_report_mutex;

band3::crash_report::Utc UtcNow() {
    SYSTEMTIME t{};
    GetSystemTime(&t);
    return {t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond};
}

// _wfopen_s, or null
FILE* Open(const std::filesystem::path& path, const wchar_t* mode) {
    FILE* f = nullptr;
    return _wfopen_s(&f, path.c_str(), mode) == 0 ? f : nullptr;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr,
                                      nullptr);
    if (n <= 0) return {};
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

void SetUpReportFile() {
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_report.folder = std::filesystem::path(exe).parent_path() / "logs";
    g_report.started = UtcNow();
    g_report.path = g_report.folder / band3::crash_report::ReportFileName(
                                          g_report.started, GetCurrentProcessId());
    // band3.exe's link time stamp, which band3.map's header gives too
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    g_report.exe_timestamp = nt->FileHeader.TimeDateStamp;
}

// appends to this run's report, the header first; try_lock, as AppendDred
// does, so a crash while another report is being written doesn't deadlock
bool WriteReport(const std::string& text) {
    std::unique_lock lock(g_report_mutex, std::try_to_lock);
    if (!lock || g_report.path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(g_report.folder, ec);
    FILE* f = Open(g_report.path, L"ab");
    if (!f) return false;
    if (!g_report.header_written) {
        band3::crash_report::Run run;
        run.build = band3::BuildTag();
        run.exe_timestamp = g_report.exe_timestamp;
        run.started = g_report.started;
        run.pid = GetCurrentProcessId();
        run.renderer = REXCVAR_GET(renderer);
        if (std::unique_lock dred(g_dred_mutex, std::try_to_lock); dred) run.gpu = g_gpu_name;
        std::fputs(band3::crash_report::FormatHeader(run).c_str(), f);
        g_report.header_written = true;
    }
    std::fputs(text.c_str(), f);
    std::fclose(f);
    return true;
}

// what ended this run, for the next start's notice: the first one, since
// std::terminate goes on to abort()
void RecordFatal(band3::crash_report::Kind kind) {
    std::unique_lock lock(g_report_mutex, std::try_to_lock);
    if (!lock || g_report.path.empty() || g_report.fatal_recorded) return;
    g_report.fatal_recorded = true;
    band3::crash_report::LastCrash c;
    c.report = Utf8(g_report.path.wstring());
    c.kind = kind;
    c.renderer = REXCVAR_GET(renderer);
    c.build = band3::BuildTag();
    if (FILE* f = Open(g_report.folder / "last_crash.txt", L"wb")) {
        std::fputs(band3::crash_report::FormatLastCrash(c).c_str(), f);
        std::fclose(f);
    }
}

// the adapter a device was made on, as DXGI describes it
std::string AdapterName(ID3D12Device* device) {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    const auto create = dxgi ? reinterpret_cast<CreateFactory>(
                                   GetProcAddress(dxgi, "CreateDXGIFactory1"))
                             : nullptr;
    ComPtr<IDXGIFactory4> factory;
    if (!create || FAILED(create(IID_PPV_ARGS(&factory)))) return {};
    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
        return {};
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc))) return {};
    return Utf8(desc.Description);
}

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

// The watched device's DRED, if it was removed, and whether it was. try_lock:
// an abort while WatchD3D12Device holds the lock on the same thread mustn't
// deadlock.
bool AppendDred(std::string& text) {
    std::unique_lock lock(g_dred_mutex, std::try_to_lock);
    if (!lock || !g_dred_device) return false;
    const HRESULT reason = g_dred_device->GetDeviceRemovedReason();
    if (reason == S_OK) return false;
    char head[160];
    std::snprintf(head, sizeof(head), "DRED (%s): the Direct3D 12 device was removed, 0x%08lX\n",
                  g_dred_status.c_str(), reason);
    text += head;
    if (g_dred_status != kDredOn) return true;
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (FAILED(g_dred_device.As(&dred))) {
        text += "DRED: the device has no extended data\n";
        return true;
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
        // the native renderer's draw, where the list stopped at one of its
        band3::dred::IndexedDrawNamer namer;
        if (g_namer) namer = g_namer;
        text += band3::dred::FormatBreadcrumbs(lists, 6, 8, namer);
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
    return true;
}

// writes the stack, and the device's DRED, to the log and this run's report;
// true when the device was removed
bool DumpStack(const char* why) {
    void* frames[64];
    const USHORT n = RtlCaptureStackBackTrace(1, 64, frames, nullptr);
    std::string text = std::string("[crash-trace] ") + why + " on thread " +
                       std::to_string(GetCurrentThreadId()) + " at " +
                       band3::crash_report::FormatUtc(UtcNow()) + " UTC\n";
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
    const bool removed = AppendDred(text);
    REXLOG_CRITICAL("{}", text);
    if (WriteReport(text)) REXLOG_CRITICAL("Crash report: {}", Utf8(g_report.path.wstring()));
    if (auto logger = rex::GetLogger()) logger->flush();
    return removed;
}

void OnAbort(int) {
    const bool removed = DumpStack("abort()");
    RecordFatal(removed ? band3::crash_report::Kind::kGpuHang : band3::crash_report::Kind::kAbort);
}

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
    RecordFatal(band3::crash_report::Kind::kTerminate);
    std::abort();
}

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    DumpStack("CRT invalid parameter");
}

// With d3d12_debug, where the debug layer's errors come from: it reports each
// message through OutputDebugString (DBG_PRINTEXCEPTION_C) on the thread
// whose call it's about, and with d3d12_break_on_error (its default) breaks
// there too, so the stack names the caller (SDL in band3.exe, or the SDK's
// GPU in rexgpu-xenos.dll). A breakpoint nothing handles ends band3 with no
// word of where; it still does, once its stack is written.
LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
    static thread_local bool in_handler = false;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT && !in_handler) {
        in_handler = true;
        DumpStack("breakpoint (with d3d12_debug: the debug layer's error, logged just before)");
        in_handler = false;
    } else if (code == 0x40010006 && !in_handler &&
               info->ExceptionRecord->NumberParameters >= 2) {
        // an error's or a corruption's, the first few
        const char* text = reinterpret_cast<const char*>(
            info->ExceptionRecord->ExceptionInformation[1]);
        if (text && (std::strstr(text, "D3D12 ERROR") || std::strstr(text, "CORRUPTION"))) {
            static std::atomic<int> dumps{0};
            if (dumps++ < 4) {
                in_handler = true;
                std::string why = std::string("debug-layer message: ") + text;
                while (!why.empty() && (why.back() == '\n' || why.back() == '\r'))
                    why.pop_back();
                DumpStack(why.c_str());
                in_handler = false;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// --- guest faults (WatchGuestFaults) ---

// recompiled functions by their host address, to their guest address
std::unordered_map<uintptr_t, uint32_t> g_guest_functions;
HMODULE g_exe = nullptr;

// the start of the function a RUNTIME_FUNCTION belongs to: a function the
// compiler split has entries for its parts, chained to the first
const RUNTIME_FUNCTION* PrimaryEntry(const RUNTIME_FUNCTION* fn, DWORD64 image_base) {
    constexpr uint8_t kChainInfo = 0x4;  // UNW_FLAG_CHAININFO
    for (int i = 0; i < 32; i++) {
        const auto* info = reinterpret_cast<const uint8_t*>(image_base + fn->UnwindData);
        if (!((info[0] >> 3) & kChainInfo)) break;
        // after the unwind codes, which are counted in 16-bit slots and padded to an even count
        const uint32_t codes = (info[2] + 1u) & ~1u;
        fn = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + codes * 2);
    }
    return fn;
}

// "0x82517400 (band3.exe+0x1234)" for a recompiled function's frame, else module+RVA
std::string DescribeFrame(DWORD64 pc, bool return_address) {
    // a return address can be just past its function's end, after a call that doesn't return
    const DWORD64 lookup = return_address ? pc - 1 : pc;
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(lookup), &mod);
    char name[MAX_PATH] = "?";
    if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
    const char* file = std::strrchr(name, '\\');
    char where[MAX_PATH + 32];
    std::snprintf(where, sizeof(where), "%s+0x%llX", file ? file + 1 : name,
                  static_cast<unsigned long long>(pc - reinterpret_cast<DWORD64>(mod)));
    if (mod == g_exe) {
        DWORD64 image_base = 0;
        if (const auto* fn = RtlLookupFunctionEntry(lookup, &image_base, nullptr)) {
            const uintptr_t start = image_base + PrimaryEntry(fn, image_base)->BeginAddress;
            if (auto it = g_guest_functions.find(start); it != g_guest_functions.end()) {
                char guest[MAX_PATH + 64];
                std::snprintf(guest, sizeof(guest), "guest 0x%08X (%s)", it->second, where);
                return guest;
            }
        }
    }
    return where;
}

// the frames from the fault outward, unwound from the faulting context
std::string GuestCallChain(const rex::arch::HostThreadContext& host) {
    CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    c.Rip = host.rip;
    c.Rax = host.rax, c.Rcx = host.rcx, c.Rdx = host.rdx, c.Rbx = host.rbx;
    c.Rsp = host.rsp, c.Rbp = host.rbp, c.Rsi = host.rsi, c.Rdi = host.rdi;
    c.R8 = host.r8, c.R9 = host.r9, c.R10 = host.r10, c.R11 = host.r11;
    c.R12 = host.r12, c.R13 = host.r13, c.R14 = host.r14, c.R15 = host.r15;
    std::string text;
    for (int i = 0; i < 24 && c.Rip; i++) {
        char line[24];
        std::snprintf(line, sizeof(line), "\n  #%02d ", i);
        text += line;
        text += DescribeFrame(c.Rip, i > 0);
        DWORD64 image_base = 0;
        const auto* fn = RtlLookupFunctionEntry(i > 0 ? c.Rip - 1 : c.Rip, &image_base, nullptr);
        if (fn) {
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, const_cast<RUNTIME_FUNCTION*>(fn),
                             &c, &handler_data, &establisher, nullptr);
        } else {
            // a leaf function: its return address is on top of the stack
            c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
            c.Rsp += 8;
        }
    }
    return text;
}

bool OnGuestFault(rex::arch::Exception* ex, void*) {
    using rex::arch::Exception;
    if (ex->code() != Exception::Code::kAccessViolation) return false;
    auto* memory = rex::system::kernel_memory();
    if (!memory) return false;
    const uint64_t membase = reinterpret_cast<uint64_t>(memory->virtual_membase());
    const uint64_t fault = ex->fault_address();
    // the guest's address space and the physical memory mapped after it
    if (fault < membase || fault - membase >= (uint64_t(2) << 32)) return false;
    // the first few: one the game catches itself comes on every boot
    static std::atomic<int> reports{0};
    if (reports++ >= 8) return false;
    const char* op = ex->access_violation_operation() == Exception::AccessViolationOperation::kWrite
                         ? "write"
                         : ex->access_violation_operation() ==
                                   Exception::AccessViolationOperation::kRead
                               ? "read"
                               : "access";
    REXLOG_ERROR("Guest fault: {} of guest 0x{:08X} on thread {}; the guest functions it was in, "
                 "innermost first (addresses as in band3_config.toml):{}",
                 op, static_cast<uint32_t>(fault - membase), GetCurrentThreadId(),
                 GuestCallChain(*ex->thread_context()));
    if (auto logger = rex::GetLogger()) logger->flush();
    return false;  // not handled: the game's own handler, if any, or the crash
}

struct Install {
    Install() {
        SetUpReportFile();
        AddVectoredExceptionHandler(1, OnException);
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

void SetIndexedDrawNamer(std::string (*namer)(uint32_t before, uint32_t total)) {
    std::lock_guard lock(g_dred_mutex);
    g_namer = namer;
}

void WatchGuestFaults() {
    g_exe = GetModuleHandleW(nullptr);
    for (const PPCFuncMapping* m = PPCFuncMappings; m->host; m++) {
        g_guest_functions.emplace(reinterpret_cast<uintptr_t>(m->host), static_cast<uint32_t>(m->guest));
    }
    rex::arch::ExceptionHandler::Install(OnGuestFault, nullptr);
}

void WatchD3D12Device(void* device, void* direct_queue) {
    // outside the lock: DXGI may take a while
    std::string gpu = device ? AdapterName(static_cast<ID3D12Device*>(device)) : std::string();
    std::lock_guard lock(g_dred_mutex);
    g_dred_device = static_cast<ID3D12Device*>(device);
    g_dred_queue = direct_queue;
    if (!gpu.empty()) g_gpu_name = std::move(gpu);
}

void RunCrashTest() {
    char test[32] = "";
    if (!GetEnvironmentVariableA("BAND3_CRASH_TEST", test, sizeof(test)) || !*test) return;
    REXLOG_WARN("BAND3_CRASH_TEST={}: crashing on purpose", test);
    if (std::strcmp(test, "abort") == 0) std::abort();
    if (std::strcmp(test, "terminate") == 0) std::terminate();
    REXLOG_WARN("BAND3_CRASH_TEST: unknown test (abort or terminate)");
}

void ShowLastCrashNotice() {
    const auto marker = g_report.folder / "last_crash.txt";
    FILE* f = Open(marker, L"rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    std::fclose(f);
    // once: the notice is for the next start only
    std::error_code ec;
    std::filesystem::remove(marker, ec);
    const auto crash = band3::crash_report::ParseLastCrash(text);
    if (!crash) return;
    REXLOG_WARN("The last run crashed; its report: {}", crash->report);
    const std::string notice = band3::crash_report::Notice(*crash);
    // SDL's message box needs no SDL_Init or window
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0,
         "OK"},
        {0, 1, "Open folder"},
    };
    const SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, nullptr, "band3", notice.c_str(),
                                 SDL_arraysize(buttons), buttons, nullptr};
    int chosen = 0;
    if (SDL_ShowMessageBox(&box, &chosen) && chosen == 1) {
        ShellExecuteW(nullptr, L"open", g_report.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

}  // namespace band3::crash_trace

#endif
