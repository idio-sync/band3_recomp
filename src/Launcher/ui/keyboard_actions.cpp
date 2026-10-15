#include "keyboard_actions.h"
#include <imgui.h>

namespace band3::launcher::ui {

std::vector<Action> KeyboardActions() {
    std::vector<Action> out;
    const auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
    if (pressed(ImGuiKey_UpArrow)) out.push_back(Action::kUp);
    if (pressed(ImGuiKey_DownArrow)) out.push_back(Action::kDown);
    if (pressed(ImGuiKey_LeftArrow)) out.push_back(Action::kLeft);
    if (pressed(ImGuiKey_RightArrow)) out.push_back(Action::kRight);
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        out.push_back(Action::kSelect);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
        out.push_back(Action::kBack);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Slash, false)) out.push_back(Action::kSecondary);
    if (ImGui::IsKeyPressed(ImGuiKey_R, false) && !ImGui::GetIO().KeyCtrl) out.push_back(Action::kReset);
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        out.push_back(ImGui::GetIO().KeyShift ? Action::kPreviousCategory : Action::kNextCategory);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) out.push_back(Action::kPlay);
    return out;
}

}
