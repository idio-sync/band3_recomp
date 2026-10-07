#include "midi_keys_driver.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <rex/logging.h>
#include "midi_port.h"
#include "song_state.h"
#include "src/settings.h"
#include "xinput_state.h"

namespace band3::input {

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::DeviceId;
using rex::input::DeviceInfo;

namespace {

// clear of the SDK drivers' ids and band3's others ("B3VI", "B3HI", "B3MI")
constexpr uint64_t kDeviceIdBase = 0x42334D4B00000000ull;  // "B3MK"
constexpr std::string_view kGuidPrefix = "band3-midikeys:";
// counts across drivers, since the launcher restarts the driver inside one
// input system, which never takes an id back
std::atomic<uint64_t> g_generation{0};
// how many events the Lab shows
constexpr size_t kRecentEvents = 12;
// the port name the test harness's keyboard reports
constexpr std::string_view kHarnessPort = "harness";

// midi_keys_base_note as it is now. Read as the keyboard plays rather than
// handed over by the launcher, so a change takes in the launcher and in the
// game alike, without a restart.
uint8_t BaseNote() {
    return static_cast<uint8_t>(std::clamp(REXCVAR_GET(midi_keys_base_note), 0, 127 - 24));
}

class MidiKeysDriver final : public rex::input::InputDriver {
public:
    MidiKeysDriver()
        : InputDriver(nullptr, 0), keyboard_({}), midi_("MIDI keyboard", "band3 keys") {}
    ~MidiKeysDriver() override { Stop(); }

    X_STATUS Setup() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            base_note_ = BaseNote();
            keyboard_ = midi_keys::Keyboard({.base_note = base_note_});
            // only ever under the harness, which is what it's for
            test_device_ = REXCVAR_GET(midi_keys_test_device) && REXCVAR_GET(test_port) != 0;
        }
        if (test_device_) {
            REXLOG_INFO("MIDI keyboard: the harness's keyboard connects with its first message");
        }
        if (!midi_.Start(
                REXCVAR_GET(midi_keys_device),
                [this](std::span<const uint8_t> message) { OnMessage(message); },
                [this](const std::string& port) { OnPort(port); })) {
            return X_STATUS_UNSUCCESSFUL;
        }

        {
            std::lock_guard<std::mutex> lock(driver_mutex());
            driver() = this;
        }
        return X_STATUS_SUCCESS;
    }

    void EnumerateDevices(std::vector<DeviceInfo>& out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id_ == DeviceId::kInvalid) return;
        DeviceInfo info;
        info.id = id_;
        info.name = "MIDI keyboard: " + port_;
        info.guid = std::string(kGuidPrefix) + port_;
        info.synthetic = false;
        out.push_back(std::move(info));
    }

    X_RESULT GetDeviceState(DeviceId id, rex::input::X_INPUT_STATE* out_state) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id == DeviceId::kInvalid || id != id_) return X_ERROR_DEVICE_NOT_CONNECTED;
        FollowBaseNote();
        const Gamepad360 g = EncodeKeys(keyboard_.State(midi_keys::Clock::now()));
        if (auto event = keyboard_.TakeTimedEvent()) Remember(std::move(*event));
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
        if (out_caps) StoreCaps(KeysCaps(), *out_caps);
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

    // the one running driver, for GetMidiKeysStatus and InjectMidiKeysMessage
    static std::mutex& driver_mutex() {
        static std::mutex mutex;
        return mutex;
    }
    static MidiKeysDriver*& driver() {
        static MidiKeysDriver* running = nullptr;
        return running;
    }

    // the harness's message, as from the port; the harness's keyboard
    // connects with the first one while no port is open
    void Inject(std::span<const uint8_t> message) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (test_device_ && id_ == DeviceId::kInvalid) Connect(std::string(kHarnessPort));
        Receive(message);
    }

    MidiKeysStatus Status() {
        MidiKeysStatus status;
        status.running = true;
        status.ports = midi_.ports();
        status.mode = CurrentKeysMode();
        std::lock_guard<std::mutex> lock(mutex_);
        status.port = id_ != DeviceId::kInvalid ? port_ : std::string();
        status.recent.assign(recent_.begin(), recent_.end());
        status.base_note = base_note_;
        return status;
    }

private:
    // RtMidi's thread, for every message from the open port
    void OnMessage(std::span<const uint8_t> message) {
        std::lock_guard<std::mutex> lock(mutex_);
        Receive(message);
    }

    // under mutex_
    void Receive(std::span<const uint8_t> message) {
        FollowBaseNote();
        auto event = keyboard_.Receive(message, midi_keys::Clock::now(), CurrentKeysMode());
        if (event) Remember(std::move(*event));
    }

    // under mutex_: a new base note, from the next message or read on
    void FollowBaseNote() {
        const uint8_t base_note = BaseNote();
        if (base_note == base_note_) return;
        base_note_ = base_note;
        keyboard_.SetSettings({.base_note = base_note});
    }

    // under mutex_
    void Remember(midi_keys::Event event) {
        recent_.push_back(std::move(event));
        if (recent_.size() > kRecentEvents) recent_.pop_front();
    }

    // under mutex_: reports the keyboard on `port`, with a new id, which reads
    // as a new controller
    void Connect(const std::string& port) {
        port_ = port;
        id_ = static_cast<DeviceId>(kDeviceIdBase + ++g_generation);
    }

    // the port's watcher thread, when the port opens (its name) or closes (empty)
    void OnPort(const std::string& port) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (port.empty()) {
            id_ = DeviceId::kInvalid;
            return;
        }
        Connect(port);
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(driver_mutex());
            if (driver() == this) driver() = nullptr;
        }
        midi_.Stop();
    }

    // shared by RtMidi's thread, the port's watcher thread, guest threads, the
    // Lab, the launcher and the harness (Inject)
    std::mutex mutex_;
    midi_keys::Keyboard keyboard_;
    uint8_t base_note_ = 48;
    // midi_keys_test_device, under the harness
    bool test_device_ = false;
    DeviceId id_ = DeviceId::kInvalid;
    std::string port_;
    std::deque<midi_keys::Event> recent_;
    Gamepad360 last_{};
    uint32_t packet_number_ = 0;

    // last, so it goes first: its threads call into the members above
    MidiPort midi_;
};

}

std::unique_ptr<rex::input::InputDriver> CreateMidiKeysDriver() {
    return std::make_unique<MidiKeysDriver>();
}

bool IsMidiKeys(const DeviceInfo& device) { return device.guid.starts_with(kGuidPrefix); }

MidiKeysStatus GetMidiKeysStatus() {
    std::lock_guard<std::mutex> lock(MidiKeysDriver::driver_mutex());
    auto* driver = MidiKeysDriver::driver();
    return driver ? driver->Status() : MidiKeysStatus{};
}

bool InjectMidiKeysMessage(std::span<const uint8_t> message) {
    std::lock_guard<std::mutex> lock(MidiKeysDriver::driver_mutex());
    auto* driver = MidiKeysDriver::driver();
    if (!driver) return false;
    driver->Inject(message);
    return true;
}

}
