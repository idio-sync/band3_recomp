#include "pause_screen.h"
#include <chrono>
#include <cmath>
#include "keyboard_actions.h"
#include "src/Launcher/launcher_style.h"
#include "widgets.h"

namespace band3::launcher::ui {

namespace {

constexpr const char* kItems[] = {"Resume", "Settings", "Instrument Lab", "Quit Game"};
constexpr int kCount = int(std::size(kItems));

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

NavPad Combined(std::span<const NavPad> pads) {
    NavPad out;
    for (const NavPad& p : pads) {
        out.buttons |= p.buttons;
        if (std::abs(p.left_x) > std::abs(out.left_x)) out.left_x = p.left_x;
        if (std::abs(p.left_y) > std::abs(out.left_y)) out.left_y = p.left_y;
    }
    return out;
}

}

PauseScreen::PauseScreen(Skin& skin, PauseHost host) : skin_(skin), host_(std::move(host)) {}

void PauseScreen::Show() {
    selected_ = 0;
    box_.reset();
    pads_ = PadActions{};
    shown_frame_ = ImGui::GetFrameCount();
}

void PauseScreen::Back() { Handle(Action::kBack); }

void PauseScreen::Handle(Action action) {
    if (box_) {
        if (const auto picked = StepBox(*box_, action)) {
            const auto chosen = std::move(box_->chosen);
            box_.reset();
            if (chosen) chosen(*picked);
        }
        return;
    }
    switch (action) {
    case Action::kUp:
    case Action::kDown:
        selected_ = MoveSelection(selected_, action == Action::kUp ? -1 : 1, kCount, true);
        break;
    case Action::kSelect: Pick(selected_); break;
    // Start, as on a console, and B go back to the game
    case Action::kBack:
    case Action::kPlay:
        if (host_.resume) host_.resume();
        break;
    default: break;
    }
}

void PauseScreen::Pick(int item) {
    switch (item) {
    case 0:
        if (host_.resume) host_.resume();
        break;
    case 1:
        if (host_.settings) host_.settings();
        break;
    case 2:
        if (host_.instrument_lab) host_.instrument_lab();
        break;
    case 3: {
        Box box{.title = "Quit Rock Band 3?", .options = {"Quit", "Cancel"}, .selected = 1, .cancel = 1,
                .chosen = [this](int choice) {
                    if (choice == 0 && host_.quit) host_.quit();
                }};
        if (host_.in_song && host_.in_song()) box.lines.push_back("The song you're playing isn't kept.");
        if (host_.unsaved_settings && host_.unsaved_settings()) {
            box.lines.push_back("Your settings changes aren't saved.");
        }
        box_ = std::move(box);
        shown_frame_ = ImGui::GetFrameCount();
        break;
    }
    default: break;
    }
}

void PauseScreen::Draw(ImGuiIO& io, std::span<const NavPad> pads) {
    const bool first = ImGui::GetFrameCount() <= shown_frame_ + 1;
    const std::vector<Action> from_pads = pads_.Step(Combined(pads), Now());
    if (!from_pads.empty()) keyboard_hints_ = false;
    const std::vector<Action> from_keys = first ? std::vector<Action>{} : KeyboardActions();
    if (!from_keys.empty()) keyboard_hints_ = true;
    for (const Action a : from_pads) Handle(a);
    for (const Action a : from_keys) Handle(a);
    if (!first && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) Handle(Action::kBack);

    const float s = Scale();
    const ImVec2 origin(std::round((io.DisplaySize.x - 1280 * s) * 0.5f),
                        std::round((io.DisplaySize.y - 720 * s) * 0.5f));
    const auto at = [&](float x, float y) { return ImVec2(origin.x + x * s, origin.y + y * s); };
    const bool moved = io.MouseDelta.x != 0 || io.MouseDelta.y != 0;
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !first;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground;
    if (ImGui::Begin("band3##pause_screen", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // the game, darkened
        dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 158));
        const ImVec2 min = at(470, 220), max = at(810, 220 + 40 + 44 * kCount + 22);
        DrawPanel(dl, min, max, skin_);
        const TextStyle style = kCaps(34);
        for (int i = 0; i < kCount; i++) {
            const ImVec2 imin = at(482, 260 + 44.0f * i), imax = at(798, 260 + 44.0f * i + 42);
            const bool selected = i == selected_ && !box_;
            if (selected) DrawHighlight(dl, imin, imax, skin_);
            const float w = TextWidth(style, kItems[i]);
            PaintText(dl, ImVec2((imin.x + imax.x - w) * 0.5f, imin.y + (imax.y - imin.y - Px(34)) * 0.5f), style,
                      selected ? color::kOnYellow : color::kText, kItems[i]);
            if (!box_ && ImGui::IsMouseHoveringRect(imin, imax)) {
                if (moved) selected_ = i;
                if (clicked) Pick(i);
            }
        }
        const HelpItem help[] = {{HelpButton::kA, "Select"}, {HelpButton::kB, box_ ? "Back" : "Resume"}};
        const int hovered = DrawHelpBar(dl, ImVec2(0, at(0, 664).y), io.DisplaySize, help, keyboard_hints_,
                                        false, skin_);
        if (box_) {
            const int option = DrawBox(dl, ImVec2(0, 0), io.DisplaySize, *box_, skin_);
            if (option >= 0 && moved) box_->selected = option;
            // not the click that opened it
            if (option >= 0 && clicked && ImGui::GetFrameCount() != shown_frame_) {
                box_->selected = option;
                Handle(Action::kSelect);
            }
        } else if (hovered >= 0 && clicked) {
            Handle(hovered == 0 ? Action::kSelect : Action::kBack);
        }
    }
    ImGui::End();
}

}
