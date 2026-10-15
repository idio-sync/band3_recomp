#pragma once
#include <span>
#include <string>
#include <vector>
#include <imgui.h>
#include "navigation.h"
#include "skin.h"

// A page of every widget of the redesigned launcher (widgets.h), for checking
// the look and the navigation before the screens use them: a menu and a list
// of rows to move through with a controller, the keyboard or the mouse, a
// framed panel with text, chips, key caps and icons, and the help bar. F
// swaps between the game's textures and the plain look drawn without them; K
// swaps the help bar's controller and keyboard hints. The launcher shows it
// in place of its page with Ctrl+Shift+G, or from the start with the
// environment variable BAND3_LAUNCHER_GALLERY=1.

namespace band3::launcher::ui {

class WidgetGallery {
public:
    explicit WidgetGallery(Skin& skin);

    // the page, covering the window; `pads` this frame's controllers
    void Draw(ImGuiIO& io, std::span<const NavPad> pads);

private:
    void Handle(Action action);

    Skin& skin_;
    // a skin that never loads: the plain look
    Skin plain_skin_{nullptr};
    bool plain_ = false;
    bool keyboard_ = false;
    PadActions pad_;
    // 0: the menu, 1: the rows
    int column_ = 0;
    int menu_ = 0;
    int row_ = 0;
    std::vector<bool> changed_;
    std::string last_action_;
};

}
