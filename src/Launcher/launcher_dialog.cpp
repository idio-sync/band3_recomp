#include "launcher_dialog.h"
#include <imgui.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <algorithm>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include "src/Input/input_system.h"
#include "src/paths.h"
#include "src/settings.h"
#include "config_file.h"
#include "launcher_start.h"
#include "launcher_style.h"

namespace band3::launcher {

namespace {

// the registry's rows the table doesn't have, made before the model that uses them
std::unique_ptr<GeneratedRows> MakeGenerated() {
    return std::make_unique<GeneratedRows>(ReadRegistry(), SettingTable());
}

}

LauncherDialog::LauncherDialog(rex::ui::ImGuiDrawer* imgui_drawer, LauncherHost host)
    : rex::ui::ImGuiDialog(imgui_drawer),
      host_(std::move(host)),
      generated_(MakeGenerated()),
      table_(JoinTables(SettingTable(), generated_->Rows())),
      model_(table_, ReadEnvironment(table_, host_.path_defaults, host_.anchor, Where::kLauncher), store_) {
    // the old views use ImGui's navigation; the Settings screen has its own
    ImGuiIO& io = GetIO();
    saved_config_flags_ = io.ConfigFlags;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    nav_.emplace(io);
    skin_ = std::make_unique<ui::Skin>(host_.immediate_drawer);
    screen_ = std::make_unique<ui::SettingsScreen>(
        model_, *skin_,
        ui::SettingsScreenHost{
            .where = Where::kLauncher,
            .config_path = [this] { return host_.config_path; },
            .game_data_root = host_.game_data_root,
            .native_window = host_.native_window,
            .save = [this] { return Save(); },
            .left =
                [this] {
                    screen_->ShowBox(ui::Box{.title = "Quit band3?",
                                             .lines = {"Your settings are saved."},
                                             .options = {"Quit", "Cancel"},
                                             .selected = 1,
                                             .cancel = 1,
                                             .chosen = [this](int choice) {
                                                 if (choice == 0 && host_.quit) host_.quit();
                                             }});
                },
            .play = [this] { Play(); },
            .test_controllers = [this] { view_ = View::kTestControllers; },
            .videos_list = [this] { view_ = View::kVideos; },
        });
    gallery_ = std::make_unique<ui::WidgetGallery>(*skin_);
    // 1 for the gallery, plain for it in the look drawn without the game's textures
    if (const char* gallery = std::getenv("BAND3_LAUNCHER_GALLERY")) {
        show_gallery_ = std::string_view(gallery) == "1" || std::string_view(gallery) == "plain";
        gallery_->SetPlain(std::string_view(gallery) == "plain");
    }
    if (const auto problem = ConfigFileProblem(host_.config_path)) {
        REXLOG_WARN("Launcher: {} couldn't be read: {}", rex::path_to_utf8(host_.config_path), *problem);
        screen_->ShowBox(ui::Box{.title = "band3.toml couldn't be read",
                                 .lines = {*problem,
                                           "Saving replaces it, and the old file is kept as "
                                           "band3.toml.bak."},
                                 .options = {"OK"},
                                 .cancel = 0});
    }
    REXLOG_INFO("Launcher: {} rows, {} of them from the registry", table_.size(), generated_->Rows().size());
}

LauncherDialog::~LauncherDialog() {
    screen_->CloseMeters();
    nav_.reset();
    ImGuiIO& io = GetIO();
    io.ConfigFlags = (io.ConfigFlags & ~ImGuiConfigFlags_NavEnableKeyboard) |
                     (saved_config_flags_ & ImGuiConfigFlags_NavEnableKeyboard);
}

bool LauncherDialog::HasUnsavedChanges() const { return model_.HasUnsavedChanges(); }

void LauncherDialog::RequestQuit() {
    if (stage_ != Stage::kEditing) return;
    if (!HasUnsavedChanges()) {
        if (host_.quit) host_.quit();
        return;
    }
    view_ = View::kSettings;
    screen_->ShowBox(ui::Box{.title = "Quit without saving?",
                             .lines = {"Your changes haven't been saved."},
                             .options = {"Save and quit", "Quit without saving", "Cancel"},
                             .selected = 2,
                             .cancel = 2,
                             .chosen = [this](int choice) {
                                 if (choice == 0 && Save().ok && host_.quit) host_.quit();
                                 if (choice == 1 && host_.quit) host_.quit();
                             }});
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
    skin_->Load(host_.game_data_root ? host_.game_data_root() : std::filesystem::path());
    skin_->Poll();
    {
        FontScope font(kBodySize);
        if (stage_ == Stage::kEditing && ImGui::IsKeyPressed(ImGuiKey_G, false) && io.KeyCtrl &&
            io.KeyShift) {
            show_gallery_ = !show_gallery_;
        }
        if (stage_ == Stage::kEditing && show_gallery_) {
            device_panel_.Poll(false);
            gallery_->Draw(io, device_panel_.NavPads());
        } else if (stage_ == Stage::kEditing) {
            // the instrument settings restart band3's drivers as they change
            input::ApplyInputSettings();
            // the devices are read only until Play: then they're the game's
            device_panel_.Poll(view_ == View::kTestControllers);
            if (view_ == View::kSettings) {
                screen_->Draw(io, device_panel_.NavPads(), false);
            } else {
                nav_->Feed(device_panel_.NavPads());
                DrawOldView(io);
            }
        } else {
            // nothing of the launcher's records once Play is pressed: the
            // game's own capture opens the microphones
            screen_->CloseMeters();
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

void LauncherDialog::DrawOldView(ImGuiIO& io) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("band3##launcher_old_view", nullptr, flags)) {
        if (ImGui::Button("< Settings") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            view_ = View::kSettings;
        }
        ImGui::SameLine();
        {
            FontScope font(kHeadingSize);
            ImGui::TextUnformatted(view_ == View::kVideos ? "Music videos" : "Test controllers");
        }
        ImGui::Separator();
        if (view_ == View::kTestControllers) {
            device_panel_.Draw();
        } else {
            std::vector<std::string> entries = paths::SplitList(model_.Value("music_videos_folder"));
            if (entries.empty()) entries.push_back("videos");
            std::vector<std::filesystem::path> folders;
            for (const auto& entry : entries) folders.push_back(paths::Resolve(entry, model_.Env().anchor));
            VideosTab::Page page;
            page.settings = [] {
                ImGui::TextColored(kMuted, "The videos' settings are in Settings, Music Videos.");
            };
            page.description = [] {};
            page.description_height = 0;
            videos_.Draw(folders, page);
        }
    }
    ImGui::End();
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

void LauncherDialog::Play() {
    // the folder may have changed since the last check
    const GameDataCheck& check = screen_->GameData(true);
    if (check.ok) {
        Begin();
        return;
    }
    const std::filesystem::path root = host_.game_data_root ? host_.game_data_root() : std::filesystem::path();
    screen_->ShowBox(ui::Box{
        .title = "Rock Band 3 isn't in the game data folder",
        .lines = {std::string(DescribeProblem(check.problem)) + ": " +
                      (root.empty() ? std::string("no folder set") : rex::path_to_utf8(root)) + ".",
                  "Pick its folder in Settings, Game, Game data folder."},
        .options = {"Play anyway", "Cancel"},
        .selected = 1,
        .cancel = 1,
        .chosen = [this](int choice) {
            if (choice == 0) Begin();
        }});
}

void LauncherDialog::Begin() {
    if (stage_ != Stage::kEditing) return;
    const bool gpu_changed = model_.RendererNeedsRestart();
    // unsaved changes stay unsaved: a restart reads band3.toml
    save_failed_ = save_failed_ || model_.HasUnsavedChanges();
    restart_ = RestartsForInput({
        .backend_changed = input::InputBackendChanged(),
        .saved = !save_failed_,
        .test_port = REXCVAR_GET(test_port) != 0,
        .gpu_changed = gpu_changed,
    });
    REXLOG_INFO("Launcher: Play{}{}", save_failed_ ? ", without saving" : "",
                !restart_     ? ""
                : gpu_changed ? ", restarting band3 for the new renderer"
                              : ", restarting band3 for the new input backend");
    stage_ = Stage::kStarting;
    starting_frames_ = 0;
}

SaveResult LauncherDialog::Save() {
    const SaveResult result = model_.Save(host_.config_path);
    save_failed_ = !result.ok;
    if (!result.ok) {
        REXLOG_ERROR("Launcher: couldn't save {}: {}", rex::path_to_utf8(host_.config_path), result.error);
        return result;
    }
    REXLOG_INFO("Launcher: saved {}{}", rex::path_to_utf8(host_.config_path),
                result.backed_up ? " (the old file is kept as .bak)" : "");
    return result;
}

}
