#pragma once
#include <cstdint>
#include <string>
#include <rex/ui/imgui_dialog.h>
#include "liveless_rooms_protocol.h"

namespace band3::rooms {

struct Status;

// The Liveless Rooms panel (bind_liveless_rooms, F10, and the overshell's
// Invite Friends, PlatformMgr::ShowFriendsUI, while Rooms is on): this
// player's code to give out, and a
// field and a grid of the codes' characters to join another's by, with the
// Rooms server's state and its last error. With Rooms off it says how to turn
// it on.
//
// While it's open and focused it takes the controllers: they move around it
// with ImGui's gamepad navigation, and the game reads a neutral pad (the SDK's
// UI input blocker), so a player at the overshell with only a controller can
// enter a code.
class RoomsPanelDialog : public rex::ui::ImGuiDialog {
public:
    explicit RoomsPanelDialog(rex::ui::ImGuiDrawer* imgui_drawer)
        : rex::ui::ImGuiDialog(imgui_drawer) {}

    void Toggle();

protected:
    void OnDraw(ImGuiIO& io) override;

private:
    void Show();
    void DrawOff();
    void DrawRooms(const Status& status);
    void DrawPicker(bool can_join);
    void TryJoin();
    void AddChar(char c);
    void Backspace();

    // the controllers, while the panel has them
    void TakePad(ImGuiIO& io);
    void ReleasePad(ImGuiIO& io);

    bool visible_ = false;
    // focus the window the next time it draws: it was just opened
    bool focus_next_ = false;
    char code_[kMaxCodeLength + 1] = {};
    // what the last Join or Connect said at once (the server's answer comes
    // in Status::error)
    std::string request_error_;
    std::string requested_code_;

    // holding the SDK's input blocker and feeding ImGui the pads
    bool pad_taken_ = false;
    // the ImGui flags the panel turned on, to turn off again
    int config_flags_added_ = 0;
    int backend_flags_added_ = 0;
    // buttons fed to ImGui as down
    uint16_t pad_buttons_ = 0;
    // buttons held when the panel took the pads, left out until let go, so
    // the press that opened it (the Friends button's A) doesn't press a key
    uint16_t pad_ignored_ = 0;
};

}  // namespace band3::rooms
