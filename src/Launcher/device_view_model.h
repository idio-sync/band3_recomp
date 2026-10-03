#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include "src/Input/device_kind.h"
#include "src/Input/instruments.h"

// The pure parts of the launcher's Controllers device list, test view
// (instrument_view.h) and gamepad navigation (gamepad_nav.h): what a device is
// called, which view draws it, how a hit fades, what a device's state does
// to the launcher's navigation, and which devices navigate while one is
// tested. Kept apart from the ImGui and input
// system code so the unit tests can check them.

namespace band3::launcher {

// The SDK's stand-in controller, which reads as a controller that never
// presses anything, as against the keyboard and mouse (with mnk_mode): both
// are synthetic devices, told apart by name. The SDK's names, from its
// rexruntime.dll: the stand-in (NopInputDriver, id "NOP") is "None"; the
// keyboard (MnkInputDriver, id "MNK") is "Keyboard and Mouse", or "Keyboard"
// in one of its modes. Anything synthetic not named for a keyboard or mouse
// counts as a stand-in.
bool IsStandIn(input::DeviceKind kind, std::string_view name);

// what a device is, for the device list: "Controller", "Xbox 360 guitar",
// "Drum kit (USB dongle)", "MIDI drum kit"... `sub_type` is the XInput
// subtype it reports, when it has been read
std::string DeviceKindLabel(input::DeviceKind kind, std::string_view name,
                            std::optional<uint8_t> sub_type);

// "Player 2", or "Not playing"
std::string PlayerLabel(int player);

// what the game plays a gamepad (or the keyboard) as for a controller_type
// value: "Guitar", "Drums", "Vocals"; nullopt for -1, where RB3 reads a
// gamepad as no instrument at all
std::optional<std::string> PlaysAsLabel(int controller_type);

// how the test view draws a device, by the subtype it reports
enum class TestView { kGuitar, kDrums, kPad };
TestView ViewFor(const input::Caps360& caps);

// How brightly a pad or cymbal shows `seconds` after a hit at `velocity`
// (1-127; 0 = not hit): from 0.35 for the softest to 1 for the hardest, at
// once, fading out over kFlashSeconds. A hit still held reads as 0 seconds.
inline constexpr float kFlashSeconds = 0.35f;
float FlashLevel(uint8_t velocity, float seconds);

// Gamepad navigation

// what one device does to the launcher's navigation: Xbox button bits
// (input::xbox), and the sticks and triggers past their dead zones
struct NavPad {
    uint16_t buttons = 0;
    // -1 to 1, up and right positive
    float left_x = 0, left_y = 0, right_x = 0, right_y = 0;
    // 0 to 1
    float left_trigger = 0, right_trigger = 0;

    bool operator==(const NavPad&) const = default;
};

// a stick axis past XInput's left-stick dead zone, -1 to 1
float StickAmount(int16_t axis);
// a trigger past XInput's trigger threshold, 0 to 1
float TriggerAmount(uint8_t trigger);

// whether a device of this kind drives the launcher's navigation: everything
// that plays but the keyboard (which navigates by itself) and SDL's copies
// of dongle instruments (the dongle's reading is the one that counts)
bool DrivesNavigation(input::DeviceKind kind);

// One device's state as navigation input. A gamepad gives everything. An
// instrument gives only its menu buttons (A, B, X, Y, Start, Back, the d-pad),
// as RB3's menus read it: its whammy, tilt and pickup are sticks and a
// trigger, and a guitar's orange fret and a kit's kick pedal are LB, which
// would switch tabs. An RB2-or-later kit's cymbal hit sets d-pad up or down
// as its color's marker, which isn't navigation either.
NavPad NavFromReading(const input::Caps360& caps, const input::Gamepad360& state);

// several devices at once: any device's buttons, and each axis as far as any
// device pushes it
NavPad CombinePads(std::span<const NavPad> pads);

// the launcher's own buttons, pressed since the last frame
struct NavEdges {
    bool start = false;         // Play
    bool tab_previous = false;  // LB
    bool tab_next = false;      // RB
};
NavEdges PressedEdges(uint16_t before, uint16_t now);

// The test view's test mode. Navigation reads every device, so a fret held
// to see it light would press the focused setting; while a device is tested
// it plays the test view only. Its buttons, sticks and triggers aren't
// navigation (Start doesn't play, and its bumpers don't switch tabs), and a
// Back pressed on it ends the test. Other devices, the mouse and the keyboard
// navigate as before. The test also ends when its device goes, when the
// Controllers tab stops showing, and at Play. The buttons the device holds as
// its test ends count for navigation only once let go and pressed again, so
// the fret held as Back is pressed doesn't then press the focused row.
class TestMode {
public:
    // tests this device; another one's test ends
    void Begin(uint64_t id);
    void End();
    std::optional<uint64_t> Testing() const { return testing_; }

    // ends the test unless its device is still `connected` and the
    // Controllers tab `showing`; once a frame
    void Follow(std::span<const uint64_t> connected, bool showing);

    // one device's navigation this frame, as NavFromReading read it: nothing
    // from the device under test (whose Back, pressed during the test, ends
    // it), the rest as they are
    NavPad Filter(uint64_t id, const NavPad& pad);

private:
    std::optional<uint64_t> testing_;
    // the tested device's buttons at its last Filter, once it has had one
    uint16_t last_ = 0;
    bool primed_ = false;
    // the last device tested, and the buttons it still holds from then
    std::optional<uint64_t> ended_;
    uint16_t ended_held_ = 0;
};

}
