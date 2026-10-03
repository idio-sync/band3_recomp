#pragma once
#include <memory>
#include <string>
#include "mic_level.h"

// A live level meter on one microphone, for the launcher's mic slots. Each
// meter records on its own stream from band3's SDL (SDL_OpenAudioDeviceStream),
// apart from the game's mic capture, which mustn't start before the game, and
// starts and stops SDL's audio subsystem itself, once each. Meters are for the
// launcher only: StartFromLauncher closes any still open (CloseMicMeters)
// before the game starts.

namespace band3::launcher {

class MicMeter {
public:
    // Starts recording the first microphone whose name contains `device`,
    // ignoring case, as a usb_mic_devices slot picks it; an empty name records
    // the default microphone. UI thread.
    explicit MicMeter(std::string device);
    ~MicMeter();
    MicMeter(const MicMeter&) = delete;
    MicMeter& operator=(const MicMeter&) = delete;

    // whether it's recording; when not, error() says why
    bool recording() const;
    std::string error() const;
    // the device it records, as SDL names it
    std::string device_name() const;

    // the level now
    MicLevel::Reading Level() const;

    // stops recording; the meter reads silence from here on
    void Close();

    // what a meter shares with SDL's audio thread (mic_meter.cpp)
    struct State;

private:
    std::shared_ptr<State> state_;
};

// closes every meter still open; for Play
void CloseMicMeters();

}
