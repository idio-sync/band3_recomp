#pragma once
#include <functional>
#include <rex/ui/imgui_dialog.h>
#include "menu_shortcut.h"

namespace band3::input {

// Watches the controllers for the menu shortcut (menu_shortcut.h) once a frame
// and passes each chord to on_action. Draws nothing. Reads the buttons the game
// last read from each player (ChordPads), so it works with either input backend.
// on_action runs while ImGui is drawing, so anything that adds or removes a
// dialog has to be deferred.
class MenuShortcutDialog : public rex::ui::ImGuiDialog {
public:
    MenuShortcutDialog(rex::ui::ImGuiDrawer* imgui_drawer,
                       std::function<void(MenuShortcutAction)> on_action)
        : rex::ui::ImGuiDialog(imgui_drawer), on_action_(std::move(on_action)) {}

protected:
    void OnDraw(ImGuiIO& io) override;

private:
    std::function<void(MenuShortcutAction)> on_action_;
    MenuShortcut shortcut_;
};

}
