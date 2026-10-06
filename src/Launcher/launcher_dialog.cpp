#include "launcher_dialog.h"
#include <imgui.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <algorithm>
#include <string>
#include <thread>
#include <utility>
#include "src/Input/input_system.h"
#include "src/settings.h"
#include "launcher_start.h"
#include "launcher_style.h"

namespace band3::launcher {

namespace {

constexpr const char* kQuitPrompt = "Quit without saving?";
constexpr const char* kPlayPrompt = "Play anyway?";
constexpr const char* kSaveFailedPrompt = "Settings not saved";

constexpr std::pair<Tab, const char*> kTabs[] = {
    {Tab::kGame, "Game"},
    {Tab::kGraphics, "Graphics"},
    {Tab::kAudio, "Audio"},
    {Tab::kControllers, "Controllers"},
    {Tab::kOnline, "Online"},
};

}

LauncherDialog::LauncherDialog(rex::ui::ImGuiDrawer* imgui_drawer, LauncherHost host)
    : rex::ui::ImGuiDialog(imgui_drawer),
      host_(std::move(host)),
      model_(SettingTable(),
             ReadEnvironment(SettingTable(), host_.path_defaults, host_.anchor, Where::kLauncher),
             store_),
      page_(model_, Where::kLauncher,
            PageHost{.game_data_root = host_.game_data_root, .native_window = host_.native_window}) {
    // the SDK's ImGui has no navigation; the launcher can be driven from the
    // keyboard and controllers until it closes
    ImGuiIO& io = GetIO();
    saved_config_flags_ = io.ConfigFlags;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    nav_.emplace(io);
    config_problem_ = ConfigFileProblem(host_.config_path);
    if (config_problem_) {
        REXLOG_WARN("Launcher: {} couldn't be read: {}", rex::path_to_utf8(host_.config_path),
                    *config_problem_);
    }
}

LauncherDialog::~LauncherDialog() {
    page_.CloseMeters();
    nav_.reset();
    ImGuiIO& io = GetIO();
    io.ConfigFlags = (io.ConfigFlags & ~ImGuiConfigFlags_NavEnableKeyboard) |
                     (saved_config_flags_ & ImGuiConfigFlags_NavEnableKeyboard);
}

bool LauncherDialog::HasUnsavedChanges() const { return model_.HasUnsavedChanges(); }

void LauncherDialog::RequestQuit() {
    if (stage_ != Stage::kEditing) return;
    if (HasUnsavedChanges()) {
        open_quit_prompt_ = true;
        return;
    }
    if (host_.quit) host_.quit();
}

void LauncherDialog::Pace() {
    using Clock = std::chrono::steady_clock;
    Clock::time_point now = Clock::now();
    if (now >= next_refresh_query_) {
        const double hz = host_.refresh_rate ? host_.refresh_rate() : 0.0;
        refresh_hz_ = hz >= 24.0 ? std::min(hz, 360.0) : 60.0;
        next_refresh_query_ = now + std::chrono::seconds(1);
    }
    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / refresh_hz_));
    const Clock::time_point due = last_frame_ + period;
    if (now < due) {
        std::this_thread::sleep_until(due);
        // from when it was due, so oversleeping doesn't slow the next frame
        last_frame_ = due;
    } else {
        last_frame_ = now;
    }
}

void LauncherDialog::OnDraw(ImGuiIO& io) {
    // before the game runs nothing else holds the frame rate down: the SDK
    // waits for the display's vblank only once it presents the game, and not
    // at all without a display (a disconnected remote session)
    Pace();

    ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiStyle sdk_style = style;
    SetScale(ScaleFor(io.DisplaySize));
    ApplyStyle(style, Scale());
    {
        FontScope font(kBodySize);
        if (stage_ == Stage::kEditing) {
            page_.BeginFrame();
            // the instrument settings restart band3's drivers as they change
            input::ApplyInputSettings();
            // the devices are read only until Play: then they're the game's
            device_panel_.Poll(current_tab_ == Tab::kControllers);
            HandleNav(nav_->Feed(device_panel_.NavPads()));
            DrawPage(io);
            page_.EndFrame();
        } else {
            // nothing of the launcher's records once Play is pressed: the
            // game's own capture opens the microphones
            page_.CloseMeters();
            device_panel_.StopTest();
            nav_->Stop();
            DrawStarting(io);
        }
    }
    style = sdk_style;

    // the frame just drawn shows "Starting" while the runtime is built, which
    // holds up the UI thread for a while; the next one starts the game
    if (stage_ == Stage::kStarting && ++starting_frames_ > 1) {
        stage_ = Stage::kStarted;
        if (host_.start_game) host_.start_game(restart_);
    }
}

void LauncherDialog::DrawPage(ImGuiIO& io) {
    // covers the window, under any overlay opened from it (F4, the console)
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("band3##launcher", nullptr, flags)) {
        DrawHeader();
        DrawBanners();

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(18), Px(8)));
        if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTooltip)) {
            for (const auto& [tab, name] : kTabs) {
                const ImGuiTabItemFlags select =
                    pending_tab_ == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                if (ImGui::BeginTabItem(name, nullptr, select)) {
                    current_tab_ = tab;
                    ImGui::EndTabItem();
                }
            }
            pending_tab_.reset();
            ImGui::EndTabBar();
        }
        ImGui::PopStyleVar();
        page_.RefreshDeviceLists(current_tab_);
        page_.DrawDeckBanner();
        ImGui::Dummy(ImVec2(0, Px(2)));

        // the footer keeps its place at the bottom; the settings scroll above it
        const ImGuiStyle& style = ImGui::GetStyle();
        const float footer_height = Px(8) + SettingsPage::DescriptionHeight() +
                                    ImGui::GetFrameHeight() +
                                    style.ItemSpacing.y * 4;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        // (in a window too short for both, the settings keep a few rows)
        const float settings_height =
            std::max(ImGui::GetContentRegionAvail().y - footer_height, Px(80));
        // (flattened: the keyboard and controllers move between the settings
        // and the footer as if they were one page)
        if (ImGui::BeginChild("##settings", ImVec2(0, settings_height),
                              ImGuiChildFlags_NavFlattened)) {
            ImGui::Dummy(ImVec2(0, Px(4)));
            if (current_tab_ == Tab::kControllers) device_panel_.Draw();
            page_.DrawSections(current_tab_);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        DrawFooter();
        DrawPrompts();
    }
    ImGui::End();
}

void LauncherDialog::DrawHeader() {
    {
        FontScope font(kTitleSize);
        ImGui::TextUnformatted("band3");
    }
    ImGui::SameLine();
    {
        // on the title's baseline
        FontScope font(kHeadingSize);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(kTitleSize - kHeadingSize) * 0.8f);
        ImGui::TextColored(kMuted, "setup");
    }
    // where the settings go, on the right, if there's room
    const float title_end = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    FontScope font(kSmallSize);
    const std::string where = "Saves to " + rex::path_to_utf8(host_.config_path);
    const float width = ImGui::CalcTextSize(where.c_str()).x;
    const float right = ImGui::GetWindowContentRegionMax().x;
    if (right - width > title_end + Px(40)) {
        ImGui::SameLine(right - width);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(kTitleSize - kSmallSize) * 0.8f);
        ImGui::TextColored(kMuted, "%s", where.c_str());
    }
    ImGui::Dummy(ImVec2(0, Px(2)));
}

void LauncherDialog::DrawBanners() {
    if (!config_problem_) return;
    Banner("##config_problem", kProblemBanner, [&] {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(kWarn, "band3.toml couldn't be read:");
        ImGui::SameLine();
        ImGui::TextUnformatted(config_problem_->c_str());
        ImGui::TextUnformatted(
            "Saving replaces it, and the old file is kept as band3.toml.bak.");
        ImGui::PopTextWrapPos();
    });
    ImGui::Spacing();
}


void LauncherDialog::DrawFooter() {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (!ImGui::BeginChild("##footer", ImVec2(0, 0), ImGuiChildFlags_NavFlattened,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }
    const ImVec2 top = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(top, ImVec2(top.x + ImGui::GetContentRegionAvail().x, top.y),
                                        ImGui::GetColorU32(kLine), std::max(1.0f, Px(1)));
    ImGui::Dummy(ImVec2(0, Px(8)));

    page_.DrawDescription([this] {
        ImGui::TextColored(kMuted, "Point at a setting to see what it does. Changes apply as you "
                                   "make them; Save keeps them for next time.");
        if (!device_panel_.NavPads().empty()) {
            ImGui::TextColored(kMuted, "With a controller: LB and RB switch tabs, Start plays.%s",
                               current_tab_ == Tab::kControllers
                                   ? " A on a device tests it; Back stops."
                                   : "");
        }
    });

    // the bottom row: the startup box, what saving did, and the buttons
    page_.DrawStartupBox();

    const float play_padding = Px(28);
    const float buttons = ButtonWidth("Save") + ButtonWidth("Close") + ButtonWidth("Play") +
                          play_padding * 2 + style.ItemSpacing.x * 2;
    const float right = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine();
    const float status_start = ImGui::GetCursorPosX() + Px(12);
    const float status_end = right - buttons - Px(12);
    std::string status = save_message_;
    ImVec4 status_color = save_failed_ ? kBad : kGood;
    if (!save_failed_ && HasUnsavedChanges()) {
        status = "Unsaved changes";
        status_color = kMuted;
    }
    if (!status.empty() && status_end > status_start) {
        ImGui::SetCursorPosX(status_start);
        ImGui::AlignTextToFramePadding();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::PushClipRect(at, ImVec2(at.x + (status_end - status_start),
                                       at.y + ImGui::GetFrameHeight()),
                            true);
        ImGui::TextColored(status_color, "%s", status.c_str());
        ImGui::PopClipRect();
        if (ImGui::IsItemHovered() && ImGui::CalcTextSize(status.c_str()).x > status_end - status_start) {
            ImGui::SetTooltip("%s", status.c_str());
        }
        ImGui::SameLine();
    }

    ImGui::SetCursorPosX(right - buttons);
    if (ImGui::Button("Save")) Save();
    ImGui::SameLine();
    if (ImGui::Button("Close")) RequestQuit();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(style.FramePadding.x + play_padding, style.FramePadding.y));
    if (ImGui::Button("Play")) Play();
    // Enter or Space plays once the keyboard has moved to it; Play has it first
    ImGui::SetItemDefaultFocus();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);

    ImGui::EndChild();
    ImGui::PopStyleVar();
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
    if (open_save_failed_prompt_) {
        ImGui::OpenPopup(kSaveFailedPrompt);
        open_save_failed_prompt_ = false;
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(22), Px(18)));
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kQuitPrompt, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextUnformatted("Your changes haven't been saved.");
        ImGui::Spacing();
        if (ImGui::Button("Save and quit")) {
            ImGui::CloseCurrentPopup();
            if (Save() && host_.quit) host_.quit();
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit without saving")) {
            ImGui::CloseCurrentPopup();
            if (host_.quit) host_.quit();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kPlayPrompt, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextUnformatted("The game data folder doesn't look like Rock Band 3's:");
        ImGui::TextColored(kWarn, "%s", DescribeProblem(page_.GameData(false).problem));
        ImGui::Spacing();
        if (ImGui::Button("Play")) {
            ImGui::CloseCurrentPopup();
            Start();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    // a band3.toml that can't be written (a read-only install) mustn't keep
    // the game from starting: the settings are already applied to the cvars
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kSaveFailedPrompt, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        // the system's messages end in a full stop of their own ("Access is denied.")
        std::string_view error = save_error_;
        while (!error.empty() && (error.back() == '.' || error.back() == ' ')) {
            error.remove_suffix(1);
        }
        ImGui::PushTextWrapPos(Px(620));
        ImGui::TextColored(kBad, "Couldn't save your settings (%.*s).",
                           static_cast<int>(error.size()), error.data());
        ImGui::PopTextWrapPos();
        ImGui::TextUnformatted("Play anyway with these settings for this session?");
        // a restart reads band3.toml, so Play doesn't restart without it
        if (input::InputBackendChanged()) {
            ImGui::TextColored(kWarn, "The new input backend applies only once it's saved.");
        }
        if (model_.RendererNeedsRestart()) {
            ImGui::TextColored(kWarn, "The new renderer applies only once it's saved.");
        }
        ImGui::Spacing();
        if (ImGui::Button("Play anyway")) {
            ImGui::CloseCurrentPopup();
            Begin();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void LauncherDialog::DrawStarting(ImGuiIO& io) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("band3##launcher_starting", nullptr, flags)) {
        FontScope font(kTitleSize);
        const char* text = restart_ ? "Restarting band3..." : "Starting Rock Band 3...";
        const ImVec2 size = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - size.x) / 2, (io.DisplaySize.y - size.y) / 2));
        ImGui::TextUnformatted(text);
    }
    ImGui::End();
}

void LauncherDialog::HandleNav(const NavEdges& edges) {
    if (edges.tab_previous || edges.tab_next) {
        const int count = static_cast<int>(std::size(kTabs));
        int i = 0;
        while (i < count && kTabs[i].first != current_tab_) i++;
        i = (i + (edges.tab_next ? 1 : count - 1)) % count;
        pending_tab_ = kTabs[i].first;
    }
    // not while a prompt or a dropdown is open
    if (edges.start && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) Play();
}

void LauncherDialog::Play() {
    // the folder may have changed since the last check
    if (!page_.GameData(true).ok) {
        open_play_prompt_ = true;
        return;
    }
    Start();
}

void LauncherDialog::Start() {
    if (stage_ != Stage::kEditing) return;
    if (!Save()) {
        open_save_failed_prompt_ = true;
        return;
    }
    Begin();
}

void LauncherDialog::Begin() {
    if (stage_ != Stage::kEditing) return;
    const bool gpu_changed = model_.RendererNeedsRestart();
    restart_ = RestartsForInput({
        .backend_changed = input::InputBackendChanged(),
        .saved = !save_failed_,
        .test_port = REXCVAR_GET(test_port) != 0,
        .gpu_changed = gpu_changed,
    });
    REXLOG_INFO("Launcher: Play{}{}", save_failed_ ? ", without saving" : "",
                !restart_      ? ""
                : gpu_changed ? ", restarting band3 for the new renderer"
                              : ", restarting band3 for the new input backend");
    stage_ = Stage::kStarting;
    starting_frames_ = 0;
}

bool LauncherDialog::Save() {
    const std::string name = rex::path_to_utf8(host_.config_path.filename());
    const SaveResult result = model_.Save(host_.config_path);
    save_failed_ = !result.ok;
    if (!result.ok) {
        save_error_ = result.error;
        save_message_ = "Couldn't save " + name + ": " + result.error;
        REXLOG_ERROR("Launcher: couldn't save {}: {}", rex::path_to_utf8(host_.config_path),
                     result.error);
        return false;
    }
    const std::string backup = rex::path_to_utf8(BackupPath(host_.config_path).filename());
    if (result.had_comments) {
        save_message_ = "Saved; your old file with its comments is in " + backup + ".";
    } else if (result.backed_up) {
        save_message_ =
            "Saved. The old " + name + " couldn't be read and is kept as " + backup + ".";
    } else {
        save_message_ = "Saved to " + name + ".";
    }
    config_problem_.reset();
    REXLOG_INFO("Launcher: saved {}{}", rex::path_to_utf8(host_.config_path),
                result.backed_up ? " (the old file is kept as .bak)" : "");
    return true;
}

}
