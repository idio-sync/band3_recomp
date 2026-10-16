#pragma once
#include <functional>
#include <span>
#include <imgui.h>
#include "screen_kit.h"
#include "skin.h"
#include "src/Launcher/instrument_view.h"
#include "src/Launcher/launcher_settings.h"

// The launcher's Test Controllers screen: every controller, instrument and
// MIDI device band3 finds, with RB3's instrument icons and the player each
// feeds, and the selected one drawn live in the panel as it's played. A
// tests one on its own: while it's tested its buttons only light the
// drawing and don't move the menu (a guitar's red fret is B), the others
// dim, and its own Back button (or B and Esc from anything else) stops the test.

namespace band3::launcher::ui {

struct ControllersHost {
    // the rows that open Settings at Controllers and at MIDI
    std::function<void(Category)> settings;
    std::function<void()> back;
    // B's name in the help bar ("Back to menu" without one)
    std::function<std::string()> back_label;
    std::function<void()> play;
};

class ControllersScreen {
public:
    ControllersScreen(Skin& skin, DevicePanel& devices, ControllersHost host);

    void Show();
    // `pads`: the devices' navigation, without the one under test
    void Draw(ImGuiIO& io, std::span<const NavPad> pads);

private:
    void Handle(Action action);
    void Activate(int index);

    Skin& skin_;
    DevicePanel& devices_;
    ControllersHost host_;
    ScreenInput input_;
    // the devices, then the two settings rows
    int selected_ = 0;
    std::vector<DevicePanel::Entry> entries_;
};

}
