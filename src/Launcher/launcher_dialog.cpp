#include "launcher_dialog.h"
#include <imgui.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace band3::launcher {

namespace {

constexpr const char* kQuitPrompt = "Quit without saving?";
constexpr const char* kPlayPrompt = "Play anyway?";

const ImVec4 kBackground(0.07f, 0.07f, 0.09f, 1.0f);
const ImVec4 kGood(0.45f, 0.85f, 0.45f, 1.0f);
const ImVec4 kBad(1.0f, 0.55f, 0.25f, 1.0f);

// a button row's width, for lining it up on the right
float ButtonWidth(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
}

}

LauncherDialog::LauncherDialog(rex::ui::ImGuiDrawer* imgui_drawer, LauncherHost host)
    : rex::ui::ImGuiDialog(imgui_drawer), host_(std::move(host)) {
    // the SDK's ImGui has no navigation; the launcher can be driven from the
    // keyboard until it closes
    ImGuiIO& io = GetIO();
    saved_config_flags_ = io.ConfigFlags;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
}

LauncherDialog::~LauncherDialog() {
    ImGuiIO& io = GetIO();
    io.ConfigFlags = (io.ConfigFlags & ~ImGuiConfigFlags_NavEnableKeyboard) |
                     (saved_config_flags_ & ImGuiConfigFlags_NavEnableKeyboard);
}

bool LauncherDialog::HasUnsavedChanges() const {
    // nothing can be changed yet
    return false;
}

void LauncherDialog::RequestQuit() {
    if (stage_ != Stage::kEditing) return;
    if (HasUnsavedChanges()) {
        open_quit_prompt_ = true;
        return;
    }
    if (host_.quit) host_.quit();
}

void LauncherDialog::OnDraw(ImGuiIO& io) {
    if (stage_ == Stage::kEditing) {
        DrawPage(io);
        return;
    }
    DrawStarting(io);
    // the frame just drawn shows "Starting" while the runtime is built, which
    // holds up the UI thread for a while; the next one starts the game
    if (stage_ == Stage::kStarting && ++starting_frames_ > 1) {
        stage_ = Stage::kStarted;
        if (host_.start_game) host_.start_game();
    }
}

void LauncherDialog::DrawPage(ImGuiIO& io) {
    // covers the window, under any overlay opened from it (F4, the console)
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("band3##launcher", nullptr, flags)) {
        ImGui::TextUnformatted("band3");
        ImGui::SameLine();
        ImGui::TextDisabled("setup");
        ImGui::Separator();
        ImGui::Spacing();
        DrawGameData();
        ImGui::Spacing();
        ImGui::TextDisabled("More settings come here soon; F4 has all of them meanwhile.");
        DrawFooter();
        DrawPrompts();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

void LauncherDialog::DrawGameData() {
    const std::filesystem::path root =
        host_.game_data_root ? host_.game_data_root() : std::filesystem::path();
    if (!check_done_ || root != checked_root_) {
        checked_root_ = root;
        check_ = CheckGameData(root);
        check_done_ = true;
    }
    ImGui::TextUnformatted("Rock Band 3 game data");
    ImGui::Indent();
    const std::string shown = root.empty() ? "(not set)" : rex::path_to_utf8(root);
    ImGui::TextUnformatted(shown.c_str());
    if (check_.ok) {
        ImGui::TextColored(kGood, "Found Rock Band 3 here.");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kBad);
        ImGui::TextWrapped("%s", DescribeProblem(check_.problem));
        ImGui::PopStyleColor();
    }
    ImGui::Unindent();
}

void LauncherDialog::DrawFooter() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float height = ImGui::GetFrameHeight();
    const float bottom = ImGui::GetWindowHeight() - style.WindowPadding.y - height;
    if (ImGui::GetCursorPosY() < bottom) ImGui::SetCursorPosY(bottom);

    const float width = ButtonWidth("Close") + style.ItemSpacing.x + ButtonWidth("Play");
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - style.WindowPadding.x - width);
    if (ImGui::Button("Close")) RequestQuit();
    ImGui::SameLine();
    if (ImGui::Button("Play")) Play();
    // Enter or Space plays once the keyboard has moved to it; Play has it first
    ImGui::SetItemDefaultFocus();
}

void LauncherDialog::DrawPrompts() {
    if (open_quit_prompt_) {
        ImGui::OpenPopup(kQuitPrompt);
        open_quit_prompt_ = false;
    }
    if (open_play_prompt_) {
        ImGui::OpenPopup(kPlayPrompt);
        open_play_prompt_ = false;
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kQuitPrompt, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Your changes haven't been saved.");
        if (ImGui::Button("Quit")) {
            ImGui::CloseCurrentPopup();
            if (host_.quit) host_.quit();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kPlayPrompt, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("The game data folder doesn't look like Rock Band 3's:");
        ImGui::TextUnformatted(DescribeProblem(check_.problem));
        if (ImGui::Button("Play")) {
            ImGui::CloseCurrentPopup();
            Start();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

void LauncherDialog::DrawStarting(ImGuiIO& io) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("band3##launcher_starting", nullptr, flags)) {
        const char* text = "Starting...";
        const ImVec2 size = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - size.x) / 2, (io.DisplaySize.y - size.y) / 2));
        ImGui::TextUnformatted(text);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

void LauncherDialog::Play() {
    if (!check_.ok) {
        open_play_prompt_ = true;
        return;
    }
    Start();
}

bool LauncherDialog::Start() {
    if (stage_ != Stage::kEditing) return false;
    if (!Save()) return false;
    REXLOG_INFO("Launcher: Play");
    stage_ = Stage::kStarting;
    starting_frames_ = 0;
    return true;
}

bool LauncherDialog::Save() {
    return true;
}

bool ShiftHeld() {
#ifdef _WIN32
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
    return false;
#endif
}

}
