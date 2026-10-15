#include "ingame_settings_dialog.h"
#include <imgui.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include "src/Hooks/song_pause.h"
#include "src/Input/input_lock.h"
#include "src/Input/input_system.h"
#include "src/Input/xinput_state.h"
#include "src/Render/native_view.h"
#include "src/paths.h"
#include "config_file.h"
#include "launcher_style.h"

namespace band3::launcher {

InGameSettingsDialog::InGameSettingsDialog(rex::ui::ImGuiDrawer* imgui_drawer, InGameHost host)
    : rex::ui::ImGuiDialog(imgui_drawer), host_(std::move(host)) {
    skin_ = std::make_unique<ui::Skin>(host_.immediate_drawer);
    pause_ = std::make_unique<ui::PauseScreen>(
        *skin_, ui::PauseHost{
                    .resume = [this] { Resume(); },
                    .settings = [this] { OpenSettings(); },
                    .instrument_lab =
                        [this] {
                            // the lab shows what the game reads, which it reads
                            // nothing of while the menu has the controllers: the
                            // menu closes, and a song stays paused for its own
                            // pause menu to resume
                            band3::song_pause::Forget();
                            SetView(View::kClosed);
                            REXLOG_INFO("Pause menu: closed for the Instrument Lab");
                            if (host_.open_instrument_lab) host_.open_instrument_lab();
                        },
                    .quit =
                        [this] {
                            REXLOG_INFO("Pause menu: quitting");
                            if (host_.quit) host_.quit();
                        },
                    .in_song = [] { return render::InSong(); },
                    .unsaved_settings = [this] { return model_ && model_->HasUnsavedChanges(); },
                });
}

// Not the blocker: the input system may be gone by then (the Rooms panel's rule).
InGameSettingsDialog::~InGameSettingsDialog() = default;

void InGameSettingsDialog::TogglePause() {
    if (host_.all_settings_open && host_.all_settings_open()) {
        if (host_.close_all_settings) host_.close_all_settings();
        SetView(View::kPause);
        return;
    }
    switch (view_) {
    case View::kClosed:
        OpenPause();
        break;
    // asked for and not open (yet): the song is paused already
    case View::kAllSettings:
        SetView(View::kPause);
        break;
    case View::kPause:
    case View::kSettings:
    case View::kVideos:
        // the frame decides, so the Escape that closes a prompt doesn't also go back
        back_requested_ = true;
        break;
    }
}

void InGameSettingsDialog::SetView(View view) {
    view_ = view;
    view_frame_ = ImGui::GetFrameCount();
    appearing_ = view != View::kClosed;
    back_requested_ = false;
    all_settings_seen_ = false;
}

void InGameSettingsDialog::OpenPause() {
    band3::song_pause::Pause();
    SetView(View::kPause);
    pause_->Show();
    REXLOG_INFO("Pause menu: opened");
}

void InGameSettingsDialog::Resume() {
    if (screen_) screen_->CloseMeters();
    band3::song_pause::Resume();
    SetView(View::kClosed);
    REXLOG_INFO("Pause menu: closed");
}

void InGameSettingsDialog::OpenSettings() {
    // back where they were left, as the session goes on
    EnsureModel();
    screen_->Show();
    SetView(View::kSettings);
    REXLOG_INFO("Settings: opened");
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
    screen_ = std::make_unique<ui::SettingsScreen>(
        *model_, *skin_,
        ui::SettingsScreenHost{
            .where = Where::kInGame,
            .config_path = [this] { return config_path_; },
            .game_data_root = host_.game_data_root,
            .native_window = host_.native_window,
            .save = [this] { return Save(); },
            .left =
                [this] {
                    SetView(View::kPause);
                    pause_->Show();
                },
            .play = [this] { Resume(); },
            .videos_list =
                [this] {
                    videos_->Show();
                    SetView(View::kVideos);
                },
            .sdk_settings =
                [this] {
                    if (!host_.open_all_settings) return;
                    // one settings window at a time; Escape goes back from the
                    // SDK's to the pause menu
                    screen_->CloseMeters();
                    SetView(View::kAllSettings);
                    host_.open_all_settings();
                },
        });
    videos_ = std::make_unique<ui::VideosScreen>(
        *skin_, ui::VideosHost{
                    .folders =
                        [this] {
                            std::vector<std::string> entries = paths::SplitList(model_->Value("music_videos_folder"));
                            if (entries.empty()) entries.push_back("videos");
                            std::vector<std::filesystem::path> folders;
                            for (const auto& entry : entries) {
                                folders.push_back(paths::Resolve(entry, model_->Env().anchor));
                            }
                            return folders;
                        },
                    .settings =
                        [this] {
                            screen_->Open(Category::kVideos);
                            screen_->Show();
                            SetView(View::kSettings);
                        },
                    .back =
                        [this] {
                            screen_->Show();
                            SetView(View::kSettings);
                        },
                });
    if (const auto problem = ConfigFileProblem(config_path_)) {
        screen_->ShowBox(ui::Box{.title = "band3.toml couldn't be read",
                                 .lines = {*problem,
                                           "Saving replaces it, and the old file is kept as "
                                           "band3.toml.bak."},
                                 .options = {"OK"},
                                 .cancel = 0});
    }
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
            input::AddGameInputBlocker(*system);
            blocker_taken_ = true;
        }
        caps_ = {};
        next_caps_ = {};
        input_held_ = true;
        return;
    }
    if (blocker_taken_ && system) input::RemoveGameInputBlocker(*system);
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
        // the keyboard navigates by itself, so a player only it plays
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

void InGameSettingsDialog::OnDraw(ImGuiIO& io) {
    const bool all_settings = host_.all_settings_open && host_.all_settings_open();
    // the SDK's menu opens a moment after it's asked for; once it has, its
    // own close goes back to the pause menu too, as does its not opening
    if (view_ == View::kAllSettings) {
        if (all_settings) {
            all_settings_seen_ = true;
        } else if (all_settings_seen_ || ImGui::GetFrameCount() > view_frame_ + 30) {
            SetView(View::kPause);
        }
    }
    HoldInput(io, view_ != View::kClosed || all_settings);
    if (view_ == View::kClosed && !all_settings) return;

    ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiStyle sdk_style = style;
    SetScale(ScaleFor(io.DisplaySize));
    ApplyStyle(style, Scale());
    {
        FontScope font(kBodySize);
        const PadReading pads = ReadPads();
        // the menu shortcut's chords, which the game can't see while it's blocked
        const input::MenuShortcutAction chord =
            shortcut_.Update(pads.chord_buttons, input::MenuShortcut::Clock::now());
        if (chord == input::MenuShortcutAction::kPauseMenu) {
            TogglePause();
        } else if (chord == input::MenuShortcutAction::kInstrumentLab && host_.open_instrument_lab) {
            host_.open_instrument_lab();
        }
        skin_->Load(host_.game_data_root ? host_.game_data_root() : std::filesystem::path());
        skin_->Poll();
        if (view_ == View::kPause) {
            // the bind, when it isn't Escape (which the menu reads itself), goes back
            if (std::exchange(back_requested_, false) && !ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
                ImGui::GetFrameCount() > view_frame_ + 1) {
                pause_->Back();
            }
            if (view_ == View::kPause) pause_->Draw(io, pads.nav);
        } else if (view_ == View::kSettings) {
            // the bind, when it isn't Escape (which the screen reads itself), goes back
            if (std::exchange(back_requested_, false) && !ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
                ImGui::GetFrameCount() > view_frame_ + 1) {
                screen_->Back();
            }
            screen_->Draw(io, pads.nav, true);
        } else if (view_ == View::kVideos) {
            if (std::exchange(back_requested_, false) && !ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
                ImGui::GetFrameCount() > view_frame_ + 1) {
                videos_->Back();
            }
            if (view_ == View::kVideos) videos_->Draw(io, pads.nav, true);
        }
        if (all_settings) DrawAllSettingsNote(io);
    }
    style = sdk_style;
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
            OpenSettings();
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            if (host_.close_all_settings) host_.close_all_settings();
            SetView(View::kPause);
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kMuted, "Escape goes back to the pause menu.");
    }
    ImGui::End();
}

SaveResult InGameSettingsDialog::Save() {
    const SaveResult result = model_->Save(config_path_);
    if (!result.ok) {
        REXLOG_ERROR("Settings: couldn't save {}: {}", rex::path_to_utf8(config_path_), result.error);
        return result;
    }
    REXLOG_INFO("Settings: saved {}{}", rex::path_to_utf8(config_path_),
                result.backed_up ? " (the old file is kept as .bak)" : "");
    return result;
}

}
