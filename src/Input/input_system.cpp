#include "input_system.h"
#include <algorithm>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include "hid_instruments.h"
#include "input_lock.h"
#include "midi_drums_driver.h"
#include "player_slots.h"
#include "src/settings.h"
#include "virtual_instrument.h"
#include "xinput_state.h"

namespace band3::input {

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::kMaxGuestUsers;

namespace {

// PlayerAssignment's last view of the devices, for PlayerDevices
std::mutex g_devices_mutex;
std::vector<InputDevice> g_devices;

DeviceKind KindOf(const DeviceInfo& device) {
    if (VirtualInstrumentPlayer(device)) return DeviceKind::kVirtual;
    if (IsSdlCopyOfHidInstrument(device)) return DeviceKind::kSdlCopy;
    if (IsHidInstrument(device)) return DeviceKind::kHidInstrument;
    if (IsMidiDrums(device)) return DeviceKind::kMidiDrums;
    if (device.synthetic) return DeviceKind::kSynthetic;
    return DeviceKind::kPad;
}

// The SDK's SlotAssignment, plus (see player_slots.h):
// - a fixed player for each virtual instrument; real pads take the other players
//   in the order they connected. A player stays reserved while its instrument is
//   plugged in, including the moment a type change leaves it unplugged, so real
//   pads never shift under it.
// - SDL's copy of an instrument the HID driver reads is left out, and the other
//   pads close up over its slot, as if it had never connected.
// - the keyboard and other synthetic devices feed player 1, as in the SDK, unless
//   a virtual instrument has player 1: then it has the slot to itself, so a type
//   change empties the slot and RB3 reads the new type when it comes back (RB3
//   only reads a pad's type when its slot connects).
//
// For the launcher it can also hand player 1 a single device to read (Probe),
// or hand no player anything (Hide). The input system's callers hold
// InputLock(), and so do the launcher's, so these never change under a read.
static_assert(kPlayers == kMaxGuestUsers);

class PlayerAssignment final : public rex::input::DeviceAssignment {
public:
    void OnDevicesChanged(const std::vector<DeviceInfo>& devices) override {
        std::vector<SlotDevice> slots;
        slots.reserve(devices.size());
        for (const auto& device : devices) {
            SlotDevice slot;
            slot.ordinal = device.ordinal;
            if (int player = VirtualInstrumentPlayer(device)) {
                slot.kind = SlotDevice::Kind::kVirtual;
                slot.virtual_player = player;
            } else if (IsSdlCopyOfHidInstrument(device)) {
                slot.kind = SlotDevice::Kind::kSkipped;
            } else if (device.synthetic) {
                slot.kind = SlotDevice::Kind::kSynthetic;
            }
            slots.push_back(slot);
        }

        const auto players = AssignPlayers(slots, VirtualInstrumentPlayers());
        std::vector<InputDevice> seen(devices.size());
        for (size_t i = 0; i < devices.size(); i++) {
            seen[i].id = static_cast<uint64_t>(devices[i].id);
            seen[i].name = devices[i].name;
            seen[i].guid = devices[i].guid;
            seen[i].kind = KindOf(devices[i]);
        }
        for (uint32_t user = 0; user < kMaxGuestUsers; user++) {
            users_[user].clear();
            for (size_t i : players[user]) {
                users_[user].push_back(devices[i].id);
                if (seen[i].player == 0) seen[i].player = static_cast<int>(user) + 1;
            }
        }

        for (const auto& device : seen) {
            if (device.player != 0) {
                REXLOG_INFO("Input: {} is player {}", device.name, device.player);
            }
        }
        std::lock_guard<std::mutex> lock(g_devices_mutex);
        g_devices = std::move(seen);
    }

    void DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const override {
        out.clear();
        if (hidden_ || user_index >= kMaxGuestUsers) return;
        if (probe_) {
            if (user_index == 0) out.push_back(*probe_);
            return;
        }
        out = users_[user_index];
    }

    void Probe(std::optional<DeviceId> id) { probe_ = id; }
    void Hide(bool hidden) { hidden_ = hidden; }

private:
    std::vector<std::vector<DeviceId>> users_ = std::vector<std::vector<DeviceId>>(kMaxGuestUsers);
    std::optional<DeviceId> probe_;
    bool hidden_ = false;
};

// One of band3's own drivers (the HID instruments', the MIDI drums'), which the
// launcher starts and stops as their settings change. The input system can't
// take a driver out, so this one stays and the driver inside it comes and
// goes. Those drivers keep process-wide state, so the old one is gone before a
// new one starts.
class DriverSlot final : public rex::input::InputDriver {
public:
    DriverSlot() : InputDriver(nullptr, 0) {}

    X_STATUS Setup() override { return X_STATUS_SUCCESS; }

    bool running() {
        std::lock_guard<std::mutex> lock(mutex_);
        return driver_ != nullptr;
    }

    // Stops the driver inside, then starts `create`'s, if it sets up; with no
    // `create`, leaves the slot empty.
    void Restart(std::unique_ptr<rex::input::InputDriver> (*create)()) {
        std::lock_guard<std::mutex> lock(mutex_);
        driver_.reset();
        if (!create) return;
        auto driver = create();
        if (driver->Setup() == X_STATUS_SUCCESS) driver_ = std::move(driver);
    }

    void EnumerateDevices(std::vector<DeviceInfo>& out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (driver_) driver_->EnumerateDevices(out);
    }

    X_RESULT GetDeviceState(DeviceId id, rex::input::X_INPUT_STATE* out_state) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return driver_ ? driver_->GetDeviceState(id, out_state) : X_ERROR_DEVICE_NOT_CONNECTED;
    }

    X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                   rex::input::X_INPUT_CAPABILITIES* out_caps) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return driver_ ? driver_->GetDeviceCapabilities(id, flags, out_caps)
                       : X_ERROR_DEVICE_NOT_CONNECTED;
    }

    X_RESULT SetDeviceVibration(DeviceId id, rex::input::X_INPUT_VIBRATION* vibration) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return driver_ ? driver_->SetDeviceVibration(id, vibration)
                       : X_ERROR_DEVICE_NOT_CONNECTED;
    }

    X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                                rex::input::X_INPUT_KEYSTROKE* out_keystroke) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return driver_ ? driver_->GetDeviceKeystroke(id, flags, out_keystroke)
                       : X_ERROR_DEVICE_NOT_CONNECTED;
    }

private:
    std::mutex mutex_;
    std::unique_ptr<rex::input::InputDriver> driver_;
};

// what the MIDI drums driver reads from the settings when it starts
struct MidiConfig {
    bool enabled = false;
    std::string device;
    std::string notes;
    int32_t pulse_ms = 0;
    int32_t min_velocity = 0;
    bool combos = false;

    static MidiConfig Current() {
        return {REXCVAR_GET(midi_drums), REXCVAR_GET(midi_drums_device),
                REXCVAR_GET(midi_drums_notes), REXCVAR_GET(midi_drums_pulse_ms),
                REXCVAR_GET(midi_drums_min_velocity), REXCVAR_GET(midi_drums_combos)};
    }
    bool operator==(const MidiConfig&) const = default;
};

// the SDK's input_backend (sdl or xinput), which CreateDefaultInputSystem reads
std::string Backend() { return rex::cvar::GetFlagByName("input_backend"); }

// An input system with band3's drivers, and the parts the launcher changes.
struct Built {
    std::unique_ptr<rex::input::InputSystem> system;
    std::string backend;
    PlayerAssignment* assignment = nullptr;
    DriverSlot* hid = nullptr;
    DriverSlot* midi = nullptr;
    bool hid_on = false;
    MidiConfig midi_config;

    // starts and stops the HID and MIDI drivers to match the settings
    void Follow(bool hid_wanted, const MidiConfig& midi_wanted) {
        if (hid_wanted != hid_on) {
            hid->Restart(hid_wanted ? &CreateHidInstrumentDriver : nullptr);
            hid_on = hid_wanted;
        }
        if (!(midi_wanted == midi_config)) {
            midi->Restart(midi_wanted.enabled ? &CreateMidiDrumsDriver : nullptr);
            midi_config = midi_wanted;
        }
    }

    // band3's drivers off, for a system that isn't the one being read
    void Stop() { Follow(false, MidiConfig{}); }
};

Built Build() {
    Built built;
    built.backend = Backend();
    built.system = rex::input::CreateDefaultInputSystem(false);

    auto virtual_driver = CreateVirtualInstrumentDriver();
    if (virtual_driver->Setup() == X_STATUS_SUCCESS) {
        built.system->AddDriver(std::move(virtual_driver));
    }
    auto hid = std::make_unique<DriverSlot>();
    auto midi = std::make_unique<DriverSlot>();
    built.hid = hid.get();
    built.midi = midi.get();
    built.system->AddDriver(std::move(hid));
    built.system->AddDriver(std::move(midi));
    built.Follow(REXCVAR_GET(hid_instruments), MidiConfig::Current());

    auto assignment = std::make_unique<PlayerAssignment>();
    built.assignment = assignment.get();
    built.system->SetDeviceAssignment(std::move(assignment));
    return built;
}

rex::input::InputSystem* g_game_input = nullptr;

// The launcher's input system, before the runtime takes it (UI thread only).
std::optional<Built> g_prepared;
rex::ui::Window* g_window = nullptr;
// An SDL system the launcher switched away from: kept for switching back, and
// never destroyed once it has seen the window (see input_system.h).
std::optional<Built>* g_parked_sdl = new std::optional<Built>();
// ReadyInputForGame was called: the game's from here on
bool g_ready = false;

// makes `backend`'s system the prepared one
void SwitchTo(const std::string& backend) {
    std::lock_guard<std::recursive_mutex> lock(InputLock());
    if (g_prepared) {
        g_prepared->Stop();
        // an SDL system that has seen the window can't be destroyed
        if (g_prepared->backend == "sdl" && g_window) {
            *g_parked_sdl = std::move(g_prepared);
        }
        g_prepared.reset();
    }
    if (backend == "sdl" && *g_parked_sdl) {
        g_prepared = std::move(*g_parked_sdl);
        g_parked_sdl->reset();
        g_prepared->Follow(REXCVAR_GET(hid_instruments), MidiConfig::Current());
    } else {
        g_prepared = Build();
    }
    if (g_window) g_prepared->system->AttachWindow(g_window);
    REXLOG_INFO("Input: the launcher's input system uses {}", g_prepared->backend);
}

}

std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode) {
    if (tool_mode) return rex::input::CreateDefaultInputSystem(tool_mode);

    std::unique_ptr<rex::input::InputSystem> input;
    if (g_prepared) {
        ApplyInputSettings();
        input = std::move(g_prepared->system);
        g_prepared.reset();
        REXLOG_INFO("Input: the game takes over the launcher's input system");
    } else {
        input = std::move(Build().system);
    }
    g_ready = true;
    g_game_input = input.get();
    return input;
}

rex::input::InputSystem* GameInputSystem() { return g_game_input; }

void PrepareInputSystem(rex::ui::Window* window) {
    if (g_game_input) return;
    g_window = window;
    if (!g_prepared || g_prepared->backend != Backend()) {
        SwitchTo(Backend());
    } else if (g_window) {
        std::lock_guard<std::recursive_mutex> lock(InputLock());
        g_prepared->system->AttachWindow(g_window);
    }
}

void ApplyInputSettings() {
    if (!g_prepared || g_ready) return;
    const std::string backend = Backend();
    if (backend != g_prepared->backend) {
        SwitchTo(backend);
        return;
    }
    const bool hid = REXCVAR_GET(hid_instruments);
    const MidiConfig midi = MidiConfig::Current();
    if (hid == g_prepared->hid_on && midi == g_prepared->midi_config) return;
    std::lock_guard<std::recursive_mutex> lock(InputLock());
    g_prepared->Follow(hid, midi);
}

void ReadyInputForGame() {
    if (!g_prepared || g_ready) return;
    ApplyInputSettings();
    std::lock_guard<std::recursive_mutex> lock(InputLock());
    // a player with no device reads as disconnected, which the input system
    // notes without telling anyone while there's no runtime
    rex::input::X_INPUT_STATE state{};
    g_prepared->assignment->Hide(true);
    for (uint32_t user = 0; user < kMaxGuestUsers; user++) {
        g_prepared->system->GetStateForUI(user, &state);
    }
    g_prepared->assignment->Hide(false);
    // and drops the keystrokes queued while the launcher was typed in, so the
    // game doesn't read them as menu presses
    for (uint32_t user = 0; user < kMaxGuestUsers; user++) {
        rex::input::X_INPUT_KEYSTROKE keystroke{};
        for (int i = 0; i < 256; i++) {
            if (g_prepared->system->GetKeystroke(user, 0, &keystroke) != X_ERROR_SUCCESS) break;
        }
    }
    g_ready = true;
}

std::vector<InputDevice> PlayerDevices() {
    // before the game nothing else asks the input system, which only looks for
    // devices when asked
    if (g_prepared && !g_ready) {
        std::lock_guard<std::recursive_mutex> lock(InputLock());
        rex::input::X_INPUT_CAPABILITIES caps{};
        g_prepared->system->GetCapabilities(0, 0, &caps);
    }
    std::lock_guard<std::mutex> lock(g_devices_mutex);
    return g_devices;
}

std::optional<DeviceReading> ReadInputDevice(uint64_t id) {
    if (!g_prepared || g_ready) return std::nullopt;
    std::lock_guard<std::recursive_mutex> lock(InputLock());
    // player 1 is handed just this device for the read
    PlayerAssignment& assignment = *g_prepared->assignment;
    assignment.Probe(static_cast<DeviceId>(id));
    rex::input::X_INPUT_CAPABILITIES caps{};
    rex::input::X_INPUT_STATE state{};
    const bool ok = g_prepared->system->GetCapabilities(0, 0, &caps) == X_ERROR_SUCCESS &&
                    g_prepared->system->GetStateForUI(0, &state) == X_ERROR_SUCCESS;
    assignment.Probe(std::nullopt);
    if (!ok) return std::nullopt;
    return DeviceReading{LoadCaps(caps), LoadGamepad(state.gamepad)};
}

}
