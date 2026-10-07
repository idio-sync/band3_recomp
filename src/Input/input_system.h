#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <rex/system/interfaces/input.h>
#include "device_kind.h"
#include "instruments.h"

namespace rex::input {
class InputSystem;
}
namespace rex::ui {
class Window;
}

namespace band3::input {

// The SDK's input system (SDL or XInput, plus keyboard/mouse) with band3's
// drivers added: the Instrument Lab's virtual instrument and, with the
// hid_instruments and midi_drums settings on, instrument dongles and MIDI drum
// kits. Player slots make room for the
// virtual instrument. For RuntimeConfig::input_factory: hands over the one
// PrepareInputSystem built for the launcher, or builds one.
std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode);

// the input system CreateInputSystem made for the game, for hooks that read a
// player's state directly; null before then
rex::input::InputSystem* GameInputSystem();

// The SDK's AddUIInputBlocker and RemoveUIInputBlocker, for band3's dialogs
// that take the controller from the game, counted so GameInputBlocked can say
// so: the mouse's presses (mouse_menus_driver.h) join the game's reads after
// the SDK has blanked them, so they have to know to stay out too.
void AddGameInputBlocker(rex::input::InputSystem& system);
void RemoveGameInputBlocker(rex::input::InputSystem& system);
bool GameInputBlocked();

// The launcher's devices (Band3App::OnFinalizePaths). The launcher reads the
// input system the game will get, built before the runtime, so it lists and
// tests exactly what the game will see; the runtime then takes it over
// (CreateInputSystem). All of these run on the UI thread, before the game
// starts, and take InputLock() themselves.
//
// The system is built once and never destroyed or replaced: the SDK's SDL
// driver leaves its SDL event watch and window listener behind when destroyed,
// so destroying one that has seen the window would crash at the next gamepad
// or window event, and one kept aside would queue every gamepad event for the
// rest of the run. hid_instruments and midi_drums* restart band3's own drivers
// inside it; an input_backend change applies only when band3 starts again,
// which the launcher's Play does for it (InputBackendChanged).

// Builds the input system for the settings as they are and shows it the
// window, so the SDL driver opens the gamepads (it starts SDL's gamepad
// subsystem only then) and keyboard input is read. The runtime shows it the
// window again, which changes nothing: the SDL driver ignores a second window,
// and the window takes the keyboard driver's listeners only once.
void PrepareInputSystem(rex::ui::Window* window);

// Follows the input settings the launcher may have changed (hid_instruments,
// midi_drums*): starts or stops the HID and MIDI drivers. The MIDI driver
// restarts only for its port, notes or midi_drums itself, once they have
// stayed the same for a moment (so clicking through ports opens the kit once);
// it follows its other settings (the Minimum velocity slider) as it runs.
// Cheap when nothing changed, so call it every frame.
void ApplyInputSettings();

// input_backend names another backend than the launcher's input system uses
// (as the SDK reads it: anything but "xinput" on Windows is SDL), so it
// applies only once band3 restarts. False once the game has the system.
bool InputBackendChanged();

// Play: applies the settings a last time and forgets which players the
// launcher saw connected, so the game's first reads announce them as at a
// start without the launcher (the SDK tells the game when a player connects,
// which it can't do before the runtime exists). The devices stay open for the
// game; ReadInputCaps and ReadInputState refuse from here on.
void ReadyInputForGame();

struct InputDevice {
    uint64_t id = 0;
    std::string name;
    std::string guid;
    DeviceKind kind = DeviceKind::kPad;
    // 1-4, or 0 when it feeds no player
    int player = 0;
};

// Every device the input system has, in connection order, with the player
// each feeds: the player assignment's last view, taken whenever the devices
// change. Before the game (on the UI thread) it looks for devices first,
// since nothing else asks the input system yet; once the game runs, any thread.
std::vector<InputDevice> PlayerDevices();

// The subtype the game is to read for `user`'s device, which the input system
// reported as `reported`: a guitar becomes the kind guitar_type says, as it
// was when the game started (guitar_type.h). Anything else is left alone.
uint8_t GameGuitarSubtype(uint32_t user, uint8_t reported);

// One device's capabilities, or its state, as the game would read them on its
// player; nullopt once it's gone, or once the game has the input system. Read
// on a player the device doesn't feed, so the read can't change which of a
// player's devices the game reads first. Every read makes the SDK look for
// devices again, so read the capabilities when the device list is taken, and
// only the state every frame.
std::optional<Caps360> ReadInputCaps(uint64_t id);
std::optional<Gamepad360> ReadInputState(uint64_t id);

}
