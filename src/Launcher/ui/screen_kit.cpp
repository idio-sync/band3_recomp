#include "screen_kit.h"
#include <chrono>
#include <cmath>
#include "keyboard_actions.h"
#include "src/Launcher/launcher_style.h"
#include "widgets.h"

namespace band3::launcher::ui {

Page PageFor(const ImGuiIO& io) {
    const float s = Scale();
    return Page{ImVec2(std::round((io.DisplaySize.x - 1280 * s) * 0.5f),
                       std::round((io.DisplaySize.y - 720 * s) * 0.5f)),
                s};
}

ImDrawList* BeginScreen(const char* id, const ImGuiIO& io, const Skin& skin, bool over_game) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground;
    ImGui::Begin(id, nullptr, flags);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (over_game) {
        DrawGameDim(dl, ImVec2(0, 0), io.DisplaySize, skin);
    } else {
        DrawBackground(dl, ImVec2(0, 0), io.DisplaySize, skin);
    }
    return dl;
}

void EndScreen() { ImGui::End(); }

void ScreenInput::Show() {
    pads_ = PadActions{};
    shown_frame_ = ImGui::GetFrameCount();
}

std::vector<Action> ScreenInput::Read(std::span<const NavPad> pads) {
    std::vector<Action> out = pads_.Step(CombinedPads(pads), NowSeconds());
    if (!out.empty()) keyboard_ = false;
    // the keys that showed it were this frame's
    if (ImGui::GetFrameCount() <= shown_frame_ + 1) return out;
    const std::vector<Action> keys = KeyboardActions();
    if (!keys.empty()) keyboard_ = true;
    out.insert(out.end(), keys.begin(), keys.end());
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) out.push_back(Action::kBack);
    return out;
}

bool ScreenInput::Clicked() const {
    return ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetFrameCount() > shown_frame_;
}

bool ScreenInput::MouseMoved() const {
    const ImGuiIO& io = ImGui::GetIO();
    return io.MouseDelta.x != 0 || io.MouseDelta.y != 0;
}

NavPad CombinedPads(std::span<const NavPad> pads) {
    NavPad out;
    for (const NavPad& p : pads) {
        out.buttons |= p.buttons;
        if (std::abs(p.left_x) > std::abs(out.left_x)) out.left_x = p.left_x;
        if (std::abs(p.left_y) > std::abs(out.left_y)) out.left_y = p.left_y;
    }
    return out;
}

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}
