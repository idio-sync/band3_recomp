#include "lights.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_hidapi.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include "kit_sender.h"
#include "pico_fleet.h"
#include "src/Launcher/launcher_platform.h"
#include "src/Net/native_socket.h"
#include "src/settings.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <xinput.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace band3::lights {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kNoSocket = INVALID_SOCKET;
void CloseSocket(socket_t s) { closesocket(s); }
#else
using socket_t = int;
constexpr socket_t kNoSocket = -1;
void CloseSocket(socket_t s) { close(s); }
#endif

// how often the USB kits are looked for, and the Picos asked for
constexpr auto kScanInterval = 2s;
constexpr auto kDiscoveryInterval = 5s;
// between a kit's commands: a kit sent them faster can drop some (YARG's
// StageKitHardware.cs). Each kit is sent one per pass.
constexpr int64_t kCommandGapNs = 1'000'000;
// a kit whose sends keep failing (found, but not taking reports) is given up
// on after this many, until band3 starts again
constexpr int kMaxFailures = 3;
// the pretend kit's commands kept for the harness
constexpr size_t kMaxFakeCommands = 4096;

// ---------------------------------------------------------------- USB kits

// a Santroller Stage Kit in HID mode
class HidPort final : public KitPort {
public:
    explicit HidPort(SDL_hid_device* handle) : handle_(handle) {}
    ~HidPort() override { SDL_hid_close(handle_); }
    bool Send(Command command) override {
        const auto report = HidReport(command);
        return SDL_hid_write(handle_, report.data(), report.size()) >= 0;
    }

private:
    SDL_hid_device* handle_;
};

#ifdef _WIN32
// XInput's functions, from whichever XInput the PC has
struct XInputApi {
    using GetCapabilities = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
    using SetState = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
    GetCapabilities get_capabilities = nullptr;
    SetState set_state = nullptr;

    static XInputApi Load() {
        XInputApi api;
        for (const wchar_t* name : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
            if (HMODULE module = LoadLibraryW(name)) {
                api.get_capabilities = reinterpret_cast<GetCapabilities>(
                    reinterpret_cast<void*>(GetProcAddress(module, "XInputGetCapabilities")));
                api.set_state = reinterpret_cast<SetState>(
                    reinterpret_cast<void*>(GetProcAddress(module, "XInputSetState")));
                if (api.get_capabilities && api.set_state) return api;
                api = {};
            }
        }
        return api;
    }
    explicit operator bool() const { return get_capabilities && set_state; }
};

// an Xbox 360 Stage Kit, or a Santroller one set to XInput on Windows
class XInputPort final : public KitPort {
public:
    XInputPort(const XInputApi& api, DWORD slot) : api_(api), slot_(slot) {}
    bool Send(Command command) override {
        const Rumble rumble = XInputRumble(command);
        XINPUT_VIBRATION vibration{rumble.left, rumble.right};
        return api_.set_state(slot_, &vibration) == ERROR_SUCCESS;
    }

private:
    XInputApi api_;
    DWORD slot_;
};
#endif

struct Queued {
    Command command;
    bool game = false;
    std::optional<std::string> usb_key;  // a test command's kit; every kit if none
};

struct Usb {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Queued> queue;
    bool stop = false;
    std::vector<KitInfo> kits;
    std::deque<Command> fake_sent;
    std::thread thread;
};
Usb g_usb;

// the pretend kit: keeps what it's sent for TakeFakeCommands
class FakePort final : public KitPort {
public:
    bool Send(Command command) override {
        std::lock_guard<std::mutex> lock(g_usb.mutex);
        g_usb.fake_sent.push_back(command);
        if (g_usb.fake_sent.size() > kMaxFakeCommands) g_usb.fake_sent.pop_front();
        return true;
    }
};

std::string Utf8(const wchar_t* text) {
    std::string out;
    if (!text) return out;
    for (; *text; text++) out += *text < 0x80 ? static_cast<char>(*text) : '?';
    return out;
}

class UsbKits {
public:
    void Run() {
        hid_ok_ = SDL_hid_init() == 0;
        if (!hid_ok_) REXLOG_WARN("Lights: no HID ({}), so no Santroller Stage Kits", SDL_GetError());
#ifdef _WIN32
        xinput_ = XInputApi::Load();
        if (!xinput_) REXLOG_WARN("Lights: no XInput, so no Xbox 360 Stage Kits");
#endif
        bool pending = false;
        for (;;) {
            std::deque<Queued> queue;
            bool stop = false;
            {
                std::unique_lock<std::mutex> lock(g_usb.mutex);
                if (!pending) {
                    g_usb.cv.wait_for(lock, 250ms,
                                      [] { return g_usb.stop || !g_usb.queue.empty(); });
                }
                queue.swap(g_usb.queue);
                stop = g_usb.stop;
            }
            for (const Queued& q : queue) {
                if (q.game) sender_.Game(q.command);
                else sender_.Test(q.command, q.usb_key);
            }

            const bool want = !stop && REXCVAR_GET(stagekit_usb);
            if (enabled_ && !want) Disable();
            if (stop) break;
            if (want && !enabled_) {
                enabled_ = true;
                next_scan_ = {};
            }
            if (enabled_ && Clock::now() >= next_scan_) {
                Scan();
                next_scan_ = Clock::now() + kScanInterval;
            }
            pending = enabled_ && sender_.Pump();
            for (const KitInfo& lost : sender_.TakeLost()) {
                const int failures = ++failures_[lost.key];
                REXLOG_INFO("Lights: lost {}{}", lost.name,
                            failures >= kMaxFailures ? "; it keeps failing, so it's left alone"
                                                     : "");
                Publish();
            }
            if (pending) pacing::SleepFor(kCommandGapNs);
        }
        sender_ = {};
        if (hid_ok_) SDL_hid_exit();
    }

private:
    // every kit's lights off, then the kits let go
    void Disable() {
        sender_.Test(kAllOffCommand, std::nullopt);
        for (int i = 0; i < 16 && sender_.Pump(); i++) pacing::SleepFor(kCommandGapNs);
        for (const KitInfo& kit : sender_.Kits()) sender_.Remove(kit.key);
        enabled_ = false;
        Publish();
    }

    bool Wanted(const std::string& key) const {
        const auto it = failures_.find(key);
        return it == failures_.end() || it->second < kMaxFailures;
    }

    void Add(KitInfo info, std::unique_ptr<KitPort> port) {
        REXLOG_INFO("Lights: found {}", info.name);
        sender_.Add(std::move(info), std::move(port));
    }

    void Scan() {
        std::set<std::string> present;
        if (hid_ok_) {
            SDL_hid_device_info* list = SDL_hid_enumerate(kSantrollerVendor, kSantrollerProduct);
            for (SDL_hid_device_info* info = list; info; info = info->next) {
                if (!info->path ||
                    !IsSantrollerStageKit(info->vendor_id, info->product_id, info->release_number)) {
                    continue;
                }
                // its game controller (Generic Desktop), where the platform says
                if (info->usage_page != 0 && info->usage_page != 0x01) continue;
                const std::string key = std::string("hid:") + info->path;
                present.insert(key);
                if (sender_.Has(key) || !Wanted(key)) continue;
                SDL_hid_device* handle = SDL_hid_open_path(info->path);
                if (!handle) {
                    if (open_failed_.insert(key).second) {
                        REXLOG_WARN("Lights: can't open the Santroller Stage Kit at {}: {}",
                                    info->path, SDL_GetError());
                    }
                    continue;
                }
                std::string name = Utf8(info->product_string);
                if (name.empty()) name = "Santroller Stage Kit";
                Add({key, name + " (HID)", KitKind::kHid}, std::make_unique<HidPort>(handle));
            }
            SDL_hid_free_enumeration(list);
        }
#ifdef _WIN32
        if (xinput_) {
            for (DWORD slot = 0; slot < XUSER_MAX_COUNT; slot++) {
                XINPUT_CAPABILITIES caps{};
                if (xinput_.get_capabilities(slot, 0, &caps) != ERROR_SUCCESS ||
                    caps.SubType != kSubtypeStageKit) {
                    continue;
                }
                const std::string key = "xinput:" + std::to_string(slot);
                present.insert(key);
                if (sender_.Has(key) || !Wanted(key)) continue;
                Add({key, "Stage Kit (XInput, slot " + std::to_string(slot + 1) + ")",
                     KitKind::kXInput},
                    std::make_unique<XInputPort>(xinput_, slot));
            }
        }
#endif
        if (REXCVAR_GET(stagekit_fake)) {
            present.insert("fake");
            if (!sender_.Has("fake")) {
                Add({"fake", "Pretend Stage Kit (stagekit_fake)", KitKind::kFake},
                    std::make_unique<FakePort>());
            }
        }
        for (const KitInfo& kit : sender_.Kits()) {
            if (present.contains(kit.key)) continue;
            REXLOG_INFO("Lights: {} unplugged", kit.name);
            sender_.Remove(kit.key);
        }
        Publish();
    }

    void Publish() {
        auto kits = sender_.Kits();
        std::lock_guard<std::mutex> lock(g_usb.mutex);
        g_usb.kits = std::move(kits);
    }

    KitSender sender_;
    bool enabled_ = false;
    bool hid_ok_ = false;
    Clock::time_point next_scan_{};
    std::map<std::string, int> failures_;
    std::set<std::string> open_failed_;
#ifdef _WIN32
    XInputApi xinput_;
#endif
};

void QueueUsb(Queued queued) {
    {
        std::lock_guard<std::mutex> lock(g_usb.mutex);
        if (!g_usb.thread.joinable() || g_usb.stop) return;
        g_usb.queue.push_back(std::move(queued));
    }
    g_usb.cv.notify_one();
}

// ---------------------------------------------------------------- Picos

struct PicoCommand {
    Command command;
    Target target;
};

struct Picos {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<PicoCommand> queue;
    bool stop = false;
    std::vector<PicoFleet::Pico> list;
    std::string problem;
    std::thread thread;
};
Picos g_picos;

uint32_t Broadcast() { return INADDR_BROADCAST; }

// this PC's subnet's broadcast address, guessed as a /24 from the address the
// default route leaves from, as the dashboard does; 0 if there's none
uint32_t SubnetBroadcast() {
    socket_t probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe == kNoSocket) return 0;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(80);
    inet_pton(AF_INET, "8.8.8.8", &to.sin_addr);
    uint32_t address = 0;
    if (connect(probe, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
        sockaddr_in local{};
        socklen_t size = sizeof(local);
        if (getsockname(probe, reinterpret_cast<sockaddr*>(&local), &size) == 0) {
            address = local.sin_addr.s_addr | htonl(0xFF);
        }
    }
    CloseSocket(probe);
    return address;
}

socket_t OpenUdp() {
    socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) return s;
    int broadcast = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast),
               sizeof(broadcast));
    return s;
}

class PicoLink {
public:
    void Run() {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            SetProblem("Couldn't start Windows' networking, so no wireless Stage Kits.");
            return;
        }
#endif
        command_ = OpenUdp();
        for (;;) {
            std::deque<PicoCommand> queue;
            bool stop = false;
            {
                std::unique_lock<std::mutex> lock(g_picos.mutex);
                // with no socket to wait on, wait here
                if (telemetry_ == kNoSocket) {
                    g_picos.cv.wait_for(lock, 100ms,
                                        [] { return g_picos.stop || !g_picos.queue.empty(); });
                }
                queue.swap(g_picos.queue);
                stop = g_picos.stop;
            }
            for (const PicoCommand& q : queue) Send(q.command, q.target);
            if (stop) break;

            const auto now = Clock::now();
            const bool want = REXCVAR_GET(pico_discovery);
            if (want && telemetry_ == kNoSocket && now >= next_bind_) Listen(now);
            if (!want && telemetry_ != kNoSocket) {
                CloseSocket(telemetry_);
                telemetry_ = kNoSocket;
                fleet_ = {};
            }
            if (!want) SetProblem("");
            if (telemetry_ != kNoSocket) {
                if (now >= next_discovery_) {
                    Discover();
                    next_discovery_ = now + kDiscoveryInterval;
                }
                Receive();
            }
            auto list = fleet_.List(Clock::now());
            std::lock_guard<std::mutex> lock(g_picos.mutex);
            g_picos.list = std::move(list);
        }
        // the lights a test left on go dark
        if (seen_any_) Send(kAllOffCommand, Target{});
        if (telemetry_ != kNoSocket) CloseSocket(telemetry_);
        if (command_ != kNoSocket) CloseSocket(command_);
#ifdef _WIN32
        WSACleanup();
#endif
    }

private:
    void SetProblem(std::string problem) {
        std::lock_guard<std::mutex> lock(g_picos.mutex);
        g_picos.problem = std::move(problem);
    }

    void Listen(Clock::time_point now) {
        next_bind_ = now + kDiscoveryInterval;
        socket_t s = OpenUdp();
        if (s == kNoSocket) {
            SetProblem("Couldn't open a network socket for the wireless Stage Kits.");
            return;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(kTelemetryPort);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
            const uint32_t error = net::LastSocketError();
            CloseSocket(s);
            if (!bind_warned_) {
                REXLOG_WARN("Lights: can't listen on UDP port {} (error {}), so no wireless "
                            "Stage Kits until it's free",
                            kTelemetryPort, error);
                bind_warned_ = true;
            }
            SetProblem("Port 21071 is in use, so wireless Stage Kits can't be found. Is the RB3E "
                       "Dashboard running?");
            return;
        }
        telemetry_ = s;
        bind_warned_ = false;
        next_discovery_ = now;
        SetProblem("");
        REXLOG_INFO("Lights: looking for wireless Stage Kits on UDP port {}", kTelemetryPort);
    }

    void Discover() {
        const auto* packet = reinterpret_cast<const uint8_t*>(kDiscoveryPacket.data());
        net::SendTo(telemetry_, packet, kDiscoveryPacket.size(), Broadcast(), htons(kTelemetryPort));
        if (const uint32_t subnet = SubnetBroadcast()) {
            net::SendTo(telemetry_, packet, kDiscoveryPacket.size(), subnet, htons(kTelemetryPort));
        }
    }

    // takes the telemetry waiting, waiting up to 100 ms for the first
    void Receive() {
        uint8_t buffer[1024];
        int wait_ms = 100;
        for (int i = 0; i < 32; i++) {
            uint32_t address = 0;
            uint16_t port = 0;
            const int size = net::ReceiveFrom(telemetry_, buffer, sizeof(buffer), address, port,
                                              wait_ms);
            if (size < 0) return;
            wait_ms = 0;
            const auto status = ParseTelemetry(
                std::string_view(reinterpret_cast<const char*>(buffer), static_cast<size_t>(size)));
            if (!status) continue;
            if (known_.insert(address).second) {
                REXLOG_INFO("Lights: found the wireless Stage Kit {} at {} (kit {}, {} dBm)",
                            status->name, FormatAddress(address), status->usb_status,
                            status->wifi_signal);
            }
            seen_any_ = true;
            fleet_.Heard(address, *status, Clock::now());
        }
    }

    void Send(Command command, const Target& target) {
        if (command_ == kNoSocket) return;
        const auto packet = StageKitPacket(command);
        const auto to = [&](uint32_t address) {
            net::SendTo(command_, packet.data(), packet.size(), address, htons(kRb3ePort));
        };
        if (target.kind == Target::Kind::kPico) {
            to(target.address);
            return;
        }
        // every Pico: each one listed, or the network's broadcast if none is
        const auto picos = fleet_.List(Clock::now());
        if (!picos.empty()) {
            for (const auto& pico : picos) to(pico.address);
            return;
        }
        to(Broadcast());
        if (const uint32_t subnet = SubnetBroadcast()) to(subnet);
    }

    socket_t command_ = kNoSocket;
    socket_t telemetry_ = kNoSocket;
    PicoFleet fleet_;
    std::set<uint32_t> known_;
    bool seen_any_ = false;
    bool bind_warned_ = false;
    Clock::time_point next_bind_{};
    Clock::time_point next_discovery_{};
};

void QueuePico(Command command, Target target) {
    {
        std::lock_guard<std::mutex> lock(g_picos.mutex);
        if (!g_picos.thread.joinable() || g_picos.stop) return;
        g_picos.queue.push_back({command, std::move(target)});
    }
    g_picos.cv.notify_one();
}

// ---------------------------------------------------------------- scenes

struct Scene {
    std::mutex mutex;
    std::condition_variable cv;
    bool cancel = false;
    std::thread thread;
};
Scene g_scene;

void StopScene() {
    {
        std::lock_guard<std::mutex> lock(g_scene.mutex);
        g_scene.cancel = true;
    }
    g_scene.cv.notify_all();
    if (g_scene.thread.joinable()) g_scene.thread.join();
    g_scene.cancel = false;
}

std::mutex g_lifecycle_mutex;
bool g_started = false;

std::mutex g_last_mutex;
std::string g_last_sent;

}  // namespace

void Start() {
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
    if (g_started) return;
    g_started = true;
    g_usb.thread = std::thread([] { UsbKits().Run(); });
    g_picos.thread = std::thread([] { PicoLink().Run(); });
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
    if (!g_started) return;
    StopScene();
    {
        std::lock_guard<std::mutex> usb(g_usb.mutex);
        g_usb.stop = true;
    }
    g_usb.cv.notify_one();
    {
        std::lock_guard<std::mutex> picos(g_picos.mutex);
        g_picos.stop = true;
    }
    g_picos.cv.notify_one();
    if (g_usb.thread.joinable()) g_usb.thread.join();
    if (g_picos.thread.joinable()) g_picos.thread.join();
    g_started = false;
}

void NoteGame(uint8_t left, uint8_t right) { QueueUsb({{left, right}, true, std::nullopt}); }

std::vector<DeviceRow> Devices() {
    std::vector<KitInfo> kits;
    {
        std::lock_guard<std::mutex> lock(g_usb.mutex);
        kits = g_usb.kits;
    }
    std::vector<PicoFleet::Pico> picos;
    {
        std::lock_guard<std::mutex> lock(g_picos.mutex);
        picos = g_picos.list;
    }
    return DeviceRows(kits, picos);
}

bool PicosNeedEvents() {
    std::lock_guard<std::mutex> lock(g_picos.mutex);
    return lights::PicosNeedEvents(g_picos.list, REXCVAR_GET(events_enabled));
}

std::string PicoProblem() {
    std::lock_guard<std::mutex> lock(g_picos.mutex);
    return g_picos.problem;
}

void SendTest(Command command, const std::optional<std::string>& key) {
    const Target target = ParseTarget(key);
    if (target.kind != Target::Kind::kPico) {
        QueueUsb({command, false,
                  target.kind == Target::Kind::kUsb ? std::optional(target.usb_key)
                                                    : std::nullopt});
    }
    if (target.kind != Target::Kind::kUsb) QueuePico(command, target);

    std::string where = "all devices";
    if (target.kind != Target::Kind::kAll) {
        for (const DeviceRow& row : Devices()) {
            if (row.key == *key) where = row.name;
        }
    }
    std::lock_guard<std::mutex> lock(g_last_mutex);
    g_last_sent = DescribeCommand(command) + " to " + where;
}

void PlayScene(std::span<const Step> steps, const std::optional<std::string>& key) {
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
    if (!g_started) return;
    StopScene();
    g_scene.thread = std::thread([steps, key] {
        for (const Step& step : steps) {
            SendTest(step.command, key);
            std::unique_lock<std::mutex> wait(g_scene.mutex);
            if (g_scene.cv.wait_for(wait, std::chrono::milliseconds(step.delay_ms),
                                    [] { return g_scene.cancel; })) {
                return;
            }
        }
    });
}

std::string LastSent() {
    std::lock_guard<std::mutex> lock(g_last_mutex);
    return g_last_sent;
}

std::vector<Command> TakeFakeCommands() {
    std::lock_guard<std::mutex> lock(g_usb.mutex);
    std::vector<Command> sent(g_usb.fake_sent.begin(), g_usb.fake_sent.end());
    g_usb.fake_sent.clear();
    return sent;
}

}  // namespace band3::lights
