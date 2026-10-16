#include "src/Render/xr_present.h"

#include <algorithm>
#include <mutex>

#include <fmt/format.h>
#include <rex/logging.h>

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
#include "src/settings.h"

// the headers only: every function comes from the loader at run time
#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D12
#include "src/ThirdParty/openxr/include/openxr/openxr.h"
#include "src/ThirdParty/openxr/include/openxr/openxr_platform.h"
#endif

namespace band3::render::xr {

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
bool Session::Present(const FrameViews&, void* const[2], double) { return false; }
void Session::AddRenderMs(int, double, double, double) {}
Stats Session::GetStats() { return {}; }
bool Session::LastEyes(vr::Eye[2], uint32_t&, uint32_t&) { return false; }
void Session::NoteEyes(const vr::Eye[2]) {}
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

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;  // between xrBeginSession and xrEndSession
    uint32_t width = 0, height = 0;
    struct Eye {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        std::vector<ComPtr<ID3D12Resource>> images;
        ComPtr<ID3D12DescriptorHeap> rtvs;  // one per image
    } eyes[2];
    // the present: present.hlsl into the images, on band3's command list on
    // the SDK's queue; per frame in flight an allocator and two SRVs, reused
    // once `fence` passes its value
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12DescriptorHeap> srvs;
    UINT srv_size = 0, rtv_size = 0;
    static constexpr int kInFlight = 3;
    ComPtr<ID3D12CommandAllocator> allocators[kInFlight];
    UINT64 allocator_value[kInFlight] = {};
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    UINT64 fence_value = 0;
    uint64_t presents = 0;
    bool srgb_view = false;

    // made on a thread of its own (the runtime may take seconds to start)
    std::thread maker;
    std::atomic<bool> made{false}, failed{false};
    std::atomic<bool> started{false};

    std::mutex stats_mutex;
    Stats stats, measured_from;
    bool have_eyes = false;
    vr::Eye last_eyes[2];
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
    const char* extensions[] = {XR_KHR_D3D12_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::memcpy(ici.applicationInfo.applicationName, "band3", sizeof("band3"));
    std::memcpy(ici.applicationInfo.engineName, "band3", sizeof("band3"));
    ici.applicationInfo.applicationVersion = 1;
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = 1;
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
    REXLOG_INFO("vr: {} on {}, {}x{} an eye{}", stats.runtime, stats.system, width, height,
                srgb_view ? " (sRGB images)" : "");
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
    for (Eye& e : eyes) {
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format = format;
        ci.sampleCount = 1;
        ci.width = width;
        ci.height = height;
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
            // view, decoded first (present.hlsl's PSPresentLinear)
            const DXGI_FORMAT res = tex->GetDesc().Format;
            D3D12_RENDER_TARGET_VIEW_DESC rd{};
            rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            rd.Format = res == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                                                               : DXGI_FORMAT_R8G8B8A8_UNORM;
            srgb_view = rd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            D3D12_CPU_DESCRIPTOR_HANDLE h = e.rtvs->GetCPUDescriptorHandleForHeapStart();
            h.ptr += SIZE_T(i) * rtv_size;
            device->CreateRenderTargetView(tex, &rd, h);
        }
    }
    std::lock_guard lock(stats_mutex);
    stats.srgb_view = srgb_view;
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
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2 * kInFlight;
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
    const double waited = MsSince(wait_start);
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
    f.display_time = fs.predictedDisplayTime;
    f.display_period = fs.predictedDisplayPeriod;
    f.render = fs.shouldRender == XR_TRUE;
    {
        std::lock_guard lock(m.stats_mutex);
        m.stats.wait_ms += waited;
        m.stats.display_period_ns = fs.predictedDisplayPeriod;
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
    }
    return true;
}

bool Session::Present(const FrameViews& f, void* const eye[2], double begin_to_present_ms) {
    Impl& m = *impl_;
    const auto start = std::chrono::steady_clock::now();
    XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                 {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    // drawn even when the runtime says it won't show them (shouldRender off,
    // as SteamVR's idle null headset keeps it): counted apart
    bool drawn = true;
    if (drawn) {
        // the frame in flight this allocator and these SRVs had
        const int k = int(m.presents % Impl::kInFlight);
        if (m.fence->GetCompletedValue() < m.allocator_value[k]) {
            m.fence->SetEventOnCompletion(m.allocator_value[k], m.fence_event);
            WaitForSingleObject(m.fence_event, 1000);
        }
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
        if (drawn) {
            m.allocators[k]->Reset();
            m.list->Reset(m.allocators[k].Get(), nullptr);
            ID3D12DescriptorHeap* heaps[] = {m.srvs.Get()};
            m.list->SetDescriptorHeaps(1, heaps);
            m.list->SetGraphicsRootSignature(m.root.Get());
            m.list->SetPipelineState(m.pipeline.Get());
            m.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (int e = 0; e < 2; e++) {
                D3D12_CPU_DESCRIPTOR_HANDLE rtv =
                    m.eyes[e].rtvs->GetCPUDescriptorHandleForHeapStart();
                rtv.ptr += SIZE_T(index[e]) * m.rtv_size;
                // no picture: the left eye dark red, the right dark blue
                if (!eye[e]) {
                    const float colour[2][4] = {{0.4f, 0, 0, 1}, {0, 0, 0.4f, 1}};
                    m.list->ClearRenderTargetView(rtv, colour[e], 0, nullptr);
                    continue;
                }
                auto* tex = static_cast<ID3D12Resource*>(eye[e]);
                const D3D12_RESOURCE_DESC td = tex->GetDesc();
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE cpu = m.srvs->GetCPUDescriptorHandleForHeapStart();
                cpu.ptr += SIZE_T(2 * k + e) * m.srv_size;
                m.device->CreateShaderResourceView(tex, &sd, cpu);
                D3D12_GPU_DESCRIPTOR_HANDLE gpu = m.srvs->GetGPUDescriptorHandleForHeapStart();
                gpu.ptr += UINT64(2 * k + e) * m.srv_size;
                m.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                const D3D12_VIEWPORT vp{0, 0, float(m.width), float(m.height), 0, 1};
                m.list->RSSetViewports(1, &vp);
                const D3D12_RECT sc{0, 0, LONG(m.width), LONG(m.height)};
                m.list->RSSetScissorRects(1, &sc);
                // present.hlsl's PresentConstants: the whole image, texel for
                // texel when the output is its size
                const float c[8] = {0, 0, float(m.width), float(m.height), float(td.Width),
                                    float(td.Height),
                                    td.Width == m.width && td.Height == m.height ? 1.0f : 0.0f,
                                    0};
                m.list->SetGraphicsRoot32BitConstants(0, 8, c, 0);
                m.list->SetGraphicsRootDescriptorTable(1, gpu);
                m.list->DrawInstanced(3, 1, 0, 0);
            }
            m.list->Close();
            ID3D12CommandList* lists[] = {m.list.Get()};
            m.queue->ExecuteCommandLists(1, lists);
            m.queue->Signal(m.fence.Get(), ++m.fence_value);
            m.allocator_value[k] = m.fence_value;
            m.presents++;
        }
        // released whether drawn or not: what was acquired goes back
        for (int e = 0; e < 2; e++) {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            m.release_image(m.eyes[e].swapchain, &ri);
        }
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
    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = f.display_time;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = drawn ? 1 : 0;
    ei.layers = drawn ? layers : nullptr;
    const XrResult r = m.end_frame(m.session, &ei);
    const double present_ms = MsSince(start);
    {
        std::lock_guard lock(m.stats_mutex);
        (drawn ? m.stats.frames : m.stats.skipped)++;
        if (!f.render) m.stats.hidden++;
        m.stats.present_ms += present_ms;
        const double frame_ms = begin_to_present_ms + present_ms;
        m.stats.frame_ms += frame_ms;
        if (f.display_period > 0 && frame_ms > double(f.display_period) / 1e6) m.stats.late++;
    }
    if (XR_FAILED(r)) {
        REXLOG_WARN("vr: xrEndFrame failed ({})", int(r));
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
    {
        std::lock_guard lock(impl_->stats_mutex);
        now = impl_->stats;
        from = impl_->measured_from;
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
    return fmt::format(
        "{{\"state\":{},\"runtime\":{},\"system\":{},\"width\":{},\"height\":{},"
        "\"display_period_ms\":{:.3f},\"frames\":{},\"skipped\":{},\"hidden\":{},\"late\":{},"
        "\"wait_ms_per_frame\":{:.3f},\"render_ms_per_frame\":[{:.3f},{:.3f}],"
        "\"gpu_ms_per_frame\":[{:.3f},{:.3f}],\"repose_ms_per_frame\":[{:.3f},{:.3f}],"
        "\"present_ms_per_frame\":{:.3f},\"frame_ms_per_frame\":{:.3f},\"srgb_view\":{}}}",
        text(now.state), text(now.runtime), text(now.system), now.width, now.height,
        double(now.display_period_ns) / 1e6, frames, now.skipped - from.skipped,
        now.hidden - from.hidden, now.late - from.late, per(now.wait_ms, from.wait_ms, ended),
        per(now.render_ms[0], from.render_ms[0], frames),
        per(now.render_ms[1], from.render_ms[1], frames),
        per(now.gpu_ms[0], from.gpu_ms[0], frames), per(now.gpu_ms[1], from.gpu_ms[1], frames),
        per(now.repose_ms[0], from.repose_ms[0], frames),
        per(now.repose_ms[1], from.repose_ms[1], frames),
        per(now.present_ms, from.present_ms, ended), per(now.frame_ms, from.frame_ms, ended),
        now.srgb_view ? "true" : "false");
}

bool Session::LastEyes(vr::Eye eyes[2], uint32_t& width, uint32_t& height) {
    std::lock_guard lock(impl_->stats_mutex);
    if (!impl_->have_eyes) return false;
    eyes[0] = impl_->last_eyes[0];
    eyes[1] = impl_->last_eyes[1];
    width = impl_->width;
    height = impl_->height;
    return true;
}

void Session::NoteEyes(const vr::Eye eyes[2]) {
    std::lock_guard lock(impl_->stats_mutex);
    impl_->last_eyes[0] = eyes[0];
    impl_->last_eyes[1] = eyes[1];
    impl_->have_eyes = true;
}

#endif

}  // namespace band3::render::xr
