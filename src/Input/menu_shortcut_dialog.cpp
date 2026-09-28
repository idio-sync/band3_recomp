#include "menu_shortcut_dialog.h"
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_stdinc.h>
#include "instruments.h"
#include "src/settings.h"

namespace band3::input {

namespace {

// the chord's buttons held on any pad the SDK has open. The joystick lock keeps
// the SDK from closing a pad between finding it and reading it.
uint16_t HeldChordButtons() {
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) return 0;

    uint16_t buttons = 0;
    SDL_LockJoysticks();
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; ids && i < count; i++) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (!pad) continue;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_STICK)) buttons |= xbox::kLeftThumb;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) buttons |= xbox::kRightThumb;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) {
            buttons |= xbox::kLeftShoulder;
        }
    }
    SDL_free(ids);
    SDL_UnlockJoysticks();
    return buttons;
}

}

void MenuShortcutDialog::OnDraw(ImGuiIO& io) {
    (void)io;
    const uint16_t buttons = REXCVAR_GET(menu_shortcut) ? HeldChordButtons() : 0;
    const MenuShortcutAction action = shortcut_.Update(buttons, MenuShortcut::Clock::now());
    if (action != MenuShortcutAction::kNone && on_action_) on_action_(action);
}

}
