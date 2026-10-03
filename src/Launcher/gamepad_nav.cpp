#include "gamepad_nav.h"
#include <imgui_internal.h>
#include <cmath>
#include <utility>

namespace band3::launcher {

namespace xbox = input::xbox;

namespace {

// The buttons ImGui is given. X isn't: holding ImGui's "menu" button opens
// its window switcher, which the launcher has no use for, and a guitar's blue
// fret is X. LB and RB aren't either: they switch the launcher's tabs, and
// ImGui would also take them to change how fast a slider moves.
constexpr std::pair<uint16_t, ImGuiKey> kButtons[] = {
    {xbox::kButtonA, ImGuiKey_GamepadFaceDown},
    {xbox::kButtonB, ImGuiKey_GamepadFaceRight},
    {xbox::kButtonY, ImGuiKey_GamepadFaceUp},
    {xbox::kStart, ImGuiKey_GamepadStart},
    {xbox::kBack, ImGuiKey_GamepadBack},
    {xbox::kDpadUp, ImGuiKey_GamepadDpadUp},
    {xbox::kDpadDown, ImGuiKey_GamepadDpadDown},
    {xbox::kDpadLeft, ImGuiKey_GamepadDpadLeft},
    {xbox::kDpadRight, ImGuiKey_GamepadDpadRight},
    {xbox::kLeftThumb, ImGuiKey_GamepadL3},
    {xbox::kRightThumb, ImGuiKey_GamepadR3},
};

// the analog keys, each one direction of an axis
struct Analog {
    ImGuiKey key;
    float NavPad::*axis;
    float sign;
};
constexpr Analog kAnalogs[] = {
    {ImGuiKey_GamepadL2, &NavPad::left_trigger, 1},
    {ImGuiKey_GamepadR2, &NavPad::right_trigger, 1},
    {ImGuiKey_GamepadLStickLeft, &NavPad::left_x, -1},
    {ImGuiKey_GamepadLStickRight, &NavPad::left_x, 1},
    {ImGuiKey_GamepadLStickUp, &NavPad::left_y, 1},
    {ImGuiKey_GamepadLStickDown, &NavPad::left_y, -1},
    {ImGuiKey_GamepadRStickLeft, &NavPad::right_x, -1},
    {ImGuiKey_GamepadRStickRight, &NavPad::right_x, 1},
    {ImGuiKey_GamepadRStickUp, &NavPad::right_y, 1},
    {ImGuiKey_GamepadRStickDown, &NavPad::right_y, -1},
};

float AnalogValue(const NavPad& pad, const Analog& a) {
    const float v = pad.*a.axis * a.sign;
    return v > 0 ? v : 0.0f;
}

}

bool NavCursorWithin(const ImVec2& min, const ImVec2& max) {
    // (the SDK's DLL doesn't export GImGui, which ImGui's inline helpers use)
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* window = g.NavWindow;
    if (!g.NavCursorVisible || g.NavId == 0 || window != g.CurrentWindow) return false;
    const ImRect nav = ImGui::WindowRectRelToAbs(window, window->NavRectRel[g.NavLayer]);
    return ImRect(min, max).Contains(nav.GetCenter());
}

GamepadNav::GamepadNav(ImGuiIO& io) : io_(&io) {
    saved_nav_gamepad_ = (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0;
    saved_has_gamepad_ = (io.BackendFlags & ImGuiBackendFlags_HasGamepad) != 0;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
}

NavEdges GamepadNav::Feed(std::span<const NavPad> pads) {
    if (stopped_) return {};
    const NavPad now = CombinePads(pads);
    // ImGui reads gamepad keys only while the backend says it has a pad
    const bool has_pad = !pads.empty();
    if (has_pad != had_pad_) {
        if (has_pad) {
            io_->BackendFlags |= ImGuiBackendFlags_HasGamepad;
        } else if (!saved_has_gamepad_) {
            io_->BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
        }
        had_pad_ = has_pad;
    }

    for (const auto& [bit, key] : kButtons) {
        const bool down = (now.buttons & bit) != 0;
        if (down != ((last_.buttons & bit) != 0)) io_->AddKeyEvent(key, down);
    }
    for (const Analog& a : kAnalogs) {
        const float v = AnalogValue(now, a);
        if (v != AnalogValue(last_, a)) io_->AddKeyAnalogEvent(a.key, v > 0.1f, v);
    }

    // what was held when the launcher first read the pads isn't a press
    const NavEdges edges = PressedEdges(primed_ ? last_.buttons : now.buttons, now.buttons);
    primed_ = true;
    last_ = now;
    return edges;
}

void GamepadNav::Stop() {
    if (stopped_) return;
    Feed({});
    stopped_ = true;
    if (!saved_nav_gamepad_) io_->ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    if (saved_has_gamepad_) {
        io_->BackendFlags |= ImGuiBackendFlags_HasGamepad;
    } else {
        io_->BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
    }
}

}
