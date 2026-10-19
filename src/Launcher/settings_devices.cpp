#include "settings_devices.h"
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <rex/logging.h>
#include "device_lists.h"
#include "device_view_model.h"
#include "src/Input/input_system.h"
#include "src/Input/player_slots.h"

namespace band3::launcher {

SettingsDevices::SettingsDevices(std::function<void*()> native_window)
    : native_window_(std::move(native_window)) {}

SettingsDevices::~SettingsDevices() { CloseMeters(); }

void SettingsDevices::Refresh(const SettingsModel& model, Category category) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point now = Clock::now();
    // a list is kept for this long, also while its category is away, so going
    // back and forth doesn't list again each time
    constexpr auto kEvery = std::chrono::seconds(2);
    auto due = [&](bool showing, Clock::time_point& next) {
        if (!showing || now < next) return false;
        next = now + kEvery;
        return true;
    };
    auto shown = [&](std::string_view cvar) {
        const Setting* s = model.Find(cvar);
        return s && model.Visible(*s);
    };
    // a controller left out (here, or on Test Controllers) takes effect at once
    input::FollowPlayerSettings();
    if (category == Category::kControllers && shown("left_out_controllers") && now >= next_controllers_) {
        // only a copy of the input system's list, so it's cheap to keep current
        next_controllers_ = now + std::chrono::milliseconds(500);
        lists_.controllers.clear();
        for (const input::InputDevice& d : input::PlayerDevices()) {
            if (!DrivesNavigation(d.kind) || d.kind == input::DeviceKind::kVirtual) continue;
            lists_.controllers.push_back({.name = d.name.empty() ? "(unnamed)" : d.name,
                                          .key = input::LeftOutKey(d.name, d.guid),
                                          .player = d.player,
                                          .left_out = d.left_out,
                                          .deck_aside = d.deck_aside});
        }
    }
    const bool mics = category == Category::kAudio && shown("usb_mic_devices");
    audio_wanted_ = mics;
    if (mics) HoldAudio(true);
    if (due(mics, next_mics_)) {
        std::vector<std::string> found = RecordingDeviceNames();
        if (found != lists_.mics || !mics_listed_) {
            lists_.mics = std::move(found);
            mic_generation_++;
        }
        mics_listed_ = true;
    }
    if (due(category == Category::kMidi && (shown("midi_drums_device") || shown("midi_keys_device")),
            next_midi_)) {
        lists_.midi_ports = MidiInputPorts();
        midi_listed_ = true;
    }
    if (due(category == Category::kDisplay && (shown("monitor") || shown("resolution")),
            next_monitors_)) {
        lists_.monitors = ListMonitors(native_window_ ? native_window_() : nullptr);
    }
}

std::optional<SettingsDevices::Meter> SettingsDevices::MeterFor(
    int slot, const std::optional<std::string>& device) {
    SlotMeter& m = meters_[static_cast<size_t>(slot)];
    m.used = true;
    // a meter that couldn't record tries again when the microphones change
    if (m.meter && (!device || *device != m.device ||
                    (m.generation != mic_generation_ && !m.meter->recording()))) {
        m.meter.reset();
    }
    if (!device) return std::nullopt;
    if (!m.meter) {
        m.meter = std::make_unique<MicMeter>(*device);
        m.device = *device;
        m.generation = mic_generation_;
    }
    Meter out;
    out.recording = m.meter->recording();
    if (!out.recording) {
        out.error = m.meter->error();
        return out;
    }
    out.level = m.meter->Level();
    return out;
}

void SettingsDevices::EndFrame() {
    for (SlotMeter& m : meters_) {
        if (!m.used) m.meter.reset();
        m.used = false;
    }
    if (!audio_wanted_) {
        HoldAudio(false);
        audio_hold_failed_ = false;
    }
    audio_wanted_ = false;
}

void SettingsDevices::CloseMeters() {
    for (SlotMeter& m : meters_) m = {};
    HoldAudio(false);
    audio_hold_failed_ = false;
}

void SettingsDevices::HoldAudio(bool hold) {
    if (hold == audio_held_ || (hold && audio_hold_failed_)) return;
    if (!hold) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio_held_ = false;
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        REXLOG_WARN("Launcher: SDL's audio didn't start ({})", SDL_GetError());
        audio_hold_failed_ = true;
        return;
    }
    audio_held_ = true;
}

}
