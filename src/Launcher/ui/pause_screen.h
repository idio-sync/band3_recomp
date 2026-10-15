#pragma once
#include <functional>
#include <optional>
#include <span>
#include <imgui.h>
#include "box.h"
#include "navigation.h"
#include "skin.h"

// band3's pause menu in game, as Rock Band 3's: its pause container over the
// darkened game, with Resume, Settings, Instrument Lab and Quit Game in caps
// (Quit Game asks first). Driven by controllers, the keyboard and the mouse.

namespace band3::launcher::ui {

struct PauseHost {
    std::function<void()> resume;
    std::function<void()> settings;
    std::function<void()> instrument_lab;
    std::function<void()> quit;
    // what quitting loses, for the box that asks: a song playing, unsaved settings
    std::function<bool()> in_song;
    std::function<bool()> unsaved_settings;
};

class PauseScreen {
public:
    PauseScreen(Skin& skin, PauseHost host);

    // shown again: Resume selected, and the press that opened it isn't one
    void Show();
    void Draw(ImGuiIO& io, std::span<const NavPad> pads);
    // B or the pause bind: back to the game, or out of the box
    void Back();

private:
    void Handle(Action action);
    void Pick(int item);

    Skin& skin_;
    PauseHost host_;
    PadActions pads_;
    int selected_ = 0;
    std::optional<Box> box_;
    bool keyboard_hints_ = false;
    // the frame it was shown in: that frame's keys opened it
    int shown_frame_ = -1;
};

}
