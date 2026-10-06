#include "input_system.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <rex/cvar.h>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include "hid_instruments.h"
#include "input_backend.h"
#include "input_lock.h"
#include "midi_drums_driver.h"
#include "player_slots.h"
#include "restart_debounce.h"
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
// - a pad keeps its player while it's connected, wherever the others go, and a
//   pad that connects takes the lowest free player.
// - SDL's copy of an instrument the HID driver reads is left out, and the other
//   pads close up over its slot, as if it had never connected.
// - the keyboard and other synthetic devices feed player 1, as in the SDK, unless
//   a virtual instrument has player 1: then it has the slot to itself, so a type
//   change empties the slot and RB3 reads the new type when it comes back (RB3
//   only reads a pad's type when its slot connects).
//
// For the launcher it can also hand one player a single device to read (Probe),
// or hand no player anything (Hide), and assign the players again for devices
// that stay but read differently (Reassign). The input system's callers hold
// InputLock(), and so do the launcher's, so these never change under a read.
static_assert(kPlayers == kMaxGuestUsers);

class PlayerAssignment final : public rex::input::DeviceAssignment {
public:
    void OnDevicesChanged(const std::vector<DeviceInfo>& devices) override {
        devices_ = devices;
        Assign();
    }

    // The players again for the same devices: which are SDL's copies of a HID
    // instrument depends on whether the HID driver runs, and the SDK only
    // calls OnDevicesChanged when the devices themselves change. The pads take
    // their players afresh: a stopped driver's devices stay in the SDK's list
    // until it next looks, and mustn't keep a player from SDL's copy.
    void Reassign() {
        seats_.clear();
        Assign();
    }

    void DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const override {
        out.clear();
        if (hidden_ || user_index >= kMaxGuestUsers) return;
        if (probe_) {
            if (user_index == probe_user_) out.push_back(*probe_);
            return;
        }
        out = users_[user_index];
    }

    // hands `id` alone to a player it doesn't feed (ProbePlayer), and returns
    // that player's user index
    uint32_t Probe(DeviceId id) {
        std::array<bool, kPlayers> feeds{};
        for (uint32_t user = 0; user < kMaxGuestUsers; user++) {
            feeds[user] = std::find(users_[user].begin(), users_[user].end(), id) != users_[user].end();
        }
        probe_ = id;
        probe_user_ = static_cast<uint32_t>(ProbePlayer(feeds));
        return probe_user_;
    }
    void EndProbe() { probe_.reset(); }
    void Hide(bool hidden) { hidden_ = hidden; }

private:
    void Assign() {
        const std::vector<DeviceInfo>& devices = devices_;
        std::vector<SlotDevice> slots;
        slots.reserve(devices.size());
        for (const auto& device : devices) {
            SlotDevice slot;
            if (auto seat = seats_.find(device.id); seat != seats_.end()) slot.seat = seat->second;
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
        seats_.clear();
        for (uint32_t user = 0; user < kMaxGuestUsers; user++) {
            users_[user].clear();
            for (size_t i : players[user]) {
                users_[user].push_back(devices[i].id);
                if (seen[i].player == 0) seen[i].player = static_cast<int>(user) + 1;
                if (slots[i].kind == SlotDevice::Kind::kPad) seats_[devices[i].id] = seen[i].player;
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

    // the SDK's last list, for Reassign
    std::vector<DeviceInfo> devices_;
    // the player (1-4) each pad had at the last assignment
    std::map<DeviceId, int> seats_;
    std::vector<std::vector<DeviceId>> users_ = std::vector<std::vector<DeviceId>>(kMaxGuestUsers);
    std::optional<DeviceId> probe_;
    uint32_t probe_user_ = 0;
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

// what the MIDI drums driver reads from the settings only when it starts, so
// a change restarts it
struct MidiConfig {
    bool enabled = false;
    std::string device;
    std::string notes;

    static MidiConfig Current() {
        return {REXCVAR_GET(midi_drums), REXCVAR_GET(midi_drums_device),
                REXCVAR_GET(midi_drums_notes)};
    }
    bool operator==(const MidiConfig&) const = default;
};

// what a running MIDI drums driver follows as it changes
// (UpdateMidiDrumsSettings): the launcher's Minimum velocity slider changes it
// every frame of a drag, which mustn't reopen the kit each time
struct MidiLive {
    int32_t pulse_ms = 0;
    int32_t min_velocity = 0;
    bool combos = false;

    static MidiLive Current() {
        return {REXCVAR_GET(midi_drums_pulse_ms), REXCVAR_GET(midi_drums_min_velocity),
                REXCVAR_GET(midi_drums_combos)};
    }
    bool operator==(const MidiLive&) const = default;
};

// how long the MIDI settings stay the same before the driver restarts for them
constexpr std::chrono::milliseconds kMidiSettle{300};

// the backend CreateDefaultInputSystem builds for input_backend as it is now
std::string Backend() {
#ifdef _WIN32
    constexpr bool kWindows = true;
#else
    constexpr bool kWindows = false;
#endif
    return std::string(BackendFor(rex::cvar::GetFlagByName("input_backend"), kWindows));
}

// An input system with band3's drivers, and the parts the launcher changes.
struct Built {
    std::unique_ptr<rex::input::InputSystem> system;
    std::string backend;
    PlayerAssignment* assignment = nullptr;
    DriverSlot* hid = nullptr;
    DriverSlot* midi = nullptr;
    bool hid_on = false;
    MidiConfig midi_config;
    MidiLive midi_live;
    // the MIDI settings restart the driver once they settle: a dropdown
    // clicked through, or a port typed and changed again, opens the kit once.
    // hid_instruments is a checkbox, which changes once a click, so the HID
    // driver restarts at once.
    RestartDebounce<MidiConfig> midi_settle{kMidiSettle};

    // starts and stops the HID and MIDI drivers to match the settings, and
    // assigns the players again after any restart: a device the SDK still
    // lists may read differently now (PlayerAssignment::Reassign)
    void Follow(bool hid_wanted, const MidiConfig& midi_wanted) {
        bool restarted = false;
        if (hid_wanted != hid_on) {
            hid->Restart(hid_wanted ? &CreateHidInstrumentDriver : nullptr);
            hid_on = hid_wanted;
            restarted = true;
        }
        if (!(midi_wanted == midi_config)) {
            midi->Restart(midi_wanted.enabled ? &CreateMidiDrumsDriver : nullptr);
            midi_config = midi_wanted;
            // a new driver reads them as it starts
            midi_live = MidiLive::Current();
            restarted = true;
        }
        if (restarted && assignment) assignment->Reassign();
    }

    // hands the running MIDI driver the settings it follows, when they changed
    void FollowLive(const MidiLive& wanted) {
        if (wanted == midi_live) return;
        UpdateMidiDrumsSettings();
        midi_live = wanted;
    }
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
    built.midi_live = MidiLive::Current();

    auto assignment = std::make_unique<PlayerAssignment>();
    built.assignment = assignment.get();
    built.system->SetDeviceAssignment(std::move(assignment));
    return built;
}

rex::input::InputSystem* g_game_input = nullptr;

// The launcher's input system, before the runtime takes it (UI thread only).
std::optional<Built> g_prepared;
// ReadyInputForGame was called: the game's from here on
bool g_ready = false;

// ApplyInputSettings; `settle` holds a MIDI restart back until its settings
// have settled, which Play's last apply doesn't
void ApplySettings(bool settle) {
    if (!g_prepared || g_ready) return;
    Built& built = *g_prepared;
    const bool hid = REXCVAR_GET(hid_instruments);
    const MidiConfig wanted = MidiConfig::Current();
    const bool midi_due = settle ? built.midi_settle.Due(wanted, built.midi_config,
                                                         std::chrono::steady_clock::now())
                                 : !(wanted == built.midi_config);
    built.FollowLive(MidiLive::Current());
    if (hid == built.hid_on && !midi_due) return;
    std::lock_guard<std::recursive_mutex> lock(InputLock());
    built.Follow(hid, midi_due ? wanted : built.midi_config);
}

}

std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode) {
    if (tool_mode) return rex::input::CreateDefaultInputSystem(tool_mode);

    std::unique_ptr<rex::input::InputSystem> input;
    if (g_prepared) {
        ApplySettings(false);
        // Play restarts band3 for a new backend unless it couldn't (unsaved,
        // or under the test harness); then it applies at the next start
        if (const std::string backend = Backend(); backend != g_prepared->backend) {
            REXLOG_WARN("Input: input_backend is {}, but the game keeps the launcher's {} "
                        "until band3 restarts",
                        backend, g_prepared->backend);
        }
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
    std::lock_guard<std::recursive_mutex> lock(InputLock());
    if (!g_prepared) {
        g_prepared = Build();
        REXLOG_INFO("Input: the launcher's input system uses {}", g_prepared->backend);
    }
    if (window) g_prepared->system->AttachWindow(window);
}

bool InputBackendChanged() {
    return g_prepared && !g_ready && Backend() != g_prepared->backend;
}

void ApplyInputSettings() { ApplySettings(true); }

void ReadyInputForGame() {
    if (!g_prepared || g_ready) return;
    ApplySettings(false);
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

namespace {

// Reads one device through `read(system, user)` on a player it doesn't feed,
// which is handed just this device for the read, so the SDK's note of the
// device each player last used (GetStateForUI) can't change which device the
// game reads first. False if the read fails.
template <typename Read>
bool ReadAlone(uint64_t id, Read read) {
    if (!g_prepared || g_ready) return false;
    std::lock_guard<std::recursive_mutex> lock(InputLock());
    PlayerAssignment& assignment = *g_prepared->assignment;
    const uint32_t user = assignment.Probe(static_cast<DeviceId>(id));
    const bool ok = read(*g_prepared->system, user) == X_ERROR_SUCCESS;
    assignment.EndProbe();
    return ok;
}

}

std::optional<Caps360> ReadInputCaps(uint64_t id) {
    rex::input::X_INPUT_CAPABILITIES caps{};
    const bool ok = ReadAlone(id, [&](rex::input::InputSystem& system, uint32_t user) {
        return system.GetCapabilities(user, 0, &caps);
    });
    if (!ok) return std::nullopt;
    return LoadCaps(caps);
}

std::optional<Gamepad360> ReadInputState(uint64_t id) {
    rex::input::X_INPUT_STATE state{};
    const bool ok = ReadAlone(id, [&](rex::input::InputSystem& system, uint32_t user) {
        return system.GetStateForUI(user, &state);
    });
    if (!ok) return std::nullopt;
    return LoadGamepad(state.gamepad);
}

}
