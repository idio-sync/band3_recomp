#include "launcher_dialog.h"
#include <imgui.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>
#include "src/config.h"
#include "src/Input/input_system.h"
#include "src/paths.h"
#include "src/settings.h"
#include "config_file.h"
#include "launcher_start.h"
#include "launcher_style.h"
#include "ui/screen_kit.h"
#include "ui/widgets.h"

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
    skin_ = std::make_unique<ui::Skin>(host_.immediate_drawer);
    hub_ = std::make_unique<ui::HubScreen>(
        *skin_, ui::HubHost{
                    .pick = [this](ui::HubItem item) { Pick(item); },
                    .game_data_root = host_.game_data_root,
                    .game_found = [this] { return screen_->GameData(false).ok; },
                    .controllers =
                        [this] {
                            std::vector<ui::HubController> out;
                            for (const DevicePanel::Entry& e : device_panel_.Entries()) {
                                // what plays: not the keyboard, the stand-in or SDL's copies
                                if (!e.testable || e.player <= 0) continue;
                                out.push_back({.sub_type = e.sub_type,
                                               .midi_drums = e.kind == input::DeviceKind::kMidiDrums,
                                               .midi_keys = e.kind == input::DeviceKind::kMidiKeys});
                            }
                            return out;
                        },
                    .user_data_root = [] { return UserDataRoot(); },
                });
    screen_ = std::make_unique<ui::SettingsScreen>(
        model_, *skin_,
        ui::SettingsScreenHost{
            .where = Where::kLauncher,
            .config_path = [this] { return host_.config_path; },
            .game_data_root = host_.game_data_root,
            .native_window = host_.native_window,
            .save = [this] { return Save(); },
            .left = [this] { Back(View::kSettings); },
            .back_label = [this] { return BackLabel(View::kSettings); },
            .play = [this] { Play(); },
            .test_controllers = [this] { Pick(ui::HubItem::kTestControllers); },
            .videos_list = [this] { Pick(ui::HubItem::kMusicVideos); },
            .find_game = [this] { Pick(ui::HubItem::kFindGame); },
        });
    find_ = std::make_unique<ui::FindGameScreen>(
        *skin_, ui::FindGameHost{
                    .use =
                        [this](const std::filesystem::path& folder) {
                            model_.Set("game_data_root", rex::path_to_utf8(folder));
                            Save();
                            REXLOG_INFO("Launcher: the game data folder is now {}", rex::path_to_utf8(folder));
                            ShowHub();
                        },
                    .back = [this] { Back(View::kFindGame); },
                    .back_label = [this] { return BackLabel(View::kFindGame); },
                });
    controllers_ = std::make_unique<ui::ControllersScreen>(
        *skin_, device_panel_,
        ui::ControllersHost{
            .settings = [this](Category category) { Open(View::kSettings, category); },
            .back = [this] { Back(View::kTestControllers); },
            .back_label = [this] { return BackLabel(View::kTestControllers); },
            .play = [this] { Play(); },
        });
    videos_ = std::make_unique<ui::VideosScreen>(
        *skin_, ui::VideosHost{
                    .folders =
                        [this] {
                            std::vector<std::string> entries = paths::SplitList(model_.Value("music_videos_folder"));
                            if (entries.empty()) entries.push_back("videos");
                            std::vector<std::filesystem::path> folders;
                            for (const auto& entry : entries) {
                                folders.push_back(paths::Resolve(entry, model_.Env().anchor));
                            }
                            return folders;
                        },
                    .settings = [this] { Open(View::kSettings, Category::kVideos); },
                    .back = [this] { Back(View::kVideos); },
                    .back_label = [this] { return BackLabel(View::kVideos); },
                    .play = [this] { Play(); },
                });
    gallery_ = std::make_unique<ui::WidgetGallery>(*skin_);
    // 1 for the gallery, plain for it in the look drawn without the game's textures,
    // simple for it in the Simple look
    if (const char* gallery = std::getenv("BAND3_LAUNCHER_GALLERY")) {
        const std::string_view which(gallery);
        show_gallery_ = which == "1" || which == "plain" || which == "simple";
        if (which == "plain") gallery_->SetShown(ui::WidgetGallery::Shown::kPlain);
        if (which == "simple") gallery_->SetShown(ui::WidgetGallery::Shown::kSimple);
    }
    hub_->Show();
    if (const auto problem = ConfigFileProblem(host_.config_path)) {
        REXLOG_WARN("Launcher: {} couldn't be read: {}", rex::path_to_utf8(host_.config_path), *problem);
        hub_->ShowBox(ui::Box{.title = "band3.toml couldn't be read",
                              .lines = {*problem,
                                        "Saving replaces it, and the old file is kept as band3.toml.bak."},
                              .options = {"OK"},
                              .cancel = 0});
    }
    REXLOG_INFO("Launcher: {} rows, {} of them from the registry", table_.size(), generated_->Rows().size());
}

LauncherDialog::~LauncherDialog() { screen_->CloseMeters(); }

bool LauncherDialog::HasUnsavedChanges() const { return model_.HasUnsavedChanges(); }

void LauncherDialog::ShowHub() { Show(View::kHub); }

void LauncherDialog::Show(View view, std::optional<Category> category) {
    if (view != View::kTestControllers) device_panel_.EndTest();
    if (view != View::kSettings) screen_->CloseMeters();
    switch (view) {
    case View::kHub: hub_->Show(); break;
    case View::kSettings:
        if (category) screen_->Open(*category);
        screen_->Show();
        break;
    case View::kFindGame: find_->Show(); break;
    case View::kTestControllers: controllers_->Show(); break;
    case View::kVideos: videos_->Show(); break;
    }
    view_ = view;
}

void LauncherDialog::Open(View to, std::optional<Category> category) {
    const View from = view_;
    const auto it = opened_from_.find(from);
    const bool going_back = it != opened_from_.end() && it->second == to;
    if (to != from && !going_back) opened_from_[to] = from;
    Show(to, category);
}

void LauncherDialog::Back(View from) {
    const auto it = opened_from_.find(from);
    Show(it == opened_from_.end() ? View::kHub : it->second);
}

std::string LauncherDialog::BackLabel(View from) const {
    const auto it = opened_from_.find(from);
    switch (it == opened_from_.end() ? View::kHub : it->second) {
    case View::kSettings: return "Back to Settings";
    case View::kVideos: return "Back to Music Videos";
    case View::kTestControllers: return "Back to Test Controllers";
    case View::kFindGame: return "Back to Find Rock Band 3";
    case View::kHub: break;
    }
    return "Back to menu";
}

void LauncherDialog::Pick(ui::HubItem item) {
    switch (item) {
    case ui::HubItem::kFindGame: Open(View::kFindGame); break;
    case ui::HubItem::kPlay: Play(); break;
    case ui::HubItem::kSettings: Open(View::kSettings); break;
    case ui::HubItem::kMusicVideos: Open(View::kVideos); break;
    case ui::HubItem::kTestControllers: Open(View::kTestControllers); break;
    case ui::HubItem::kQuit: Quit(); break;
    }
}

void LauncherDialog::Quit() {
    if (!HasUnsavedChanges()) {
        if (host_.quit) host_.quit();
        return;
    }
    ShowHub();
    hub_->ShowBox(ui::Box{.title = "Quit without saving?",
                          .lines = {"Your changes haven't been saved."},
                          .options = {"Save and quit", "Quit without saving", "Cancel"},
                          .selected = 2,
                          .cancel = 2,
                          .chosen = [this](int choice) {
                              if (choice == 0 && Save().ok && host_.quit) host_.quit();
                              if (choice == 1 && host_.quit) host_.quit();
                          }});
}

void LauncherDialog::RequestQuit() {
    if (stage_ != Stage::kEditing) return;
    Quit();
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
    ui::SetLook(ui::LookFromSetting(REXCVAR_GET(launcher_style)));
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
            const std::vector<NavPad>& pads = device_panel_.NavPads();
            switch (view_) {
            case View::kHub: hub_->Draw(io, pads); break;
            case View::kSettings: screen_->Draw(io, pads, false); break;
            case View::kFindGame: find_->Draw(io, pads); break;
            case View::kTestControllers: controllers_->Draw(io, pads); break;
            case View::kVideos: videos_->Draw(io, pads, false); break;
            }
        } else {
            // nothing of the launcher's records once Play is pressed: the
            // game's own capture opens the microphones
            screen_->CloseMeters();
            device_panel_.StopTest();
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

void LauncherDialog::DrawStarting(ImGuiIO& io) {
    using namespace ui;
    const Page page = PageFor(io);
    ImDrawList* dl = BeginScreen("band3##launcher_starting", io, *skin_);
    const auto centred = [&](float y, const TextStyle& style, ImU32 color, std::string_view text) {
        const float w = TextWidth(style, text);
        PaintText(dl, ImVec2(io.DisplaySize.x * 0.5f - w * 0.5f, page.At(0, y).y), style, color, text);
    };
    centred(296, kCaps(64), color::kText, restart_ ? "Restarting band3" : "Starting Rock Band 3");
    const bool kept = save_failed_ || model_.HasUnsavedChanges();
    centred(378, kCaps(22), kept ? color::kWarn : color::kGood,
            kept ? "Playing with unsaved settings" : "Settings saved");
    // a bar: the next frame holds the UI thread while the game is built, so it can't move
    dl->AddRectFilled(page.At(440, 420), page.At(840, 426), IM_COL32(255, 255, 255, 31), Px(3));
    dl->AddRectFilled(page.At(440, 420), page.At(592, 426), color::kYellow, Px(3));
    EndScreen();
}

void LauncherDialog::Play() {
    // the folder may have changed since the last check
    const GameDataCheck& check = screen_->GameData(true);
    if (check.ok) {
        Begin();
        return;
    }
    const std::filesystem::path root = host_.game_data_root ? host_.game_data_root() : std::filesystem::path();
    ShowHub();
    hub_->ShowBox(ui::Box{
        .title = "Rock Band 3 not found",
        .lines = {"Game data folder: " + (root.empty() ? std::string("none set") : rex::path_to_utf8(root)),
                  std::string(DescribeProblem(check.problem))},
        // no Play anyway: without the game it can't run
        .options = {"Find Rock Band 3", "Cancel"},
        .cancel = 1,
        .chosen = [this](int choice) {
            if (choice == 0) Pick(ui::HubItem::kFindGame);
        }});
}

void LauncherDialog::Begin() {
    if (stage_ != Stage::kEditing) return;
    // what wasn't saved yet (a game folder found, the startup box) is saved
    // now; a restart reads band3.toml, so a failed save keeps band3 from one
    if (model_.HasUnsavedChanges() && !Save().ok) save_failed_ = true;
    const bool gpu_changed = model_.RendererNeedsRestart();
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
    device_panel_.EndTest();
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
