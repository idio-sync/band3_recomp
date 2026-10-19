#include "src/Render/xr_present.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>

#include <fmt/format.h>
#include <rex/logging.h>

#include "src/Render/vr_venue.h"
#include "src/settings.h"

#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include "src/Render/gpu_view.h"
#include "src/Render/shaders/present_shaders.gen.h"

// the headers only: every function comes from the loader at run time
#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include "src/ThirdParty/openxr/include/openxr/openxr.h"
#include "src/ThirdParty/openxr/include/openxr/openxr_platform.h"
#endif

namespace band3::render::xr {

namespace {

// the song's venue for the fixed viewpoints (vr_venue.h), from whichever
// thread lays out a frame (VrFrame, the harness's capture)
std::mutex g_venue_mutex;
vr::VenueTracker g_venue;

void LogVenue(const vr::VenueRef& r, float units) {
    std::string members;
    for (int m = 0; m < r.members; m++)
        members += fmt::format("{}({:.0f},{:.0f},{:.0f})", m ? " " : "", r.member[m][0],
                               r.member[m][1], r.member[m][2]);
    std::string presets;
    for (vr::ViewMode mode :
         {vr::ViewMode::kFrontRow, vr::ViewMode::kOnStage, vr::ViewMode::kVenue}) {
        vr::Anchor a;
        if (!vr::PresetAnchor(mode, r, {}, units, a)) continue;
        presets += fmt::format(" {} {:.0f},{:.0f},{:.0f},{:.0f}", vr::ViewModeName(mode),
                               a.pos[0], a.pos[1], a.pos[2], a.yaw * 57.2957795f);
        // moved out of the world's geometry (ClearPlaces)
        const vr::Cleared& c = r.cleared[int(mode) - int(vr::ViewMode::kFrontRow)];
        if (c.before)
            presets += fmt::format(" (moved {:.2f},{:.2f},{:.2f} m from {}{})",
                                   c.shift[0] / units, c.shift[1] / units, c.shift[2] / units,
                                   vr::BlockedNames(c.before),
                                   c.after ? "; still " + vr::BlockedNames(c.after) : "");
    }
    REXLOG_INFO("vr: the venue: {} band members {}; stage floor z {:.0f}, toward the audience "
                "{:.2f},{:.2f} ({}); {} crowd points, {} in front from {:.0f} to {:.0f} units "
                "ahead, their floor z {:.0f}; viewpoints (x,y,z,yaw):{}",
                r.members, members, r.centre[2], r.toward[0], r.toward[1],
                r.toward_crowd ? "the crowd's middle" : "-y, no crowd", r.crowd, r.crowd_ahead,
                r.crowd_front, r.crowd_back, r.audience_floor, presets);
}

}  // namespace

vr::VenueRef CurrentVenue() {
    std::lock_guard lock(g_venue_mutex);
    return g_venue.Ref();
}

vr::FrameLayout LayoutFor(const FrameCapture& fc, const FrameViews& f, bool in_song,
                          vr::TrackHold* hold, const vr::Anchor* loaded) {
    vr::Viewpoint vp;
    const std::string mode = REXCVAR_GET(vr_viewpoint);
    if (!vr::ParseViewMode(mode, vp.mode)) vp.mode = vr::ViewMode::kGameCamera;
    vp.window = vp.mode == vr::ViewMode::kGameCamera;
    // a fixed place in the song's venue (vr_venue.h), found from the song's
    // first frames with the band, vr_anchor's until then; forgotten outside
    // songs and with the window, so it's found again
    const float units = vr::kUnitsPerMetre * float(REXCVAR_GET(vr_world_scale));
    vr::VenueRef ref;
    if (!loaded) {
        std::lock_guard lock(g_venue_mutex);
        if (g_venue.Next(fc, in_song, !vp.window, units)) LogVenue(g_venue.Ref(), units);
        ref = g_venue.Ref();
    }
    if (!vp.window && loaded) {
        vp.anchor = *loaded;
        vp.crowd_clear = vr::CrowdClear(vp.mode, float(REXCVAR_GET(vr_crowd_clear)));
    } else if (!vp.window) {
        vr::ViewOffset off;
        const std::string off_text = REXCVAR_GET(vr_viewpoint_offset);
        if (!off_text.empty() && !vr::ParseViewOffset(off_text, off)) {
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true))
                REXLOG_WARN("vr: vr_viewpoint_offset '{}' isn't right,ahead,up[,yaw]", off_text);
            off = {};
        }
        if (!in_song || !vr::PresetAnchor(vp.mode, ref, off, units, vp.anchor)) {
            // once each time it's set to one
            static std::atomic<int> logged{-1};
            if (in_song && logged.exchange(int(vp.mode)) != int(vp.mode))
                REXLOG_INFO("vr: vr_viewpoint {}: no band found in the venue yet; seen from "
                            "vr_anchor's place meanwhile",
                            mode);
            const std::string anchor_text = REXCVAR_GET(vr_anchor);
            vp.follow = anchor_text.empty() || !vr::ParseAnchor(anchor_text, vp.anchor);
        }
        vp.crowd_clear = vr::CrowdClear(vp.mode, float(REXCVAR_GET(vr_crowd_clear)));
    }
    vr::FrameInputs in;
    for (int e = 0; e < 2; e++) {
        in.views[e] = f.pose[e];
        in.fov[e] = f.fov[e];
    }
    in.head = f.have_head ? f.head : vr::HeadPose(f.pose[0], f.pose[1]);
    in.hud_boundary = vr::HudBoundary(fc);
    const WorldCamera wc = WorldCameraOf(fc);
    in.camera = wc.valid && freecam::Decompose(wc.view_proj, in.game, in.proj);
    in.in_song = in_song;
    in.hud = REXCVAR_GET(vr_hud);
    in.hud_in_window = REXCVAR_GET(vr_hud_in_window);
    in.hud_layout.distance = float(REXCVAR_GET(vr_hud_distance));
    in.hud_layout.below = float(REXCVAR_GET(vr_hud_below));
    in.hud_layout.width_deg = float(REXCVAR_GET(vr_hud_width));
    in.window_width_deg = float(REXCVAR_GET(vr_window_width));
    in.max_squeeze = float(REXCVAR_GET(vr_window_max_squeeze));
    in.dim = float(REXCVAR_GET(vr_window_dim));
    in.world_scale = float(REXCVAR_GET(vr_world_scale));
    in.track.on = REXCVAR_GET(vr_track_3d);
    in.track.forward = float(REXCVAR_GET(vr_track_forward));
    in.track.down = float(REXCVAR_GET(vr_track_down));
    in.track.pitch = float(REXCVAR_GET(vr_track_pitch));
    in.track.scale = float(REXCVAR_GET(vr_track_scale));
    in.window_quads = REXCVAR_GET(vr_window_quads);
    if (in_song && in.track.on) in.tracks = vr::TrackCamerasOf(fc);
    // held from the first frame that has them until there are none (or
    // another count of them)
    if (hold) {
        int moved = 0;
        in.tracks = hold->Hold(in.tracks, moved);
        // at most once a second
        static std::atomic<int64_t> logged{0};
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        if (moved && logged.exchange(now) != now)
            REXLOG_INFO("vr: {} of the {} track cameras moved more than {} units or {} "
                        "degrees; the track is placed again",
                        moved, in.tracks.count, vr::kTrackMoved, vr::kTrackTurned);
    }
    return vr::ResolveFrame(vp, in);
}

#ifndef _WIN32

void SetDevice(void*, void*) {}
Session& Session::Get() {
    static Session s;
    return s;
}
bool Session::Update(bool) { return false; }
uint32_t Session::Width() const { return 0; }
uint32_t Session::Height() const { return 0; }
bool Session::WaitFrame(FrameViews&) { return false; }
bool Session::Present(FrameViews&, void* const[2], void*, const vr::FrameLayout&, double,
                      const QueueWait*) {
    return false;
}
bool Session::CopiesDone() { return true; }
void Session::Record(vr::FrameTiming&) {}
std::string Session::FramesCsv(uint64_t& frames) {
    frames = 0;
    return vr::CsvHeader();
}
bool Session::HudFree() { return true; }
void Session::AddRenderMs(int, double, double, double) {}
void Session::AddHudMs(double, double) {}
Stats Session::GetStats() { return {}; }
bool Session::LastFrame(FrameViews&, float&, uint32_t&, uint32_t&) { return false; }
void Session::NoteFrame(const FrameViews&, float) {}
void Session::StartMeasuring() {}
std::string Session::MeasuredJson() { return {}; }

#else

using Microsoft::WRL::ComPtr;

namespace {

std::mutex g_device_mutex;
ID3D12Device* g_device = nullptr;
ID3D12CommandQueue* g_queue = nullptr;

const char* StateName(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_IDLE: return "idle";
        case XR_SESSION_STATE_READY: return "ready";
        case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
        case XR_SESSION_STATE_VISIBLE: return "visible";
        case XR_SESSION_STATE_FOCUSED: return "focused";
        case XR_SESSION_STATE_STOPPING: return "stopping";
        case XR_SESSION_STATE_LOSS_PENDING: return "loss_pending";
        case XR_SESSION_STATE_EXITING: return "exiting";
        default: return "unknown";
    }
}

// every DXGI adapter's LUID and name, for the log
std::string AdapterNames() {
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto create = dxgi ? reinterpret_cast<CreateFactory>(reinterpret_cast<void*>(
                             GetProcAddress(dxgi, "CreateDXGIFactory1")))
                       : nullptr;
    ComPtr<IDXGIFactory1> factory;
    if (!create || FAILED(create(IID_PPV_ARGS(&factory)))) return "adapters unknown";
    std::string out;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); i++) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name) - 1, nullptr,
                            nullptr);
        out += fmt::format("{}{:08x}:{:08x} {}", out.empty() ? "" : ", ",
                           uint32_t(d.AdapterLuid.HighPart), d.AdapterLuid.LowPart, name);
    }
    return out;
}

double MsSince(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

int64_t SteadyNs(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

// ms from a FrameTiming's wake (steady-clock ns) to now
double MsFromWake(int64_t wake_ns) {
    return double(SteadyNs(std::chrono::steady_clock::now()) - wake_ns) / 1e6;
}

}  // namespace

void SetDevice(void* d3d12_device, void* d3d12_queue) {
    std::lock_guard lock(g_device_mutex);
    g_device = static_cast<ID3D12Device*>(d3d12_device);
    g_queue = static_cast<ID3D12CommandQueue*>(d3d12_queue);
}

struct Session::Impl {
    // the loader and the functions taken from it
    HMODULE loader = nullptr;
    PFN_xrGetInstanceProcAddr get_proc = nullptr;
    PFN_xrDestroyInstance destroy_instance = nullptr;
    PFN_xrGetSystem get_system = nullptr;
    PFN_xrGetSystemProperties get_system_properties = nullptr;
    PFN_xrGetInstanceProperties get_instance_properties = nullptr;
    PFN_xrGetD3D12GraphicsRequirementsKHR get_d3d12_requirements = nullptr;
    PFN_xrCreateSession create_session = nullptr;
    PFN_xrPollEvent poll_event = nullptr;
    PFN_xrBeginSession begin_session = nullptr;
    PFN_xrEndSession end_session = nullptr;
    PFN_xrCreateReferenceSpace create_space = nullptr;
    PFN_xrEnumerateViewConfigurationViews enumerate_views = nullptr;
    PFN_xrEnumerateSwapchainFormats enumerate_formats = nullptr;
    PFN_xrCreateSwapchain create_swapchain = nullptr;
    PFN_xrEnumerateSwapchainImages enumerate_images = nullptr;
    PFN_xrAcquireSwapchainImage acquire_image = nullptr;
    PFN_xrWaitSwapchainImage wait_image = nullptr;
    PFN_xrReleaseSwapchainImage release_image = nullptr;
    PFN_xrWaitFrame wait_frame = nullptr;
    PFN_xrBeginFrame begin_frame = nullptr;
    PFN_xrEndFrame end_frame = nullptr;
    PFN_xrLocateViews locate_views = nullptr;
    // XR_KHR_win32_convert_performance_counter_time, where the runtime has
    // it: FrameTiming::display_in_ms
    PFN_xrConvertTimeToWin32PerformanceCounterKHR to_qpc = nullptr;
    LARGE_INTEGER qpc_frequency{};

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;  // between xrBeginSession and xrEndSession
    uint32_t width = 0, height = 0;
    // a swapchain: each eye's, the HUD panel's at the same size, and the
    // dark around the window's, kBlackSize square
    struct Chain {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        std::vector<ComPtr<ID3D12Resource>> images;
        ComPtr<ID3D12DescriptorHeap> rtvs;  // one per image
    } eyes[2], hud, black;
    static constexpr uint32_t kBlackSize = 8;
    // the alpha the dark's image was last cleared to (-1 never); whether an
    // image of its never came back (logged), which leaves the window undarkened
    float black_alpha = -1;
    bool black_failed = false;
    bool window_logged = false;  // the window's first frame, logged
    // the present: present.hlsl into the images, on band3's command list on
    // the SDK's queue; per frame in flight an allocator and kSrvs SRVs (the
    // eyes', the panel's), reused once `fence` passes its value
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    // the panel's: present.hlsl's PSPresentHud (or its linear variant), alpha kept
    ComPtr<ID3D12PipelineState> hud_pipeline;
    ComPtr<ID3D12DescriptorHeap> srvs;
    UINT srv_size = 0, rtv_size = 0;
    static constexpr int kSrvs = 3;
    static constexpr int kInFlight = 3;
    ComPtr<ID3D12CommandAllocator> allocators[kInFlight];
    UINT64 allocator_value[kInFlight] = {};
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    UINT64 fence_value = 0;
    uint64_t presents = 0;
    bool srgb_view = false;
    // the HUD panel's images through an sRGB view (MakeSwapchains)
    bool hud_srgb_view = false;
    // the panel: `fence`'s value once its last picture is copied out of the
    // output (HudFree); whether an image was ever released, which its layer
    // needs
    UINT64 hud_copy_value = 0;
    bool hud_released = false;
    // an image of its never came back (logged): the panel's left as it is
    bool hud_failed = false;
    bool hud_logged = false;  // its first frame's ending, logged
    // the head pose the panel hangs from (HudPlacement), taken at the first
    // located frame and again at a recenter: vr_hud_recenter's change, or the
    // runtime moving LOCAL space, from its frame at recenter_after on
    bool have_head = false, recenter = false;
    int64_t recenter_after = 0;
    vr::XrPose head;
    vr::ChangeTrigger recenter_setting;

    // made on a thread of its own (the runtime may take seconds to start)
    std::thread maker;
    std::atomic<bool> made{false}, failed{false};
    std::atomic<bool> started{false};

    std::mutex stats_mutex;
    Stats stats, measured_from;
    // Record's: the frames' timings (under stats_mutex), the frame before's
    // (the worker's), and the slow-frame log's limit
    vr::FrameRing ring;
    vr::RefreshPeriod refresh;  // WaitFrame's
    vr::FrameTiming last_timing;
    uint64_t timing_frames = 0;
    std::chrono::steady_clock::time_point slow_logged{};
    uint64_t slow_unlogged = 0;
    bool have_frame = false;
    FrameViews last_frame;
    float last_fade = 0;
    std::chrono::steady_clock::time_point fake_start = std::chrono::steady_clock::now();

    void SetState(const std::string& s) {
        std::lock_guard lock(stats_mutex);
        stats.state = s;
    }
    template <typename F>
    bool Load(const char* name, F& fn) {
        if (XR_FAILED(get_proc(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fn))) ||
            !fn) {
            REXLOG_WARN("vr: the OpenXR loader has no {}", name);
            return false;
        }
        return true;
    }
    bool Fail(const std::string& why) {
        REXLOG_WARN("vr: {}; VR is off", why);
        SetState("failed: " + why);
        failed = true;
        return false;
    }
    bool LoadLoader();
    bool Make();
    bool MakeSwapchains();
    bool MakePresent();
    void PollEvents();
};

bool Session::Impl::LoadLoader() {
    // the usual search path (beside band3, PATH), else SteamVR's
    loader = LoadLibraryW(L"openxr_loader.dll");
    if (!loader)
        loader = LoadLibraryW(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\bin\\"
                              L"win64\\openxr_loader.dll");
    if (!loader) return false;
    get_proc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(loader, "xrGetInstanceProcAddr")));
    return get_proc != nullptr;
}

bool Session::Impl::Make() {
    {
        std::lock_guard lock(g_device_mutex);
        device = g_device;
        queue = g_queue;
    }
    if (!device || !queue) return Fail("no Direct3D 12 device (the native renderer isn't on)");
    if (!LoadLoader()) return Fail("no OpenXR loader (openxr_loader.dll)");
    PFN_xrCreateInstance create_instance = nullptr;
    if (!Load("xrCreateInstance", create_instance)) return Fail("the OpenXR loader is broken");
    // and, for the frames' timing only, XrTime as QueryPerformanceCounter's
    // where the runtime offers it
    bool convert_time = false;
    PFN_xrEnumerateInstanceExtensionProperties enumerate_extensions = nullptr;
    if (XR_SUCCEEDED(get_proc(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                              reinterpret_cast<PFN_xrVoidFunction*>(&enumerate_extensions))) &&
        enumerate_extensions) {
        uint32_t n = 0;
        enumerate_extensions(nullptr, 0, &n, nullptr);
        std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
        const char* want = XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME;
        if (n && XR_SUCCEEDED(enumerate_extensions(nullptr, n, &n, props.data())))
            for (const XrExtensionProperties& p : props)
                convert_time |= std::strcmp(p.extensionName, want) == 0;
    }
    const char* extensions[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME,
                                XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::memcpy(ici.applicationInfo.applicationName, "band3", sizeof("band3"));
    std::memcpy(ici.applicationInfo.engineName, "band3", sizeof("band3"));
    ici.applicationInfo.applicationVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = convert_time ? 2 : 1;
    ici.enabledExtensionNames = extensions;
    SetState("starting the runtime");
    XrResult r = create_instance(&ici, &instance);
    if (XR_FAILED(r)) return Fail(fmt::format("xrCreateInstance failed ({})", int(r)));
    if (!Load("xrDestroyInstance", destroy_instance) || !Load("xrGetSystem", get_system) ||
        !Load("xrGetSystemProperties", get_system_properties) ||
        !Load("xrGetInstanceProperties", get_instance_properties) ||
        !Load("xrGetD3D12GraphicsRequirementsKHR", get_d3d12_requirements) ||
        !Load("xrCreateSession", create_session) || !Load("xrPollEvent", poll_event) ||
        !Load("xrBeginSession", begin_session) || !Load("xrEndSession", end_session) ||
        !Load("xrCreateReferenceSpace", create_space) ||
        !Load("xrEnumerateViewConfigurationViews", enumerate_views) ||
        !Load("xrEnumerateSwapchainFormats", enumerate_formats) ||
        !Load("xrCreateSwapchain", create_swapchain) ||
        !Load("xrEnumerateSwapchainImages", enumerate_images) ||
        !Load("xrAcquireSwapchainImage", acquire_image) ||
        !Load("xrWaitSwapchainImage", wait_image) ||
        !Load("xrReleaseSwapchainImage", release_image) || !Load("xrWaitFrame", wait_frame) ||
        !Load("xrBeginFrame", begin_frame) || !Load("xrEndFrame", end_frame) ||
        !Load("xrLocateViews", locate_views))
        return Fail("the runtime lacks a function band3 needs");
    if (convert_time &&
        (XR_FAILED(get_proc(instance, "xrConvertTimeToWin32PerformanceCounterKHR",
                            reinterpret_cast<PFN_xrVoidFunction*>(&to_qpc))) ||
         !QueryPerformanceFrequency(&qpc_frequency)))
        to_qpc = nullptr;
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(get_instance_properties(instance, &ip))) {
        std::lock_guard lock(stats_mutex);
        stats.runtime = fmt::format("{} {}.{}.{}", ip.runtimeName,
                                    XR_VERSION_MAJOR(ip.runtimeVersion),
                                    XR_VERSION_MINOR(ip.runtimeVersion),
                                    XR_VERSION_PATCH(ip.runtimeVersion));
    }
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = get_system(instance, &sgi, &system);
    if (XR_FAILED(r)) return Fail(fmt::format("no headset (xrGetSystem {})", int(r)));
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(get_system_properties(instance, system, &sp))) {
        std::lock_guard lock(stats_mutex);
        stats.system = sp.systemName;
    }
    // must come before xrCreateSession; the headset's GPU must be the SDK's
    XrGraphicsRequirementsD3D12KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
    r = get_d3d12_requirements(instance, system, &req);
    if (XR_FAILED(r))
        return Fail(fmt::format("xrGetD3D12GraphicsRequirementsKHR failed ({})", int(r)));
    const LUID luid = device->GetAdapterLuid();
    if (std::memcmp(&luid, &req.adapterLuid, sizeof(LUID)) != 0)
        REXLOG_WARN("vr: the runtime wants adapter {:08x}:{:08x}, the game's is {:08x}:{:08x} "
                    "({}); trying the game's",
                    uint32_t(req.adapterLuid.HighPart), req.adapterLuid.LowPart,
                    uint32_t(luid.HighPart), luid.LowPart, AdapterNames());
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = device;
    binding.queue = queue;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = system;
    r = create_session(instance, &sci, &session);
    if (XR_FAILED(r)) return Fail(fmt::format("xrCreateSession failed ({})", int(r)));
    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1;
    r = create_space(session, &rsci, &local);
    if (XR_FAILED(r)) return Fail(fmt::format("xrCreateReferenceSpace failed ({})", int(r)));
    if (!MakeSwapchains() || !MakePresent()) return false;
    REXLOG_INFO("vr: {} on {}, {}x{} an eye{}{}", stats.runtime, stats.system, width, height,
                srgb_view ? " (sRGB images)" : "",
                to_qpc ? "" : "; no XrTime conversion, so no display_in_ms");
    SetState("idle");
    made = true;
    return true;
}

bool Session::Impl::MakeSwapchains() {
    uint32_t count = 0;
    XrViewConfigurationView views[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},
                                        {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (XR_FAILED(enumerate_views(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                  &count, views)) ||
        count != 2)
        return Fail("no stereo view configuration");
    width = views[0].recommendedImageRectWidth;
    height = views[0].recommendedImageRectHeight;
    {
        std::lock_guard lock(stats_mutex);
        stats.width = width;
        stats.height = height;
    }
    uint32_t formats_count = 0;
    enumerate_formats(session, 0, &formats_count, nullptr);
    std::vector<int64_t> formats(formats_count);
    enumerate_formats(session, formats_count, &formats_count, formats.data());
    // sRGB, which the runtime shows as display values: gpu_view's bytes
    int64_t format = 0;
    for (int64_t want :
         {int64_t(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB), int64_t(DXGI_FORMAT_R8G8B8A8_UNORM)})
        if (!format && std::find(formats.begin(), formats.end(), want) != formats.end())
            format = want;
    if (!format) return Fail("the runtime offers no RGBA8 swapchain");
    rtv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    // the panel's as the eyes': its picture is drawn at their size, texel
    // for texel, and goes in as theirs do
    for (Chain* chain : {&eyes[0], &eyes[1], &hud, &black}) {
        Chain& e = *chain;
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = format;
        ci.sampleCount = 1;
        ci.width = chain == &black ? kBlackSize : width;
        ci.height = chain == &black ? kBlackSize : height;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        XrResult r = create_swapchain(session, &ci, &e.swapchain);
        if (XR_FAILED(r)) return Fail(fmt::format("xrCreateSwapchain failed ({})", int(r)));
        uint32_t n = 0;
        enumerate_images(e.swapchain, 0, &n, nullptr);
        std::vector<XrSwapchainImageD3D12KHR> images(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        enumerate_images(e.swapchain, n, &n,
                         reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = n;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&e.rtvs))))
            return Fail("no RTV heap");
        for (uint32_t i = 0; i < n; i++) {
            ID3D12Resource* tex = images[i].texture;
            e.images.emplace_back(tex);
            // The picture's bytes are display values: into a typeless image
            // as UNORM, verbatim; into a typed sRGB one through its sRGB
            // view, decoded first (present.hlsl's PSPresentLinear). The HUD
            // panel's premultiplied colour must reach the compositor
            // premultiplied in linear light, which bytes verbatim aren't
            // where they're partly covered: into any image the runtime reads
            // as sRGB (typed so, or typeless in an sRGB swapchain) through an
            // sRGB view, by PSPresentHudLinear.
            const DXGI_FORMAT res = tex->GetDesc().Format;
            const bool srgb =
                res == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                (chain == &hud && res == DXGI_FORMAT_R8G8B8A8_TYPELESS &&
                 format == int64_t(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB));
            D3D12_RENDER_TARGET_VIEW_DESC rd{};
            rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            rd.Format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
            // the dark's is only ever cleared: black at an alpha, the same
            // in either view
            if (chain != &black) (chain == &hud ? hud_srgb_view : srgb_view) = srgb;
            if (i == 0 && chain == &hud)
                REXLOG_INFO("vr: swapchains of DXGI format {}, images of {}; the HUD panel's "
                            "through a{} view",
                            format, int(res), srgb ? "n sRGB" : " UNORM");
            D3D12_CPU_DESCRIPTOR_HANDLE h = e.rtvs->GetCPUDescriptorHandleForHeapStart();
            h.ptr += SIZE_T(i) * rtv_size;
            device->CreateRenderTargetView(tex, &rd, h);
        }
    }
    std::lock_guard lock(stats_mutex);
    stats.srgb_view = srgb_view;
    stats.hud_srgb_view = hud_srgb_view;
    return true;
}

bool Session::Impl::MakePresent() {
    // native_view.cpp's D3D12Present's root signature: b0 PresentConstants
    // (8), t0 the eye's output, s0 linear clamp
    using Serialize = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                       ID3DBlob**, ID3DBlob**);
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d3d12 ? reinterpret_cast<Serialize>(reinterpret_cast<void*>(
                                 GetProcAddress(d3d12, "D3D12SerializeRootSignature")))
                           : nullptr;
    if (!serialize) return Fail("no D3D12SerializeRootSignature");
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 8;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &sampler;
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
        FAILED(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&root))))
        return Fail("no root signature");
    using namespace shaders;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = root.Get();
    pd.VS = {kPresentVertexDxbc, sizeof(kPresentVertexDxbc)};
    pd.PS = srgb_view ? D3D12_SHADER_BYTECODE{kPresentPixelLinearDxbc,
                                              sizeof(kPresentPixelLinearDxbc)}
                      : D3D12_SHADER_BYTECODE{kPresentPixelDxbc, sizeof(kPresentPixelDxbc)};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = srgb_view ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pipeline))))
        return Fail("no present pipeline");
    // the panel's keeps its alpha, which the quad layer blends by
    pd.PS = hud_srgb_view
                ? D3D12_SHADER_BYTECODE{kPresentPixelHudLinearDxbc,
                                        sizeof(kPresentPixelHudLinearDxbc)}
                : D3D12_SHADER_BYTECODE{kPresentPixelHudDxbc, sizeof(kPresentPixelHudDxbc)};
    pd.RTVFormats[0] =
        hud_srgb_view ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&hud_pipeline))))
        return Fail("no present pipeline for the HUD panel");
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSrvs * kInFlight;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvs))))
        return Fail("no SRV heap");
    srv_size = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (auto& a : allocators)
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&a))))
            return Fail("no command allocator");
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(),
                                         nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(list->Close()))
        return Fail("no command list");
    fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return Fail("no fence");
    return true;
}

void Session::Impl::PollEvents() {
    XrEventDataBuffer e{XR_TYPE_EVENT_DATA_BUFFER};
    while (poll_event(instance, &e) == XR_SUCCESS) {
        if (e.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& c = reinterpret_cast<const XrEventDataSessionStateChanged&>(e);
            state = c.state;
            REXLOG_INFO("vr: the session is {}", StateName(state));
            SetState(StateName(state));
            if (state == XR_SESSION_STATE_READY && !running) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                const XrResult r = begin_session(session, &bi);
                if (XR_SUCCEEDED(r)) running = true;
                else REXLOG_WARN("vr: xrBeginSession failed ({})", int(r));
            } else if (state == XR_SESSION_STATE_STOPPING && running) {
                end_session(session);
                running = false;
            } else if (state == XR_SESSION_STATE_EXITING ||
                       state == XR_SESSION_STATE_LOSS_PENDING) {
                // the runtime went away: VR stays off until band3 restarts
                running = false;
                failed = true;
                REXLOG_WARN("vr: the runtime ended the session; VR is off");
            }
        } else if (e.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // the headset's own recenter (SteamVR's seated position): LOCAL
            // space moves to the head, and the panel goes in front again
            const auto& c = reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(e);
            if (c.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                recenter = true;
                recenter_after = c.changeTime;
            }
        } else if (e.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            running = false;
            failed = true;
            SetState("instance lost");
            REXLOG_WARN("vr: the runtime is going away; VR is off");
        }
        e = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

Session& Session::Get() {
    static Session s;
    if (!s.impl_) s.impl_ = new Impl;  // lives as long as the process
    return s;
}

bool Session::Update(bool want) {
    Impl& m = *impl_;
    if (!want || m.failed) return false;
    if (!m.started) {
        m.started = true;
        m.SetState("starting");
        m.maker = std::thread([&m] { m.Make(); });
        m.maker.detach();
        return false;
    }
    if (!m.made) return false;
    m.PollEvents();
    return m.running && !m.failed;
}

uint32_t Session::Width() const { return impl_->width; }
uint32_t Session::Height() const { return impl_->height; }

bool Session::WaitFrame(FrameViews& f) {
    Impl& m = *impl_;
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    const auto wait_start = std::chrono::steady_clock::now();
    XrResult r = m.wait_frame(m.session, &wi, &fs);
    const auto woke = std::chrono::steady_clock::now();
    LARGE_INTEGER woke_qpc{};
    if (m.to_qpc) QueryPerformanceCounter(&woke_qpc);
    const double waited = std::chrono::duration<double, std::milli>(woke - wait_start).count();
    if (XR_FAILED(r)) {
        REXLOG_WARN("vr: xrWaitFrame failed ({})", int(r));
        return false;
    }
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    r = m.begin_frame(m.session, &bi);
    if (XR_FAILED(r)) {
        REXLOG_WARN("vr: xrBeginFrame failed ({})", int(r));
        return false;
    }
    vr::FrameTiming& t = f.timing;
    t = {};
    t.index = ++m.timing_frames;
    t.wake_ns = SteadyNs(woke);
    t.display_time_ns = fs.predictedDisplayTime;
    t.period_ns = fs.predictedDisplayPeriod;
    t.refresh_ns = m.refresh.Add(fs.predictedDisplayPeriod);
    t.should_render = fs.shouldRender == XR_TRUE;
    t.wait_ms = waited;
    t.begun_ms = MsSince(woke);
    LARGE_INTEGER display_qpc{};
    if (m.to_qpc && m.qpc_frequency.QuadPart &&
        XR_SUCCEEDED(m.to_qpc(m.instance, fs.predictedDisplayTime, &display_qpc)))
        t.display_in_ms = double(display_qpc.QuadPart - woke_qpc.QuadPart) * 1000.0 /
                          double(m.qpc_frequency.QuadPart);
    f.display_time = fs.predictedDisplayTime;
    f.display_period = fs.predictedDisplayPeriod;
    f.render = fs.shouldRender == XR_TRUE;
    {
        std::lock_guard lock(m.stats_mutex);
        m.stats.wait_ms += waited;
        m.stats.display_period_ns = t.refresh_ns;
        m.stats.predicted_period_ns = fs.predictedDisplayPeriod;
    }
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime;
    li.space = m.local;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    const bool located = XR_SUCCEEDED(m.locate_views(m.session, &li, &vs, 2, &count, views)) &&
                         count == 2;
    for (int e = 0; e < 2; e++) {
        // without a view, a forward-facing head with a 64 mm IPD and 90
        // degrees each way
        vr::XrPose& p = f.pose[e];
        p = {};
        p.p[0] = e ? 0.032f : -0.032f;
        f.fov[e] = {-0.785f, 0.785f, 0.785f, -0.785f};
        if (!located) continue;
        const XrFovf& fov = views[e].fov;
        f.fov[e] = {fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown};
        if ((vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
            (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
            f.located = true;
            const XrPosef& v = views[e].pose;
            p.q[0] = v.orientation.x;
            p.q[1] = v.orientation.y;
            p.q[2] = v.orientation.z;
            p.q[3] = v.orientation.w;
            p.p[0] = v.position.x;
            p.p[1] = v.position.y;
            p.p[2] = v.position.z;
        }
    }
    if (REXCVAR_GET(vr_fake_pose)) {
        // a slow look from side to side (40 degrees each way over 12 s) and
        // a nod, the eyes 64 mm apart
        const double t =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - m.fake_start).count();
        const float yaw = float(0.7 * std::sin(t * 2 * 3.14159265 / 12));
        const float pitch = float(0.15 * std::sin(t * 2 * 3.14159265 / 7));
        // yaw about XR's y, then pitch about x: q = q_yaw * q_pitch
        const float cy = std::cos(yaw / 2), sy = std::sin(yaw / 2);
        const float cp = std::cos(pitch / 2), sp = std::sin(pitch / 2);
        const float q[4] = {cy * sp, sy * cp, -sy * sp, cy * cp};
        for (int e = 0; e < 2; e++) {
            vr::XrPose& p = f.pose[e];
            std::memcpy(p.q, q, sizeof(q));
            const float right[3] = {e ? 0.032f : -0.032f, 0, 0};
            vr::Rotate(q, right, p.p);
        }
        f.located = true;
    }
    // the HUD panel's head: at the first located frame, then at a recenter
    if (m.recenter_setting.Fired(REXCVAR_GET(vr_hud_recenter))) {
        m.recenter = true;
        m.recenter_after = 0;
    }
    if (f.located && (!m.have_head || (m.recenter && f.display_time >= m.recenter_after))) {
        m.head = vr::HeadPose(f.pose[0], f.pose[1]);
        REXLOG_INFO("vr: the HUD panel {}, the head at {:.2f},{:.2f},{:.2f} m",
                    m.have_head ? "recentred" : "placed", m.head.p[0], m.head.p[1], m.head.p[2]);
        if (m.have_head) {
            std::lock_guard lock(m.stats_mutex);
            m.stats.recenters++;
        }
        m.have_head = true;
        m.recenter = false;
    }
    f.have_head = m.have_head;
    f.head = m.head;
    t.located_ms = MsSince(woke);
    return true;
}

bool Session::HudFree() {
    Impl& m = *impl_;
    return !m.fence || m.fence->GetCompletedValue() >= m.hud_copy_value;
}

bool Session::CopiesDone() {
    Impl& m = *impl_;
    return !m.fence || m.fence->GetCompletedValue() >= m.fence_value;
}

bool Session::Present(FrameViews& f, void* const eye[2], void* hud,
                      const vr::FrameLayout& layout, double begin_to_present_ms,
                      const QueueWait* waits) {
    Impl& m = *impl_;
    const auto start = std::chrono::steady_clock::now();
    vr::FrameTiming& t = f.timing;
    t.present_ms = MsFromWake(t.wake_ns);
    // each step's ms, from `from` on
    auto lap = [](std::chrono::steady_clock::time_point& from) {
        const auto now = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - from).count();
        from = now;
        return ms;
    };
    auto step = start;
    XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                 {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    // drawn even when the runtime says it won't show them (shouldRender off,
    // as SteamVR's idle null headset keeps it): counted apart
    bool drawn = true;
    // the panel's image acquired, and drawn with the new picture
    bool hud_acquired = false, hud_drawn = false;
    if (drawn) {
        // the frame in flight this allocator and these SRVs had
        const int k = int(m.presents % Impl::kInFlight);
        if (m.fence->GetCompletedValue() < m.allocator_value[k]) {
            m.fence->SetEventOnCompletion(m.allocator_value[k], m.fence_event);
            WaitForSingleObject(m.fence_event, 1000);
        }
        t.fence_wait_ms = lap(step);
        uint32_t index[2] = {};
        for (int e = 0; e < 2 && drawn; e++) {
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = 100000000;  // 100 ms
            if (XR_FAILED(m.acquire_image(m.eyes[e].swapchain, &ai, &index[e])) ||
                XR_FAILED(m.wait_image(m.eyes[e].swapchain, &wi))) {
                REXLOG_WARN("vr: couldn't get a swapchain image");
                drawn = false;
            }
        }
        t.acquire_ms = lap(step);
        // The panel's image only for a new picture, and only with the eyes';
        // without one the image last released stays up
        uint32_t hud_index = 0;
        if (drawn && hud && !m.hud_failed) {
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = 100000000;  // 100 ms
            hud_acquired = XR_SUCCEEDED(m.acquire_image(m.hud.swapchain, &ai, &hud_index));
            // an image may be released only once waited for: one the
            // compositor never gives up leaves the panel as it is, for good
            XrResult r = XR_ERROR_RUNTIME_FAILURE;
            for (int tries = 0; hud_acquired && tries < 3; tries++) {
                r = m.wait_image(m.hud.swapchain, &wi);
                if (r != XR_TIMEOUT_EXPIRED) break;
            }
            hud_drawn = hud_acquired && r == XR_SUCCESS;
            if (!hud_drawn) {
                REXLOG_WARN("vr: couldn't get the HUD panel's swapchain image ({}); the panel "
                            "stays as it is",
                            int(r));
                m.hud_failed = true;
            }
        }
        // The dark's image, cleared again only as vr_window_dim changes: its
        // layers show the image last released
        uint32_t black_index = 0;
        bool black_drawn = false;
        if (drawn && layout.frame_layers && !m.black_failed && m.black_alpha != layout.dim) {
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = 100000000;  // 100 ms
            const bool acquired =
                XR_SUCCEEDED(m.acquire_image(m.black.swapchain, &ai, &black_index));
            XrResult r = XR_ERROR_RUNTIME_FAILURE;
            for (int tries = 0; acquired && tries < 3; tries++) {
                r = m.wait_image(m.black.swapchain, &wi);
                if (r != XR_TIMEOUT_EXPIRED) break;
            }
            black_drawn = acquired && r == XR_SUCCESS;
            if (!black_drawn) {
                REXLOG_WARN("vr: couldn't get the window's dark swapchain image ({}); the window "
                            "goes without it",
                            int(r));
                m.black_failed = true;
            }
        }
        t.hud_acquire_ms = lap(step);
        if (drawn) {
            m.allocators[k]->Reset();
            m.list->Reset(m.allocators[k].Get(), nullptr);
            ID3D12DescriptorHeap* heaps[] = {m.srvs.Get()};
            m.list->SetDescriptorHeaps(1, heaps);
            m.list->SetGraphicsRootSignature(m.root.Get());
            m.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            // output `tex` into image `image` of `chain` by `pipeline`, through
            // this frame's SRV `srv`, faded `fade` to black; or the image
            // black, with `black`
            auto copy = [&](Impl::Chain& chain, uint32_t image, ID3D12Resource* tex, int srv,
                            ID3D12PipelineState* pipeline, float fade, bool black) {
                D3D12_CPU_DESCRIPTOR_HANDLE rtv = chain.rtvs->GetCPUDescriptorHandleForHeapStart();
                rtv.ptr += SIZE_T(image) * m.rtv_size;
                if (black) {
                    const float colour[4] = {0, 0, 0, 1};
                    m.list->ClearRenderTargetView(rtv, colour, 0, nullptr);
                    return;
                }
                // no picture: the left eye dark red, the right dark blue
                if (!tex) {
                    const float colour[2][4] = {{0.4f, 0, 0, 1}, {0, 0, 0.4f, 1}};
                    m.list->ClearRenderTargetView(rtv, colour[srv & 1], 0, nullptr);
                    return;
                }
                const D3D12_RESOURCE_DESC td = tex->GetDesc();
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels = 1;
                const int slot = Impl::kSrvs * k + srv;
                D3D12_CPU_DESCRIPTOR_HANDLE cpu = m.srvs->GetCPUDescriptorHandleForHeapStart();
                cpu.ptr += SIZE_T(slot) * m.srv_size;
                m.device->CreateShaderResourceView(tex, &sd, cpu);
                D3D12_GPU_DESCRIPTOR_HANDLE gpu = m.srvs->GetGPUDescriptorHandleForHeapStart();
                gpu.ptr += UINT64(slot) * m.srv_size;
                m.list->SetPipelineState(pipeline);
                m.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                const D3D12_VIEWPORT vp{0, 0, float(m.width), float(m.height), 0, 1};
                m.list->RSSetViewports(1, &vp);
                const D3D12_RECT sc{0, 0, LONG(m.width), LONG(m.height)};
                m.list->RSSetScissorRects(1, &sc);
                // present.hlsl's PresentConstants: the whole image, texel for
                // texel when the output is its size, and the fade
                const float c[8] = {0, 0, float(m.width), float(m.height), float(td.Width),
                                    float(td.Height),
                                    td.Width == m.width && td.Height == m.height ? 1.0f : 0.0f,
                                    fade};
                m.list->SetGraphicsRoot32BitConstants(0, 8, c, 0);
                m.list->SetGraphicsRootDescriptorTable(1, gpu);
                m.list->DrawInstanced(3, 1, 0, 0);
            };
            for (int e = 0; e < 2; e++)
                copy(m.eyes[e], index[e], static_cast<ID3D12Resource*>(eye[e]), e,
                     m.pipeline.Get(), layout.fade_drawn ? 0.0f : layout.fade, layout.eyes_black);
            if (hud_drawn)
                copy(m.hud, hud_index, static_cast<ID3D12Resource*>(hud), 2,
                     m.hud_pipeline.Get(), 0, false);
            // the dark: black, premultiplied, covering by vr_window_dim
            if (black_drawn) {
                D3D12_CPU_DESCRIPTOR_HANDLE rtv =
                    m.black.rtvs->GetCPUDescriptorHandleForHeapStart();
                rtv.ptr += SIZE_T(black_index) * m.rtv_size;
                const float colour[4] = {0, 0, 0, layout.dim};
                m.list->ClearRenderTargetView(rtv, colour, 0, nullptr);
            }
            m.list->Close();
            // the outputs' drawing on SDL_gpu's queue, waited for on this one
            // rather than on the CPU
            for (int i = 0; waits && i < kQueueWaits; i++) {
                if (!waits[i].fence) continue;
                m.queue->Wait(static_cast<ID3D12Fence*>(waits[i].fence), waits[i].value);
                t.queue_wait = true;
            }
            ID3D12CommandList* lists[] = {m.list.Get()};
            m.queue->ExecuteCommandLists(1, lists);
            m.queue->Signal(m.fence.Get(), ++m.fence_value);
            m.allocator_value[k] = m.fence_value;
            // the panel's output may be drawn again once this passes (HudFree)
            if (hud_drawn) m.hud_copy_value = m.fence_value;
            m.presents++;
        }
        t.copy_ms = lap(step);
        // released whether drawn or not: what was acquired goes back
        for (int e = 0; e < 2; e++) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            m.release_image(m.eyes[e].swapchain, &ri);
        }
        if (hud_drawn) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            if (XR_SUCCEEDED(m.release_image(m.hud.swapchain, &ri))) m.hud_released = true;
        }
        if (black_drawn) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            if (XR_SUCCEEDED(m.release_image(m.black.swapchain, &ri))) m.black_alpha = layout.dim;
            else m.black_failed = true;
        }
        t.release_ms = lap(step);
        for (int e = 0; e < 2; e++) {
            views[e].pose.orientation = {f.pose[e].q[0], f.pose[e].q[1], f.pose[e].q[2],
                                         f.pose[e].q[3]};
            views[e].pose.position = {f.pose[e].p[0], f.pose[e].p[1], f.pose[e].p[2]};
            views[e].fov = {f.fov[e].left, f.fov[e].right, f.fov[e].up, f.fov[e].down};
            views[e].subImage.swapchain = m.eyes[e].swapchain;
            views[e].subImage.imageRect = {{0, 0}, {int32_t(m.width), int32_t(m.height)}};
        }
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = m.local;
    layer.viewCount = 2;
    layer.views = views;
    // the layers, bottom up: the eyes, the dark around the window, the panel
    const XrCompositionLayerBaseHeader* layers[2 + vr::kFrameQuads] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    uint32_t layer_count = 1;
    // a quad layer of `chain`'s whole image, blended by its alpha
    // (premultiplied colour: not unpremultiplied first)
    auto quad_of = [&](const Impl::Chain& chain, uint32_t w, uint32_t h, const vr::HudQuad& q) {
        XrCompositionLayerQuad l{XR_TYPE_COMPOSITION_LAYER_QUAD};
        l.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        l.space = m.local;
        l.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        l.subImage.swapchain = chain.swapchain;
        l.subImage.imageRect = {{0, 0}, {int32_t(w), int32_t(h)}};
        l.pose.orientation = {q.pose.q[0], q.pose.q[1], q.pose.q[2], q.pose.q[3]};
        l.pose.position = {q.pose.p[0], q.pose.p[1], q.pose.p[2]};
        l.size = {q.size[0], q.size[1]};
        return l;
    };
    XrCompositionLayerQuad frames[vr::kFrameQuads];
    const bool show_frames = drawn && layout.frame_layers && m.black_alpha >= 0;
    if (show_frames) {
        for (int i = 0; i < layout.frame_quads && i < vr::kFrameQuads; i++) {
            frames[i] = quad_of(m.black, Impl::kBlackSize, Impl::kBlackSize, layout.frame[i]);
            layers[layer_count++] =
                reinterpret_cast<const XrCompositionLayerBaseHeader*>(&frames[i]);
        }
    }
    // The HUD panel over them all, once its image was ever released (the
    // last released is what the layer shows), where the layout says
    const bool show_hud = drawn && m.hud_released && layout.hud;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (show_hud) {
        quad = quad_of(m.hud, m.width, m.height, layout.hud_quad);
        layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = f.display_time;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = drawn ? layer_count : 0;
    ei.layers = drawn ? layers : nullptr;
    step = std::chrono::steady_clock::now();
    const XrResult r = m.end_frame(m.session, &ei);
    t.end_frame_ms = lap(step);
    t.ended_ms = MsFromWake(t.wake_ns);
    t.layer = drawn && XR_SUCCEEDED(r);
    const double present_ms = MsSince(start);
    if (show_hud && XR_SUCCEEDED(r) && !m.hud_logged) {
        m.hud_logged = true;
        REXLOG_INFO("vr: the HUD panel's quad layer is up: {:.2f} x {:.2f} m at "
                    "{:.2f},{:.2f},{:.2f} m",
                    quad.size.width, quad.size.height, quad.pose.position.x,
                    quad.pose.position.y, quad.pose.position.z);
    }
    if (layout.window && drawn && XR_SUCCEEDED(r) && !m.window_logged) {
        m.window_logged = true;
        const vr::WindowFrame& w = layout.window_frame;
        REXLOG_INFO("vr: the game camera's window is up: {:.2f} x {:.2f} m at {:.2f} m ({:.1f} "
                    "degrees across), the game's {:.1f} x {:.1f} degrees in it ({:.2f}x), the "
                    "panel {}, the dark around it {}",
                    w.size[0], w.size[1], w.distance,
                    2 * std::atan2(w.size[0] / 2, w.distance) * 57.29578f,
                    layout.game_fov.hfov * 57.29578f, layout.game_fov.vfov * 57.29578f,
                    layout.magnify, layout.hud_in_window ? "in it" : "on its own place",
                    show_frames            ? "as quad layers"
                    : layout.frame_quads   ? "drawn by the eyes"
                                           : "off");
    }
    {
        std::lock_guard lock(m.stats_mutex);
        if (hud_drawn) m.stats.hud_copies++;
        if (show_hud) m.stats.hud_layers++;
        if (drawn && layout.window) m.stats.window_frames++;
        if (drawn && layout.eyes_black) m.stats.black_frames++;
        if (drawn && layout.track_count > 0) m.stats.track_frames++;
        if (drawn && layout.fade >= 1) m.stats.fades++;
        (drawn ? m.stats.frames : m.stats.skipped)++;
        if (!f.render) m.stats.hidden++;
        m.stats.present_ms += present_ms;
        const double frame_ms = begin_to_present_ms + present_ms;
        m.stats.frame_ms += frame_ms;
        const double period_ms = vr::PeriodMs(t);
        if (period_ms > 0 && frame_ms > period_ms) m.stats.late++;
    }
    if (XR_FAILED(r)) {
        REXLOG_WARN("vr: xrEndFrame failed ({}, {} layers)", int(r), ei.layerCount);
        return false;
    }
    return drawn;
}

void Session::AddRenderMs(int eye, double ms, double gpu_ms, double repose_ms) {
    std::lock_guard lock(impl_->stats_mutex);
    impl_->stats.render_ms[eye & 1] += ms;
    impl_->stats.gpu_ms[eye & 1] += gpu_ms;
    impl_->stats.repose_ms[eye & 1] += repose_ms;
}

void Session::AddHudMs(double ms, double gpu_ms) {
    std::lock_guard lock(impl_->stats_mutex);
    impl_->stats.hud_renders++;
    impl_->stats.hud_render_ms += ms;
    impl_->stats.hud_gpu_ms += gpu_ms;
}

void Session::Record(vr::FrameTiming& t) {
    Impl& m = *impl_;
    vr::Classify(t, m.last_timing.index ? &m.last_timing : nullptr);
    m.last_timing = t;
    {
        std::lock_guard lock(m.stats_mutex);
        m.ring.Push(t);
        Stats& s = m.stats;
        s.recorded = t.index;
        s.missed += t.missed;
        s.slots_missed += t.slots_missed;
        s.over_period += t.over_period;
        s.late_wakes += t.late_wake;
        s.repeats += t.repeat;
        s.behind += t.newer > 0;
        s.slow += t.slow;
        s.throttled += t.throttled;
        s.queue_waits += t.queue_wait;
    }
    if (!t.slow) return;
    // at most one a second, saying how many weren't
    const auto now = std::chrono::steady_clock::now();
    if (now - m.slow_logged < std::chrono::seconds(1)) {
        m.slow_unlogged++;
        return;
    }
    REXLOG_INFO("vr: slow {}{}", vr::Breakdown(t),
                m.slow_unlogged ? fmt::format(" ({} more slow since the last logged)",
                                              m.slow_unlogged)
                                : std::string());
    m.slow_logged = now;
    m.slow_unlogged = 0;
}

std::string Session::FramesCsv(uint64_t& frames) {
    std::vector<vr::FrameTiming> all;
    {
        std::lock_guard lock(impl_->stats_mutex);
        all = impl_->ring.Since(0);
    }
    frames = all.size();
    std::string out = vr::CsvHeader();
    out.reserve(out.size() + all.size() * 400);
    for (const vr::FrameTiming& t : all) out += vr::CsvRow(t);
    return out;
}

Stats Session::GetStats() {
    std::lock_guard lock(impl_->stats_mutex);
    return impl_->stats;
}

void Session::StartMeasuring() {
    std::lock_guard lock(impl_->stats_mutex);
    impl_->measured_from = impl_->stats;
}

std::string Session::MeasuredJson() {
    if (!impl_->started) return {};
    Stats now, from;
    std::vector<vr::FrameTiming> timed;
    {
        std::lock_guard lock(impl_->stats_mutex);
        now = impl_->stats;
        from = impl_->measured_from;
        // those of the ring recorded since (all of them, if the ring has
        // wrapped since)
        timed = impl_->ring.Since(from.recorded);
    }
    const uint64_t frames = now.frames - from.frames;
    const uint64_t ended = frames + now.skipped - from.skipped;
    auto per = [&](double a, double b, uint64_t n) { return n ? (a - b) / double(n) : 0.0; };
    std::string q = "\"";
    auto text = [&](const std::string& s) {
        std::string o = q;
        for (char c : s) o += c == '"' || c == '\\' ? '\'' : c;
        return o + q;
    };
    // the HUD panel: its pictures' drawing per picture, and spread over every
    // frame as the eyes' are, to set beside theirs
    const uint64_t renders = now.hud_renders - from.hud_renders;
    const std::string hud = fmt::format(
        "{{\"renders\":{},\"copies\":{},\"layers\":{},\"recenters\":{},"
        "\"render_ms_per_render\":{:.3f},\"gpu_ms_per_render\":{:.3f},"
        "\"render_ms_per_frame\":{:.3f},\"gpu_ms_per_frame\":{:.3f},\"srgb_view\":{}}}",
        renders, now.hud_copies - from.hud_copies, now.hud_layers - from.hud_layers,
        now.recenters - from.recenters, per(now.hud_render_ms, from.hud_render_ms, renders),
        per(now.hud_gpu_ms, from.hud_gpu_ms, renders),
        per(now.hud_render_ms, from.hud_render_ms, frames),
        per(now.hud_gpu_ms, from.hud_gpu_ms, frames), now.hud_srgb_view ? "true" : "false");
    return fmt::format(
        "{{\"state\":{},\"runtime\":{},\"system\":{},\"width\":{},\"height\":{},"
        "\"display_period_ms\":{:.3f},\"frames\":{},\"skipped\":{},\"hidden\":{},\"late\":{},"
        "\"wait_ms_per_frame\":{:.3f},\"render_ms_per_frame\":[{:.3f},{:.3f}],"
        "\"gpu_ms_per_frame\":[{:.3f},{:.3f}],\"repose_ms_per_frame\":[{:.3f},{:.3f}],"
        "\"present_ms_per_frame\":{:.3f},\"frame_ms_per_frame\":{:.3f},\"srgb_view\":{},"
        "\"hud\":{},\"missed\":{},\"slots_missed\":{},\"over_period\":{},\"late_wakes\":{},"
        "\"repeats\":{},\"behind_newer\":{},\"slow\":{},\"window_frames\":{},"
        "\"black_frames\":{},\"fades\":{},\"track_frames\":{},\"predicted_period_ms\":{:.3f},"
        "\"throttled\":{},\"queue_waits\":{},\"timing\":{}}}",
        text(now.state), text(now.runtime), text(now.system), now.width, now.height,
        double(now.display_period_ns) / 1e6, frames, now.skipped - from.skipped,
        now.hidden - from.hidden, now.late - from.late, per(now.wait_ms, from.wait_ms, ended),
        per(now.render_ms[0], from.render_ms[0], frames),
        per(now.render_ms[1], from.render_ms[1], frames),
        per(now.gpu_ms[0], from.gpu_ms[0], frames), per(now.gpu_ms[1], from.gpu_ms[1], frames),
        per(now.repose_ms[0], from.repose_ms[0], frames),
        per(now.repose_ms[1], from.repose_ms[1], frames),
        per(now.present_ms, from.present_ms, ended), per(now.frame_ms, from.frame_ms, ended),
        now.srgb_view ? "true" : "false", hud, now.missed - from.missed,
        now.slots_missed - from.slots_missed, now.over_period - from.over_period,
        now.late_wakes - from.late_wakes, now.repeats - from.repeats, now.behind - from.behind,
        now.slow - from.slow, now.window_frames - from.window_frames,
        now.black_frames - from.black_frames, now.fades - from.fades,
        now.track_frames - from.track_frames, double(now.predicted_period_ns) / 1e6, now.throttled - from.throttled,
        now.queue_waits - from.queue_waits, vr::TimingJson(timed));
}

bool Session::LastFrame(FrameViews& f, float& fade, uint32_t& width, uint32_t& height) {
    std::lock_guard lock(impl_->stats_mutex);
    if (!impl_->have_frame) return false;
    f = impl_->last_frame;
    fade = impl_->last_fade;
    width = impl_->width;
    height = impl_->height;
    return true;
}

void Session::NoteFrame(const FrameViews& f, float fade) {
    std::lock_guard lock(impl_->stats_mutex);
    impl_->last_frame = f;
    impl_->last_fade = fade;
    impl_->have_frame = true;
}

#endif

}  // namespace band3::render::xr
