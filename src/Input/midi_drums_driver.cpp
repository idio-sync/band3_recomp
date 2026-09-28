#include "midi_drums_driver.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <rex/logging.h>
#include "ThirdParty/rtmidi/RtMidi.h"
#include "src/settings.h"
#include "xinput_state.h"

namespace band3::input {

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::DeviceId;
using rex::input::DeviceInfo;

namespace {

// clear of the SDK drivers' ids and band3's others ("B3VI", "B3HI")
constexpr uint64_t kDeviceIdBase = 0x42334D4900000000ull;  // "B3MI"
// how often to look for the port, and notice it going away
constexpr std::chrono::milliseconds kScanInterval{2000};
// how many notes the Lab shows
constexpr size_t kRecentHits = 12;

std::string Lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// With no port named, the first one that isn't the Linux "Midi Through" loopback
bool Matches(const std::string& port, const std::string& wanted) {
    if (wanted.empty()) return Lower(port).find("through") == std::string::npos;
    return Lower(port).find(Lower(wanted)) != std::string::npos;
}

void LogRtMidiError(RtMidiError::Type type, const std::string& text, void*) {
    if (type == RtMidiError::WARNING || type == RtMidiError::DEBUG_WARNING) {
        REXLOG_DEBUG("MIDI drums: {}", text);
    } else {
        REXLOG_WARN("MIDI drums: {}", text);
    }
}

class MidiDrumsDriver final : public rex::input::InputDriver {
public:
    MidiDrumsDriver() : InputDriver(nullptr, 0), kit_(midi_drums::DefaultNoteMap(), {}) {}
    ~MidiDrumsDriver() override { Stop(); }

    X_STATUS Setup() override {
        midi_drums::NoteMap notes = midi_drums::DefaultNoteMap();
        for (const auto& bad : midi_drums::ApplyOverrides(notes, REXCVAR_GET(midi_drums_notes))) {
            REXLOG_WARN("MIDI drums: ignoring the note override \"{}\" (expected note=Part, "
                        "e.g. 44=Kick)", bad);
        }
        midi_drums::Settings settings;
        settings.pulse = std::chrono::milliseconds(REXCVAR_GET(midi_drums_pulse_ms));
        settings.min_velocity = static_cast<uint8_t>(REXCVAR_GET(midi_drums_min_velocity));
        settings.combos = REXCVAR_GET(midi_drums_combos);
        kit_ = midi_drums::Kit(notes, settings);
        min_velocity_ = settings.min_velocity;
        wanted_port_ = REXCVAR_GET(midi_drums_device);

        try {
            input_ = std::make_unique<RtMidiIn>();
            probe_ = std::make_unique<RtMidiIn>();
        } catch (const RtMidiError& e) {
            REXLOG_WARN("MIDI drums: MIDI input isn't available ({})", e.getMessage());
            return X_STATUS_UNSUCCESSFUL;
        }
        input_->setErrorCallback(LogRtMidiError);
        probe_->setErrorCallback(LogRtMidiError);
        // drop sysex, clock and active sensing, which some kits send constantly
        input_->ignoreTypes(true, true, true);
        input_->setCallback(&MidiDrumsDriver::OnMessage, this);

        {
            std::lock_guard<std::mutex> lock(driver_mutex());
            driver() = this;
        }
        watcher_ = std::thread([this] { WatchLoop(); });
        REXLOG_INFO("MIDI drums: looking for {}",
                    wanted_port_.empty() ? std::string("a MIDI input") : "\"" + wanted_port_ + "\"");
        return X_STATUS_SUCCESS;
    }

    void EnumerateDevices(std::vector<DeviceInfo>& out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id_ == DeviceId::kInvalid) return;
        DeviceInfo info;
        info.id = id_;
        info.name = "MIDI drums: " + port_;
        info.guid = "band3-midi:" + port_;
        info.synthetic = false;
        out.push_back(std::move(info));
    }

    X_RESULT GetDeviceState(DeviceId id, rex::input::X_INPUT_STATE* out_state) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id == DeviceId::kInvalid || id != id_) return X_ERROR_DEVICE_NOT_CONNECTED;
        const Gamepad360 g = EncodeDrums(kit_.State(midi_drums::Clock::now()));
        if (std::memcmp(&g, &last_, sizeof(g)) != 0) {
            last_ = g;
            packet_number_++;
        }
        if (out_state) {
            out_state->packet_number = packet_number_;
            StoreGamepad(g, out_state->gamepad);
        }
        return X_ERROR_SUCCESS;
    }

    X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t,
                                   rex::input::X_INPUT_CAPABILITIES* out_caps) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id == DeviceId::kInvalid || id != id_) return X_ERROR_DEVICE_NOT_CONNECTED;
        if (out_caps) StoreCaps(DrumCaps(true), *out_caps);
        return X_ERROR_SUCCESS;
    }

    X_RESULT SetDeviceVibration(DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return id != DeviceId::kInvalid && id == id_ ? X_ERROR_SUCCESS
                                                     : X_ERROR_DEVICE_NOT_CONNECTED;
    }

    X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return id != DeviceId::kInvalid && id == id_ ? X_ERROR_EMPTY
                                                     : X_ERROR_DEVICE_NOT_CONNECTED;
    }

    // the one running driver, for GetMidiDrumsStatus
    static std::mutex& driver_mutex() {
        static std::mutex mutex;
        return mutex;
    }
    static MidiDrumsDriver*& driver() {
        static MidiDrumsDriver* running = nullptr;
        return running;
    }

    MidiDrumsStatus Status() {
        MidiDrumsStatus status;
        status.running = true;
        std::lock_guard<std::mutex> lock(mutex_);
        status.port = id_ != DeviceId::kInvalid ? port_ : std::string();
        status.ports = ports_;
        status.recent.assign(recent_.begin(), recent_.end());
        status.min_velocity = min_velocity_;
        return status;
    }

private:
    // RtMidi's thread, for every message from the open port
    static void OnMessage(double, std::vector<unsigned char>* message, void* user) {
        auto* self = static_cast<MidiDrumsDriver*>(user);
        if (!message) return;
        std::lock_guard<std::mutex> lock(self->mutex_);
        const auto hit = self->kit_.Receive(*message, midi_drums::Clock::now());
        if (!hit) return;
        self->recent_.push_back(*hit);
        if (self->recent_.size() > kRecentHits) self->recent_.pop_front();
    }

    void WatchLoop() {
        std::unique_lock<std::mutex> lock(stop_mutex_);
        while (!stop_) {
            lock.unlock();
            Scan();
            lock.lock();
            stop_cv_.wait_for(lock, kScanInterval, [this] { return stop_.load(); });
        }
    }

    // opens the wanted port when it appears, closes it when it goes
    void Scan() {
        std::vector<std::string> ports;
        const unsigned count = probe_->getPortCount();
        for (unsigned i = 0; i < count; i++) ports.push_back(probe_->getPortName(i));

        bool open;
        std::string open_port;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ports_ = ports;
            open = id_ != DeviceId::kInvalid;
            open_port = port_;
        }

        if (open) {
            if (std::ranges::find(ports, open_port) != ports.end()) return;
            REXLOG_INFO("MIDI drums: {} disconnected", open_port);
            input_->closePort();
            std::lock_guard<std::mutex> lock(mutex_);
            id_ = DeviceId::kInvalid;
            return;
        }

        for (unsigned i = 0; i < ports.size(); i++) {
            if (!Matches(ports[i], wanted_port_)) continue;
            input_->openPort(i, "band3 drums");
            if (!input_->isPortOpen()) continue;
            REXLOG_INFO("MIDI drums: playing {}", ports[i]);
            std::lock_guard<std::mutex> lock(mutex_);
            port_ = ports[i];
            // a new id reads as a new controller
            id_ = static_cast<DeviceId>(kDeviceIdBase + ++generation_);
            return;
        }
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
        if (watcher_.joinable()) watcher_.join();
        if (input_) {
            input_->cancelCallback();
            if (input_->isPortOpen()) input_->closePort();
        }
    }

    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    std::atomic<bool> stop_{false};
    std::thread watcher_;

    // the watcher thread opens and closes input_; probe_ only lists ports
    std::unique_ptr<RtMidiIn> input_;
    std::unique_ptr<RtMidiIn> probe_;
    std::string wanted_port_;
    uint8_t min_velocity_ = 0;

    // shared by RtMidi's thread, guest threads and the Lab
    std::mutex mutex_;
    midi_drums::Kit kit_;
    DeviceId id_ = DeviceId::kInvalid;
    uint64_t generation_ = 0;
    std::string port_;
    std::vector<std::string> ports_;
    std::deque<midi_drums::Hit> recent_;
    Gamepad360 last_{};
    uint32_t packet_number_ = 0;
};

}

std::unique_ptr<rex::input::InputDriver> CreateMidiDrumsDriver() {
    return std::make_unique<MidiDrumsDriver>();
}

MidiDrumsStatus GetMidiDrumsStatus() {
    std::lock_guard<std::mutex> lock(MidiDrumsDriver::driver_mutex());
    auto* driver = MidiDrumsDriver::driver();
    return driver ? driver->Status() : MidiDrumsStatus{};
}

}
