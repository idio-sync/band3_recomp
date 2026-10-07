#include "gameinput_instruments.h"
#include <rex/logging.h>

#ifdef BAND3_HAVE_GAMEINPUT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GameInput.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include "src/Launcher/launcher_platform.h"

namespace band3::input {

namespace {

using Microsoft::WRL::ComPtr;
using namespace GameInput::v3;

// how long a source sleeps when GameInput has no new reading for it
constexpr int64_t kPollNs = 1'000'000;

std::string PathOf(const GameInputDeviceInfo& info) {
    std::string path = "gameinput:";
    char hex[3];
    for (const uint8_t byte : info.deviceId.value) {
        std::snprintf(hex, sizeof(hex), "%02x", byte);
        path += hex;
    }
    return path;
}

// One device's raw reports, walked reading by reading from GameInput's history
// so that a drum hit shorter than a poll isn't missed.
class GameInputSource final : public ReportSource {
public:
    GameInputSource(ComPtr<IGameInput> input, ComPtr<IGameInputDevice> device)
        : input_(std::move(input)), device_(std::move(device)) {}

    int Read(uint8_t* buffer, size_t size, int timeout_ms) override {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            ComPtr<IGameInputReading> reading;
            HRESULT hr = last_reading_
                             ? input_->GetNextReading(last_reading_.Get(),
                                                      GameInputKindRawDeviceReport,
                                                      device_.Get(), &reading)
                             : E_FAIL;
            // the first read, or one that fell behind GameInput's history,
            // starts again from the latest reading
            if (!last_reading_ || hr == GAMEINPUT_E_REFERENCE_READING_TOO_OLD) {
                hr = input_->GetCurrentReading(GameInputKindRawDeviceReport, device_.Get(),
                                               &reading);
            }
            if (SUCCEEDED(hr)) {
                last_reading_ = reading;
                const int length = Take(*reading.Get(), buffer, size);
                if (length > 0) return length;
                continue;  // the same report again: on to the next reading
            }
            if (hr != GAMEINPUT_E_READING_NOT_FOUND && !Connected()) return -1;
            if (std::chrono::steady_clock::now() >= deadline) return Connected() ? 0 : -1;
            pacing::SleepFor(kPollNs);
        }
    }

private:
    bool Connected() { return (device_->GetDeviceStatus() & GameInputDeviceConnected) != 0; }

    // the reading's report as its ID and data, if it isn't the last one read
    int Take(IGameInputReading& reading, uint8_t* buffer, size_t size) {
        ComPtr<IGameInputRawDeviceReport> raw;
        if (!reading.GetRawReport(&raw) || !raw) return 0;
        GameInputRawDeviceReportInfo info{};
        raw->GetReportInfo(&info);
        report_.resize(1 + raw->GetRawDataSize());
        report_[0] = static_cast<uint8_t>(info.id);
        report_.resize(1 + raw->GetRawData(report_.size() - 1, report_.data() + 1));
        if (report_ == last_report_) return 0;
        last_report_ = report_;
        const size_t length = std::min(size, report_.size());
        std::memcpy(buffer, report_.data(), length);
        return static_cast<int>(length);
    }

    const ComPtr<IGameInput> input_;
    const ComPtr<IGameInputDevice> device_;
    ComPtr<IGameInputReading> last_reading_;
    std::vector<uint8_t> report_;
    std::vector<uint8_t> last_report_;
};

class GameInputWatch final : public GipWatch {
public:
    explicit GameInputWatch(ComPtr<IGameInput> input) : input_(std::move(input)) {
        for (const auto& known : KnownGipInstruments()) maker_vendors_.insert(known.vendor);
    }

    ~GameInputWatch() override {
        if (token_) {
            input_->StopCallback(token_);
            input_->UnregisterCallback(token_);
        }
    }

    bool Register() {
        const HRESULT hr = input_->RegisterDeviceCallback(
            nullptr, GameInputKindRawDeviceReport, GameInputDeviceConnected,
            GameInputAsyncEnumeration, this, &OnDevice, &token_);
        if (FAILED(hr)) {
            REXLOG_WARN("Xbox One instruments: GameInput won't say which devices connect "
                        "({:08X}), so they won't be read", static_cast<uint32_t>(hr));
            return false;
        }
        return true;
    }

    std::vector<GipInstrument> TakeArrivals() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::move(arrivals_);
    }

private:
    static void CALLBACK OnDevice(GameInputCallbackToken, void* context, IGameInputDevice* device,
                                  uint64_t, GameInputDeviceStatus current,
                                  GameInputDeviceStatus previous) {
        // connections only: a source notices its own device going
        if (!(current & GameInputDeviceConnected) || (previous & GameInputDeviceConnected)) return;
        static_cast<GameInputWatch*>(context)->Arrived(device);
    }

    // on GameInput's thread
    void Arrived(IGameInputDevice* device) {
        const GameInputDeviceInfo* info = nullptr;
        if (FAILED(device->GetDeviceInfo(&info)) || !info) return;
        if (info->deviceFamily != GameInputFamilyXboxOne ||
            !(info->supportedInput & GameInputKindRawDeviceReport)) {
            return;
        }
        const auto instrument = IdentifyGipInstrument(info->vendorId, info->productId);
        if (!instrument) {
            // from a maker of instruments, so perhaps one band3 doesn't know yet
            // (the RB4 wireless legacy adapter is one)
            if (maker_vendors_.contains(info->vendorId)) {
                REXLOG_INFO("Xbox One instruments: {:04X}:{:04X} ({}) isn't one band3 reads",
                            info->vendorId, info->productId,
                            info->displayName ? info->displayName : "no name");
            }
            return;
        }

        GipInstrument found;
        found.path = PathOf(*info);
        found.vendor = info->vendorId;
        found.product = info->productId;
        found.release = info->revisionNumber;
        found.instrument = *instrument;
        found.source = std::make_unique<GameInputSource>(input_, ComPtr<IGameInputDevice>(device));
        std::lock_guard<std::mutex> lock(mutex_);
        arrivals_.push_back(std::move(found));
    }

    const ComPtr<IGameInput> input_;
    GameInputCallbackToken token_ = 0;
    std::set<uint16_t> maker_vendors_;

    std::mutex mutex_;
    std::vector<GipInstrument> arrivals_;
};

}

std::unique_ptr<GipWatch> StartGipWatch() {
    ComPtr<IGameInput> input;
    const HRESULT hr = GameInputCreate(&input);
    if (FAILED(hr)) {
        REXLOG_INFO("Xbox One instruments: GameInput 3 isn't installed ({:08X}), so they won't "
                    "be read; Microsoft's GameInput redistributable adds it",
                    static_cast<uint32_t>(hr));
        return nullptr;
    }
    // read while another window has the focus, as the HID instruments are
    input->SetFocusPolicy(GameInputEnableBackgroundInput);
    auto watch = std::make_unique<GameInputWatch>(std::move(input));
    if (!watch->Register()) return nullptr;
    REXLOG_INFO("Xbox One instruments: looking for them through GameInput");
    return watch;
}

}

#else

namespace band3::input {

std::unique_ptr<GipWatch> StartGipWatch() {
#ifdef _WIN32
    REXLOG_INFO("Xbox One instruments: this build was made without GameInput, so they won't "
                "be read");
#endif
    return nullptr;
}

}

#endif
