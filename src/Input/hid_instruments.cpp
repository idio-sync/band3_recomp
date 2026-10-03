#include "hid_instruments.h"
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_guid.h>
#include <SDL3/SDL_hidapi.h>
#include <SDL3/SDL_joystick.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include "hid_capture.h"
#include "hid_instrument_types.h"
#include "xinput_state.h"

namespace band3::input {

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::DeviceId;
using rex::input::DeviceInfo;

namespace {

// clear of the SDK drivers' ids (SDL counts up from 1, MnK and NOP use
// 0x4D4E4B00 and 0x4E4F5000) and the virtual instrument's (0x42335649...)
constexpr uint64_t kDeviceIdBase = 0x4233484900000000ull;  // "B3HI"
constexpr const char* kGuidPrefix = "band3-hid:";
// how often to look for newly plugged dongles
constexpr std::chrono::milliseconds kScanInterval{1000};
// a reader wakes at least this often to notice shutdown
constexpr int kReadTimeoutMs = 100;
// the vendors of every known instrument, to enumerate by
std::set<uint16_t> KnownVendors() {
    std::set<uint16_t> vendors;
    for (const auto& known : KnownHidInstruments()) vendors.insert(known.vendor);
    return vendors;
}

// set while a HID driver is running, for IsSdlCopyOfHidInstrument
std::atomic<bool> g_active{false};
// counts across drivers, since the launcher restarts the driver inside one
// input system, which never takes an id back
std::atomic<uint64_t> g_generation{0};

std::mutex g_capture_message_mutex;
std::string g_capture_message;

void SetCaptureMessage(std::string message) {
    std::lock_guard<std::mutex> lock(g_capture_message_mutex);
    g_capture_message = std::move(message);
}

// logs/hid-capture-<vendor><product>-<local time>.txt next to the executable
void SaveCapture(const HidCapture& capture) {
    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    char name[80];
    std::snprintf(name, sizeof(name), "hid-capture-%04x%04x-%s.txt", capture.vendor,
                  capture.product, stamp);

    std::error_code ec;
    const auto dir = rex::filesystem::GetExecutableFolder() / "logs";
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / name;
    std::ofstream file(path, std::ios::binary);
    file << FormatHidCapture(capture);
    if (!file) {
        SetCaptureMessage("Couldn't write " + path.string());
        REXLOG_WARN("HID instruments: couldn't write capture {}", path.string());
        return;
    }
    SetCaptureMessage("Saved " + std::to_string(capture.reports.size()) + " reports to " +
                      path.string());
    REXLOG_INFO("HID instruments: saved {} reports from {} to {}", capture.reports.size(),
                capture.device, path.string());
}

const char* NameOf(uint16_t vendor, uint16_t product) {
    for (const auto& known : KnownHidInstruments()) {
        if (known.vendor == vendor && known.product == product) return known.name;
    }
    return "Rock Band instrument";
}

struct Device {
    Device(DeviceId id, HidInstrumentType instrument, uint16_t vendor, uint16_t product,
           uint16_t release, std::string path, std::string name, SDL_hid_device* handle)
        : id(id), instrument(instrument), vendor(vendor), product(product), release(release),
          path(std::move(path)), name(std::move(name)), handle(handle), translator(instrument) {}

    const DeviceId id;
    const HidInstrumentType instrument;
    const uint16_t vendor;
    const uint16_t product;
    const uint16_t release;
    const std::string path;
    const std::string name;
    SDL_hid_device* const handle;
    std::atomic<bool> connected{true};
    std::thread reader;

    // the reader thread writes these, guest threads and the Lab read them
    std::mutex mutex;
    Gamepad360 state{};
    uint32_t packet_number = 0;
    uint64_t report_count = 0;
    std::vector<uint8_t> last_report;
    // a capture the Lab asked for, recorded by the reader
    bool capturing = false;
    std::chrono::steady_clock::time_point capture_start;
    std::chrono::steady_clock::time_point capture_end;
    HidCapture capture;

    // reader thread only
    HidInstrumentTranslator translator;
};

class HidInstrumentDriver final : public rex::input::InputDriver {
public:
    HidInstrumentDriver() : InputDriver(nullptr, 0) {}
    ~HidInstrumentDriver() override { Stop(); }

    X_STATUS Setup() override {
        if (SDL_hid_init() != 0) {
            REXLOG_WARN("HID instruments: SDL's HID API didn't start ({}), so PS3 and Wii "
                        "instruments won't be read", SDL_GetError());
            return X_STATUS_UNSUCCESSFUL;
        }
        hid_started_ = true;
        g_active = true;
        scanner_ = std::thread([this] { ScanLoop(); });
        {
            std::lock_guard<std::mutex> lock(driver_mutex());
            driver() = this;
        }
        REXLOG_INFO("HID instruments: looking for PS3 and Wii Rock Band instruments");
        return X_STATUS_SUCCESS;
    }

    void EnumerateDevices(std::vector<DeviceInfo>& out) override {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        for (const auto& device : devices_) {
            if (!device->connected) continue;
            DeviceInfo info;
            info.id = device->id;
            info.name = device->name;
            info.guid = kGuidPrefix + device->path;
            info.synthetic = false;
            out.push_back(std::move(info));
        }
    }

    X_RESULT GetDeviceState(DeviceId id, rex::input::X_INPUT_STATE* out_state) override {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        Device* device = Find(id);
        if (!device) return X_ERROR_DEVICE_NOT_CONNECTED;
        if (out_state) {
            std::lock_guard<std::mutex> state_lock(device->mutex);
            out_state->packet_number = device->packet_number;
            StoreGamepad(device->state, out_state->gamepad);
        }
        return X_ERROR_SUCCESS;
    }

    X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t,
                                   rex::input::X_INPUT_CAPABILITIES* out_caps) override {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        Device* device = Find(id);
        if (!device) return X_ERROR_DEVICE_NOT_CONNECTED;
        if (out_caps) StoreCaps(HidInstrumentCaps(device->instrument), *out_caps);
        return X_ERROR_SUCCESS;
    }

    // these instruments have no rumble
    X_RESULT SetDeviceVibration(DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        return Find(id) ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
    }

    X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        return Find(id) ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
    }

    // the one running driver, for the Lab functions below
    static std::mutex& driver_mutex() {
        static std::mutex mutex;
        return mutex;
    }
    static HidInstrumentDriver*& driver() {
        static HidInstrumentDriver* running = nullptr;
        return running;
    }

    std::vector<HidInstrumentStatus> Statuses() {
        std::vector<HidInstrumentStatus> out;
        std::lock_guard<std::mutex> lock(devices_mutex_);
        for (const auto& device : devices_) {
            if (!device->connected) continue;
            HidInstrumentStatus status;
            status.id = static_cast<uint64_t>(device->id);
            status.name = device->name;
            status.vendor = device->vendor;
            status.product = device->product;
            status.release = device->release;
            status.instrument = device->instrument;
            std::lock_guard<std::mutex> state_lock(device->mutex);
            status.report_count = device->report_count;
            status.last_report = device->last_report;
            status.state = device->state;
            status.capturing = device->capturing;
            out.push_back(std::move(status));
        }
        return out;
    }

    bool StartCapture(uint64_t id, std::chrono::seconds length) {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        Device* device = Find(static_cast<DeviceId>(id));
        if (!device) return false;
        std::lock_guard<std::mutex> state_lock(device->mutex);
        if (device->capturing) return false;
        device->capturing = true;
        device->capture_start = std::chrono::steady_clock::now();
        device->capture_end = device->capture_start + length;
        device->capture = HidCapture{};
        device->capture.device = device->name;
        device->capture.vendor = device->vendor;
        device->capture.product = device->product;
        device->capture.release = device->release;
        SetCaptureMessage("Capturing " + device->name + "...");
        return true;
    }

private:
    // with devices_mutex_ held
    Device* Find(DeviceId id) {
        for (const auto& device : devices_) {
            if (device->id == id && device->connected) return device.get();
        }
        return nullptr;
    }

    void ScanLoop() {
        uint32_t last_change = 0;
        bool first = true;
        std::unique_lock<std::mutex> lock(stop_mutex_);
        while (!stop_) {
            lock.unlock();
            // a count of 0 means the platform can't tell, so look every time
            const uint32_t change = SDL_hid_device_change_count();
            if (first || change == 0 || change != last_change) {
                first = false;
                last_change = change;
                Scan();
            }
            Reap();
            lock.lock();
            stop_cv_.wait_for(lock, kScanInterval, [this] { return stop_.load(); });
        }
    }

    // opens every known instrument that isn't open yet
    void Scan() {
        static const std::set<uint16_t> vendors = KnownVendors();
        for (uint16_t vendor : vendors) {
            SDL_hid_device_info* list = SDL_hid_enumerate(vendor, 0);
            for (SDL_hid_device_info* info = list; info; info = info->next) {
                if (!info->path) continue;
                const auto instrument =
                    IdentifyHidInstrument(info->vendor_id, info->product_id, info->release_number);
                if (!instrument || IsOpen(info->path)) continue;

                SDL_hid_device* handle = SDL_hid_open_path(info->path);
                if (!handle) {
                    // most likely permissions (a udev rule on Linux); say so once
                    if (open_failed_.insert(info->path).second) {
                        REXLOG_WARN("HID instruments: can't open {} ({:04X}:{:04X}): {}",
                                    NameOf(info->vendor_id, info->product_id), info->vendor_id,
                                    info->product_id, SDL_GetError());
                    }
                    continue;
                }
                Open(*instrument, info->vendor_id, info->product_id, info->release_number,
                     info->path, handle);
            }
            SDL_hid_free_enumeration(list);
        }
    }

    bool IsOpen(const char* path) {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        for (const auto& device : devices_) {
            if (device->path == path) return true;
        }
        return false;
    }

    void Open(HidInstrumentType instrument, uint16_t vendor, uint16_t product, uint16_t release,
              const char* path, SDL_hid_device* handle) {
        auto device = std::make_unique<Device>(static_cast<DeviceId>(kDeviceIdBase + ++g_generation),
                                               instrument, vendor, product, release, path,
                                               NameOf(vendor, product), handle);
        REXLOG_INFO("HID instruments: {} connected ({:04X}:{:04X})", device->name, vendor, product);
        Device* raw = device.get();
        raw->reader = std::thread([this, raw] { ReadLoop(*raw); });
        std::lock_guard<std::mutex> lock(devices_mutex_);
        devices_.push_back(std::move(device));
    }

    void ReadLoop(Device& device) {
        uint8_t report[64];
        while (!stop_ && device.connected) {
            const int size = SDL_hid_read_timeout(device.handle, report, sizeof(report),
                                                  kReadTimeoutMs);
            if (size < 0) {
                REXLOG_INFO("HID instruments: {} disconnected", device.name);
                device.connected = false;
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            std::optional<HidCapture> finished;
            {
                std::lock_guard<std::mutex> lock(device.mutex);
                if (size > 0) {
                    device.report_count++;
                    device.last_report.assign(report, report + size);
                    if (device.capturing) {
                        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - device.capture_start);
                        device.capture.reports.push_back(
                            {static_cast<uint32_t>(ms.count()), device.last_report});
                    }
                }
                if (device.capturing && now >= device.capture_end) {
                    device.capturing = false;
                    finished = std::move(device.capture);
                }
            }
            if (finished) SaveCapture(*finished);
            if (size == 0) continue;

            const auto state =
                device.translator.Translate({report, static_cast<size_t>(size)});
            if (!state) continue;
            std::lock_guard<std::mutex> lock(device.mutex);
            if (std::memcmp(&*state, &device.state, sizeof(device.state)) != 0) {
                device.state = *state;
                device.packet_number++;
            }
        }

        // keep what a capture got before the instrument went away
        std::optional<HidCapture> partial;
        {
            std::lock_guard<std::mutex> lock(device.mutex);
            if (device.capturing) {
                device.capturing = false;
                partial = std::move(device.capture);
            }
        }
        if (partial) SaveCapture(*partial);
    }

    // closes the devices whose readers stopped
    void Reap() {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        std::erase_if(devices_, [](const std::unique_ptr<Device>& device) {
            if (device->connected) return false;
            if (device->reader.joinable()) device->reader.join();
            SDL_hid_close(device->handle);
            return true;
        });
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(driver_mutex());
            if (driver() == this) driver() = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(stop_mutex_);
            if (stop_) return;
            stop_ = true;
        }
        stop_cv_.notify_all();
        if (scanner_.joinable()) scanner_.join();
        {
            std::lock_guard<std::mutex> lock(devices_mutex_);
            for (const auto& device : devices_) {
                if (device->reader.joinable()) device->reader.join();
                SDL_hid_close(device->handle);
            }
            devices_.clear();
        }
        if (hid_started_) {
            SDL_hid_exit();
            g_active = false;
        }
    }

    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    std::atomic<bool> stop_{false};
    std::thread scanner_;
    bool hid_started_ = false;

    std::mutex devices_mutex_;
    std::vector<std::unique_ptr<Device>> devices_;

    // scanner thread only
    std::set<std::string> open_failed_;
};

}

std::unique_ptr<rex::input::InputDriver> CreateHidInstrumentDriver() {
    return std::make_unique<HidInstrumentDriver>();
}

bool IsHidInstrument(const DeviceInfo& device) {
    return device.guid.starts_with(kGuidPrefix);
}

bool HidInstrumentsActive() { return g_active; }

std::vector<HidInstrumentStatus> HidInstrumentStatuses() {
    std::lock_guard<std::mutex> lock(HidInstrumentDriver::driver_mutex());
    auto* driver = HidInstrumentDriver::driver();
    return driver ? driver->Statuses() : std::vector<HidInstrumentStatus>{};
}

bool StartHidCapture(uint64_t id, std::chrono::seconds length) {
    std::lock_guard<std::mutex> lock(HidInstrumentDriver::driver_mutex());
    auto* driver = HidInstrumentDriver::driver();
    return driver && driver->StartCapture(id, length);
}

std::string LastHidCaptureMessage() {
    std::lock_guard<std::mutex> lock(g_capture_message_mutex);
    return g_capture_message;
}

bool IsSdlCopyOfHidInstrument(const DeviceInfo& device) {
    // an SDL GUID is 32 hex digits; anything else is some other driver's id
    if (!g_active || device.synthetic || device.guid.size() != 32) return false;
    if (device.guid.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;

    Uint16 vendor = 0, product = 0, version = 0, crc = 0;
    SDL_GetJoystickGUIDInfo(SDL_StringToGUID(device.guid.c_str()), &vendor, &product, &version,
                            &crc);
    for (const auto& known : KnownHidInstruments()) {
        if (known.vendor == vendor && known.product == product) return true;
    }
    return false;
}

}
