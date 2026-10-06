#include "ingame_settings_dialog.h"
#include <imgui.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include "src/Input/input_lock.h"
#include "src/Input/input_system.h"
#include "src/Input/xinput_state.h"
#include "config_file.h"
#include "launcher_style.h"

namespace band3::launcher {

namespace {

constexpr const char* kClosePrompt = "Close without saving?";

constexpr std::pair<Tab, const char*> kTabs[] = {
    {Tab::kGame, "Game"},
    {Tab::kGraphics, "Graphics"},
    {Tab::kAudio, "Audio"},
    {Tab::kControllers, "Controllers"},
    {Tab::kOnline, "Online"},
    {Tab::kAdvanced, "Advanced"},
};

// what a tab says above its settings, if anything
const char* TabIntro(Tab tab) {
    switch (tab) {
    case Tab::kControllers:
        return "To see a controller or an instrument working, open the Instrument Lab (F6): it "
               "shows what the game reads from each player, live.";
    case Tab::kAdvanced:
        return "band3's technical settings, for testing and tracking down problems, by the "
               "names the command line uses. Point at one to see what it does.";
    default: return nullptr;
    }
}

}

InGameSettingsDialog::InGameSettingsDialog(rex::ui::ImGuiDrawer* imgui_drawer, InGameHost host)
    : rex::ui::ImGuiDialog(imgui_drawer), host_(std::move(host)) {}

// Not the blocker: the input system may be gone by then (the Rooms panel's
// rule). ImGui's flags go back as they were.
InGameSettingsDialog::~InGameSettingsDialog() {
    if (!input_held_) return;
    nav_.reset();
    ImGuiIO& io = GetIO();
    io.ConfigFlags = (io.ConfigFlags & ~ImGuiConfigFlags_NavEnableKeyboard) |
                     (saved_config_flags_ & ImGuiConfigFlags_NavEnableKeyboard);
}

void InGameSettingsDialog::Toggle() {
    if (host_.all_settings_open && host_.all_settings_open()) {
        if (host_.close_all_settings) host_.close_all_settings();
        return;
    }
    if (open_) {
        RequestClose();
    } else {
        Open();
    }
}

void InGameSettingsDialog::Open() {
    EnsureModel();
    open_ = true;
    appearing_ = true;
    save_message_.clear();
    save_failed_ = false;
    REXLOG_INFO("Settings: opened");
}

void InGameSettingsDialog::RequestClose() {
    if (model_ && model_->HasUnsavedChanges()) {
        open_close_prompt_ = true;
        return;
    }
    Close();
}

void InGameSettingsDialog::Close() {
    open_ = false;
    open_close_prompt_ = false;
    // lets go of SDL's audio, which the mic slots' list keeps started
    if (page_) page_->CloseMeters();
    REXLOG_INFO("Settings: closed{}",
                model_ && model_->HasUnsavedChanges() ? " with unsaved changes" : "");
}

void InGameSettingsDialog::EnsureModel() {
    if (model_) return;
    const std::vector<RegistryCvar> registry = ReadRegistry();
    generated_ = std::make_unique<GeneratedRows>(registry, SettingTable());
    table_ = JoinTables(SettingTable(), generated_->Rows());
    config_path_ = host_.config_path ? host_.config_path() : std::filesystem::path();
    model_.emplace(table_,
                   ReadEnvironment(table_, host_.path_defaults ? host_.path_defaults() : PathDefaults{},
                                   host_.anchor ? host_.anchor() : std::filesystem::path(),
                                   Where::kInGame, config_path_),
                   store_);
    page_.emplace(*model_, Where::kInGame,
                  PageHost{.game_data_root = host_.game_data_root,
                           .native_window = host_.native_window});
    config_problem_ = ConfigFileProblem(config_path_);
    REXLOG_INFO("Settings: {} rows, {} of them from the registry", table_.size(),
                generated_->Rows().size());
}

void InGameSettingsDialog::HoldInput(ImGuiIO& io, bool hold) {
    if (hold == input_held_) return;
    rex::input::InputSystem* system = input::GameInputSystem();
    if (hold) {
        // the game reads a neutral pad while any blocker is held; buttons
        // still held when the last goes stay masked until let go (the SDK)
        if (system) {
            system->AddUIInputBlocker();
            blocker_taken_ = true;
        }
        saved_config_flags_ = io.ConfigFlags;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        nav_.emplace(io);
        caps_ = {};
        next_caps_ = {};
        input_held_ = true;
        return;
    }
    nav_.reset();
    io.ConfigFlags = (io.ConfigFlags & ~ImGuiConfigFlags_NavEnableKeyboard) |
                     (saved_config_flags_ & ImGuiConfigFlags_NavEnableKeyboard);
    if (blocker_taken_ && system) system->RemoveUIInputBlocker();
    blocker_taken_ = false;
    input_held_ = false;
}

InGameSettingsDialog::PadReading InGameSettingsDialog::ReadPads() {
    PadReading out;
    rex::input::InputSystem* system = input::GameInputSystem();
    if (!system) return out;
    // the UI thread doesn't wait for the game's threads: a frame the game has
    // the lock reads nothing, which navigation takes as nothing held
    std::unique_lock<std::recursive_mutex> lock(input::InputLock(), std::try_to_lock);
    if (!lock.owns_lock()) return out;
    using rex::X_RESULT;  // X_ERROR_SUCCESS names it unqualified
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_caps_) {
        next_caps_ = now + std::chrono::seconds(2);
        // the keyboard navigates ImGui itself, so a player only it plays
        // doesn't (it reads as a controller too)
        std::array<bool, 4> navigates{};
        for (const input::InputDevice& device : input::PlayerDevices()) {
            if (device.player >= 1 && device.player <= 4 && DrivesNavigation(device.kind)) {
                navigates[static_cast<size_t>(device.player - 1)] = true;
            }
        }
        for (uint32_t user = 0; user < caps_.size(); user++) {
            caps_[user].reset();
            if (!navigates[user]) continue;
            rex::input::X_INPUT_CAPABILITIES caps{};
            if (system->GetCapabilities(user, 0, &caps) == X_ERROR_SUCCESS) {
                caps_[user] = input::LoadCaps(caps);
            }
        }
    }
    for (uint32_t user = 0; user < caps_.size(); user++) {
        if (!caps_[user]) continue;
        rex::input::X_INPUT_STATE state{};
        if (system->GetStateForUI(user, &state) != X_ERROR_SUCCESS) continue;
        const input::Gamepad360 pad = input::LoadGamepad(state.gamepad);
        out.nav.push_back(NavFromReading(*caps_[user], pad));
        if (!input::IsRb3InstrumentSubtype(caps_[user]->sub_type)) out.chord_buttons |= pad.buttons;
    }
    return out;
}

void InGameSettingsDialog::HandleNav(const NavEdges& edges) {
    if (edges.tab_previous || edges.tab_next) {
        const int count = static_cast<int>(std::size(kTabs));
        int i = 0;
        while (i < count && kTabs[i].first != current_tab_) i++;
        i = (i + (edges.tab_next ? 1 : count - 1)) % count;
        pending_tab_ = kTabs[i].first;
    }
    // Start goes back to the game; not while a prompt or a dropdown is open
    if (edges.start && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) RequestClose();
}

void InGameSettingsDialog::OnDraw(ImGuiIO& io) {
    const bool all_settings = host_.all_settings_open && host_.all_settings_open();
    HoldInput(io, open_ || all_settings);
    if (!open_ && !all_settings) return;

    ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiStyle sdk_style = style;
    SetScale(ScaleFor(io.DisplaySize));
    ApplyStyle(style, Scale());
    {
        FontScope font(kBodySize);
        const PadReading pads = ReadPads();
        const NavEdges edges = nav_ ? nav_->Feed(pads.nav) : NavEdges{};
        // the menu shortcut's chords, which the game can't see while it's blocked
        const input::MenuShortcutAction chord =
            shortcut_.Update(pads.chord_buttons, input::MenuShortcut::Clock::now());
        if (chord == input::MenuShortcutAction::kSettings) {
            if (open_) {
                RequestClose();
            } else if (host_.close_all_settings) {
                host_.close_all_settings();
            }
        } else if (chord == input::MenuShortcutAction::kInstrumentLab && host_.open_instrument_lab) {
            host_.open_instrument_lab();
        }
        if (open_) {
            HandleNav(edges);
            page_->BeginFrame();
            DrawWindow(io);
            page_->EndFrame();
        }
        if (all_settings) DrawAllSettingsNote(io);
    }
    style = sdk_style;
}

void InGameSettingsDialog::DrawWindow(ImGuiIO& io) {
    // nearly the whole window at 720p (a Steam Deck's 1280x800 included),
    // scaled up with it, as the launcher is
    const float margin = Px(20);
    const ImVec2 size(std::min(io.DisplaySize.x - margin * 2, Px(1180)),
                      std::max(io.DisplaySize.y - margin * 2, Px(300)));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.97f);
    if (appearing_) {
        ImGui::SetNextWindowFocus();
        appearing_ = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Px(8));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings;
    const bool shown = ImGui::Begin("band3 settings##ingame", nullptr, flags);
    ImGui::PopStyleVar(2);
    if (shown) {
        DrawHeader();
        if (config_problem_) {
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

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(16), Px(7)));
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
        page_->RefreshDeviceLists(current_tab_);
        page_->DrawDeckBanner();
        ImGui::Dummy(ImVec2(0, Px(2)));

        // the footer keeps its place at the bottom; the settings scroll above it
        const ImGuiStyle& style = ImGui::GetStyle();
        const float footer_height = Px(8) + SettingsPage::DescriptionHeight() +
                                    ImGui::GetFrameHeight() + style.ItemSpacing.y * 4;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        const float settings_height =
            std::max(ImGui::GetContentRegionAvail().y - footer_height, Px(80));
        if (ImGui::BeginChild("##settings", ImVec2(0, settings_height),
                              ImGuiChildFlags_NavFlattened)) {
            ImGui::Dummy(ImVec2(0, Px(4)));
            if (const char* intro = TabIntro(current_tab_)) {
                FontScope font(kSmallSize);
                ImGui::PushTextWrapPos(0);
                ImGui::TextColored(kMuted, "%s", intro);
                ImGui::PopTextWrapPos();
                ImGui::Dummy(ImVec2(0, Px(6)));
            }
            page_->DrawSections(current_tab_);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        DrawFooter();
        DrawPrompts();

        // B on a controller, or Escape, goes back to the game, unless it was
        // closing a dropdown or leaving a text field (as in the frame before)
        const bool busy = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) ||
                          ImGui::IsAnyItemActive();
        const bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                          ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (back && !busy && !busy_last_frame_ &&
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            RequestClose();
        }
        busy_last_frame_ = busy;
    }
    ImGui::End();
}

void InGameSettingsDialog::DrawHeader() {
    {
        FontScope font(kHeadingSize);
        ImGui::TextUnformatted("band3 settings");
    }
    // where Save writes, on the right, if there's room
    const float title_end = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    FontScope font(kSmallSize);
    const std::string where = "Saves to " + rex::path_to_utf8(config_path_);
    const float width = ImGui::CalcTextSize(where.c_str()).x;
    const float right = ImGui::GetWindowContentRegionMax().x;
    if (right - width > title_end + Px(40)) {
        ImGui::SameLine(right - width);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(kHeadingSize - kSmallSize) * 0.8f);
        ImGui::TextColored(kMuted, "%s", where.c_str());
    }
}

void InGameSettingsDialog::DrawFooter() {
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

    const char* kAllSettings = "All settings...";
    page_->DrawDescription([&] {
        ImGui::TextColored(kMuted, "Point at a setting to see what it does. Changes apply as you "
                                   "make them, some at the next start (the row says so); Save "
                                   "keeps them for next time. The game keeps running.");
        const bool controller =
            std::ranges::any_of(caps_, [](const auto& caps) { return caps.has_value(); });
        if (controller) {
            ImGui::TextColored(kMuted, "With a controller: LB and RB switch tabs, B or Start "
                                       "goes back to the game.");
        }
    });

    // the bottom row: the startup box, what saving did, and the buttons
    page_->DrawStartupBox();
    const float buttons = ButtonWidth(kAllSettings) + ButtonWidth("Save") + ButtonWidth("Close") +
                          style.ItemSpacing.x * 2;
    const float right = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine();
    const float status_start = ImGui::GetCursorPosX() + Px(12);
    const float status_end = right - buttons - Px(12);
    std::string status = save_message_;
    ImVec4 status_color = save_failed_ ? kBad : kGood;
    if (!save_failed_ && model_->HasUnsavedChanges()) {
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
        if (ImGui::IsItemHovered() &&
            ImGui::CalcTextSize(status.c_str()).x > status_end - status_start) {
            ImGui::SetTooltip("%s", status.c_str());
        }
        ImGui::SameLine();
    }

    ImGui::SetCursorPosX(right - buttons);
    if (ImGui::Button(kAllSettings) && host_.open_all_settings) {
        // one settings window at a time; F4 closes the SDK's
        Close();
        host_.open_all_settings();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The SDK's own settings menu: every setting by category, the SDK's\n"
                          "and the key binds too. Its \"Save to config\" rewrites band3.toml\n"
                          "with every setting that isn't its default, unlike Save here.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Save")) Save();
    ImGui::SameLine();
    if (ImGui::Button("Close")) RequestClose();

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void InGameSettingsDialog::DrawPrompts() {
    if (open_close_prompt_) {
        ImGui::OpenPopup(kClosePrompt);
        open_close_prompt_ = false;
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(22), Px(18)));
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kClosePrompt, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextUnformatted("Your changes haven't been saved.");
        ImGui::TextColored(kMuted, "Closing keeps them until band3 closes.");
        ImGui::Spacing();
        if (ImGui::Button("Save and close")) {
            ImGui::CloseCurrentPopup();
            Save();
            if (!save_failed_) Close();
        }
        ImGui::SameLine();
        if (ImGui::Button("Close without saving")) {
            ImGui::CloseCurrentPopup();
            Close();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void InGameSettingsDialog::DrawAllSettingsNote(ImGuiIO& io) {
    // at the bottom, clear of where the SDK's menu opens (60,60, 620x480)
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - Px(16)),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(16), Px(10)));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    const bool shown = ImGui::Begin("All settings##ingame_note", nullptr, flags);
    ImGui::PopStyleVar(2);
    if (shown) {
        FontScope font(kSmallSize);
        ImGui::PushTextWrapPos(Px(760));
        ImGui::TextColored(kAccent, "All settings (the SDK's settings menu)");
        ImGui::TextUnformatted(
            "Its \"Save to config\" rewrites band3.toml with every setting that isn't its "
            "default, whatever set it, and drops anything else in the file. band3's own "
            "settings save only what you change.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Back to band3's settings")) {
            if (host_.close_all_settings) host_.close_all_settings();
            Open();
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            if (host_.close_all_settings) host_.close_all_settings();
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kMuted, "F4 closes it too.");
    }
    ImGui::End();
}

void InGameSettingsDialog::Save() {
    const std::string name = rex::path_to_utf8(config_path_.filename());
    const SaveResult result = model_->Save(config_path_);
    save_failed_ = !result.ok;
    if (!result.ok) {
        save_message_ = "Couldn't save " + name + ": " + result.error;
        REXLOG_ERROR("Settings: couldn't save {}: {}", rex::path_to_utf8(config_path_),
                     result.error);
        return;
    }
    const std::string backup = rex::path_to_utf8(BackupPath(config_path_).filename());
    if (result.had_comments) {
        save_message_ = "Saved; your old file with its comments is in " + backup + ".";
    } else if (result.backed_up) {
        save_message_ =
            "Saved. The old " + name + " couldn't be read and is kept as " + backup + ".";
    } else {
        save_message_ = "Saved to " + name + ".";
    }
    config_problem_.reset();
    REXLOG_INFO("Settings: saved {}{}", rex::path_to_utf8(config_path_),
                result.backed_up ? " (the old file is kept as .bak)" : "");
}

}
