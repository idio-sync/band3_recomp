#include "menu_shortcut_dialog.h"
#include "src/settings.h"

namespace band3::input {

void MenuShortcutDialog::OnDraw(ImGuiIO& io) {
    (void)io;
    const auto now = MenuShortcut::Clock::now();
    const uint16_t buttons = REXCVAR_GET(menu_shortcut) ? GameChordPads().Held(now) : 0;
    const MenuShortcutAction action = shortcut_.Update(buttons, now);
    if (action != MenuShortcutAction::kNone && on_action_) on_action_(action);
}

}
