#pragma once
#include <span>
#include <imgui.h>
#include "device_view_model.h"

// Gamepad navigation for the launcher. The SDK's ImGui has no gamepad
// navigation, and its input system isn't ImGui's, so the launcher turns
// ImGui's on and feeds it every pad's state itself, read through the input
// system (DevicePanel's readings, on a player each device doesn't feed, so
// the launcher's reads leave the game's players as they were).

namespace band3::launcher {

// whether the keyboard or a controller's focus is on an item inside this
// rectangle (screen space) of the current window, such as a settings row's
// controls, so the footer can describe the row the player has moved to
bool NavCursorWithin(const ImVec2& min, const ImVec2& max);

class GamepadNav {
public:
    // turns ImGui's gamepad navigation on, until Stop or destruction
    explicit GamepadNav(ImGuiIO& io);
    ~GamepadNav() { Stop(); }
    GamepadNav(const GamepadNav&) = delete;
    GamepadNav& operator=(const GamepadNav&) = delete;

    // This frame's devices, combined, as ImGui's gamepad keys (they reach
    // ImGui at its next frame). Returns the launcher's own buttons pressed
    // since the last frame: Start and the bumpers, which ImGui isn't given.
    // Buttons already held at the first frame count as pressed only once let
    // go and pressed again.
    NavEdges Feed(std::span<const NavPad> pads);

    // lets go of every key it holds and gives ImGui's flags back as they
    // were; Feed does nothing after it
    void Stop();

private:
    ImGuiIO* io_;
    bool stopped_ = false;
    bool primed_ = false;
    // the state ImGui was last given
    NavPad last_;
    bool had_pad_ = false;
    // the flags' bits before the launcher set them
    bool saved_nav_gamepad_ = false;
    bool saved_has_gamepad_ = false;
};

}
