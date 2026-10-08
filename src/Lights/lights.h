#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "lights_view.h"
#include "stagekit.h"

// The Stage Kits band3 reaches itself: those plugged into this PC by USB
// (Santroller kits in HID mode, Xbox 360 kits through XInput), lit as the game
// lights them, and the RB3E Dashboard's Pico W wireless kits, found on the
// network and tested from the Lights tab (they follow the game through the
// RB3Enhanced events, src/Net/events.h). Starts with the launcher, so the tab
// works before the game, and follows stagekit_usb and pico_discovery as they
// change. Each side runs on a thread of its own; these calls only queue work.
namespace band3::lights {

void Start();
// turns every kit's lights off, and stops; at exit
void Stop();

// JoypadStageKitSetRaw(left, right), from the game thread
void NoteGame(uint8_t left, uint8_t right);

// the Lights tab's
std::vector<DeviceRow> Devices();
// whether to say the Picos found need the RB3Enhanced events turned on
bool PicosNeedEvents();
// why the Picos can't be found, if they can't
std::string PicoProblem();
// a test command for a device (by its row's key), or for every device
void SendTest(Command command, const std::optional<std::string>& key);
// plays a scene (ColourCheck, Chase) on a device or all of them, in place of
// one already playing
void PlayScene(std::span<const Step> steps, const std::optional<std::string>& key);
// the last test command in words, and where it went; empty before one
std::string LastSent();

// the commands the pretend kit (stagekit_fake) has been sent since the last
// call, oldest first, for the test harness
std::vector<Command> TakeFakeCommands();

}  // namespace band3::lights
