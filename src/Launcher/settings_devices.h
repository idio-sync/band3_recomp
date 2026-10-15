#pragma once
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include "launcher_settings.h"
#include "mic_level.h"
#include "mic_meter.h"
#include "ui/settings_rows.h"

// What the Settings screen finds plugged in: the microphones, the MIDI ports
// and the monitors, listed again every couple of seconds while a category
// that picks one shows (listing starts SDL's audio, or walks every display
// mode, which mustn't happen every frame), and a level meter on each mic
// slot's microphone while the Audio category shows it (the launcher's only:
// in game the game is capturing from them). UI thread.

namespace band3::launcher {

class SettingsDevices {
public:
    explicit SettingsDevices(std::function<void*()> native_window);
    ~SettingsDevices();
    SettingsDevices(const SettingsDevices&) = delete;
    SettingsDevices& operator=(const SettingsDevices&) = delete;

    // before drawing `category`: lists what it picks from again, when due
    void Refresh(const SettingsModel& model, Category category);
    const ui::DeviceLists& Lists() const { return lists_; }
    // the microphones have been listed at least once
    bool MicsListed() const { return mics_listed_; }
    bool MidiListed() const { return midi_listed_; }

    struct Meter {
        bool recording = false;
        // why it isn't, when it isn't
        std::string error;
        MicLevel::Reading level;
    };
    // the level on `slot`'s microphone (`device`: its saved name, "" for the
    // system default), opening or moving its meter as need be; nullopt for none
    std::optional<Meter> MeterFor(int slot, const std::optional<std::string>& device);
    // after drawing: closes the meters not asked for this frame, and lets go
    // of SDL's audio once none are
    void EndFrame();
    // closes every meter and lets go of SDL's audio (for Play)
    void CloseMeters();

private:
    struct SlotMeter {
        std::unique_ptr<MicMeter> meter;
        std::string device;
        // mic_generation_ when it was opened
        unsigned generation = 0;
        bool used = false;
    };
    // keeps band3's SDL audio subsystem started while the mic slots show, so
    // listing the microphones every couple of seconds doesn't start and stop
    // it each time, and SDL sees them come and go
    void HoldAudio(bool hold);

    std::function<void*()> native_window_;
    ui::DeviceLists lists_;
    bool mics_listed_ = false;
    bool midi_listed_ = false;
    // counts changes to the microphones, so a meter that couldn't record
    // tries again once they change
    unsigned mic_generation_ = 0;
    std::chrono::steady_clock::time_point next_mics_{};
    std::chrono::steady_clock::time_point next_midi_{};
    std::chrono::steady_clock::time_point next_monitors_{};
    std::array<SlotMeter, 3> meters_;
    bool audio_wanted_ = false;
    bool audio_held_ = false;
    bool audio_hold_failed_ = false;
};

}
