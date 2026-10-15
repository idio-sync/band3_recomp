#pragma once
#include <span>
#include <string_view>
#include <vector>
#include <imgui.h>
#include "navigation.h"
#include "skin.h"
#include "theme.h"

// What the redesigned screens share: the 1280x720 page scaled and centred in
// the window, the full-window ImGui window they draw in over the background,
// and their input, read the same way on every screen.

namespace band3::launcher::ui {

// the page this frame: where its 1280x720 layout lands in the window
struct Page {
    ImVec2 origin;
    float scale = 1;
    ImVec2 At(float x, float y) const { return ImVec2(origin.x + x * scale, origin.y + y * scale); }
};
Page PageFor(const ImGuiIO& io);

// Begins the screen's window, covering the window, and draws its background
// (the dimmed game when `over_game`); returns its draw list. EndScreen after.
ImDrawList* BeginScreen(const char* id, const ImGuiIO& io, const Skin& skin, bool over_game = false);
void EndScreen();

// A screen's input: the controllers, then the keyboard (navigation.h's
// actions), and a right click as Back. Whichever was used last picks the
// help bar's hints.
class ScreenInput {
public:
    // input from now on: the press that showed the screen isn't one
    void Show();
    // this frame's actions; once a frame
    std::vector<Action> Read(std::span<const NavPad> pads);
    // keyboard hints in the help bar
    bool Keyboard() const { return keyboard_; }
    // a left click this frame, not the one that showed the screen
    bool Clicked() const;
    bool MouseMoved() const;

private:
    PadActions pads_;
    bool keyboard_ = false;
    int shown_frame_ = -1;
};

// every controller's buttons together, and the stick pushed furthest
NavPad CombinedPads(std::span<const NavPad> pads);
// seconds on a steady clock, for the input's repeats
double NowSeconds();

}
