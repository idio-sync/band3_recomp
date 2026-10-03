#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>
#include "device_view_model.h"
#include "src/Input/input_system.h"

// The Controllers tab's device list and test view: every device the input
// system has, with the player each feeds, and the selected one drawn live
// (guitar frets, strum, whammy and tilt; drum pads and cymbals flashing with
// how hard they're hit; a controller's buttons and sticks), read as the game
// would read it (input::ReadInputDevice). Before the game only: Poll and Draw
// are called while the launcher's settings are edited, never after Play.

namespace band3::launcher {

class DevicePanel {
public:
    // Looks for devices again every quarter second (hotplugs), and reads every
    // device gamepad navigation uses, and the selected one. Once a frame.
    void Poll();

    // the devices' states for gamepad navigation, from the last Poll
    const std::vector<NavPad>& NavPads() const { return nav_pads_; }

    // the device list and the selected device's test view, where the cursor is
    void Draw();

private:
    using Clock = std::chrono::steady_clock;

    struct Device {
        input::InputDevice info;
        // this frame's reading, for devices Poll reads
        std::optional<input::DeviceReading> reading;
    };

    // a pad, cymbal or kick's last hit, for its flash
    struct Hit {
        uint8_t velocity = 0;
        Clock::time_point at{};
    };

    // the device the test view shows, picked by the player or, until they
    // pick one, the first that plays
    void ChooseDevice();
    void DrawList();
    void DrawTestView(const Device& device);
    void DrawGuitar(const input::GuitarInputs& in);
    void DrawDrums(const input::DrumInputs& in, bool velocity);
    void DrawPad(const input::Gamepad360& g);
    void DrawMidiHits();

    // how brightly a pad shows: its last hit's velocity, fading (FlashLevel)
    float Flash(Hit& hit, uint8_t velocity_now);

    std::vector<Device> devices_;
    bool listed_ = false;
    Clock::time_point next_list_{};
    // the subtype each device last reported, for its kind's label
    std::map<uint64_t, uint8_t> sub_types_;
    std::vector<NavPad> nav_pads_;

    std::optional<uint64_t> selected_;
    // the player chose it: it stays chosen while it's connected
    bool picked_ = false;
    // the drum view's hits, for the device they're of
    uint64_t hits_for_ = 0;
    std::array<Hit, input::kPadCount> pad_hits_{};
    std::array<Hit, input::kCymbalCount> cymbal_hits_{};
    std::array<Hit, 2> kick_hits_{};
};

}
