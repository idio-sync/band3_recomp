#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <rex/ui/imgui_dialog.h>
#include "game_data_check.h"
#include "gamepad_nav.h"
#include "generated_rows.h"
#include "instrument_view.h"
#include "launcher_cvars.h"
#include "launcher_settings.h"
#include "videos_tab.h"
#include "ui/settings_screen.h"
#include "ui/skin.h"
#include "ui/widget_gallery.h"

namespace band3::launcher {

// What the launcher needs from the app (Band3App).
struct LauncherHost {
    // band3.toml, where Save writes
    std::filesystem::path config_path;
    // the folders the folder settings fall back to (OnFinalizePaths' defaults)
    PathDefaults path_defaults;
    // what relative folders are relative to (IniAnchor)
    std::filesystem::path anchor;
    // the game data folder the game would start with now
    std::function<std::filesystem::path()> game_data_root;
    // the refresh rate of the window's display, in Hz (0 when unknown); the
    // launcher paces itself to it, since nothing else does before the game runs
    std::function<double()> refresh_rate;
    // the game window's native handle (an HWND), for which monitor it's on
    std::function<void*()> native_window;
    // Play, once the "Starting" frame is on screen: starts the game, or with
    // `restart` (RestartsForInput) starts band3 again straight into the game
    // and quits this one. Runs inside the launcher's draw, so it defers
    // removing the launcher and building the runtime.
    std::function<void(bool restart)> start_game;
    // quits band3 without starting the game; runs inside the draw too
    std::function<void()> quit;
    // the drawer the RB3 textures are made with (ui/skin.h); null until there is one
    std::function<rex::ui::ImmediateDrawer*()> immediate_drawer;
};

// The launcher: band3's setup screen, shown before the game starts
// (Band3App::OnFinalizePaths): the Settings screen (ui/settings_screen.h) with
// every category, driven with the mouse, the keyboard or a controller. Every
// edit is applied to its cvar at once; leaving or Play saves the overrides to
// band3.toml. The controller tester and the music videos' list show as they
// were until they're screens of their own.
//
// Band3App owns it and removes it itself: ImGuiDialog::Close() deletes the
// dialog it's called on.
class LauncherDialog : public rex::ui::ImGuiDialog {
public:
    // Builds the settings model, so call it before anything sets a setting:
    // the sources the model records then are what lock a setting.
    LauncherDialog(rex::ui::ImGuiDrawer* imgui_drawer, LauncherHost host);
    ~LauncherDialog() override;

    // settings changed since the last save, the startup box too; quitting
    // asks first while there are. Reads the model only, so the window's close
    // request can ask.
    bool HasUnsavedChanges() const;

    // the settings are still being edited: Play hasn't been pressed
    bool IsEditing() const { return stage_ == Stage::kEditing; }

    // the window's close button: asks first while there are unsaved changes,
    // otherwise quits
    void RequestQuit();

protected:
    void OnDraw(ImGuiIO& io) override;

private:
    enum class Stage {
        kEditing,
        // Play was pressed: "Starting" is drawn, then start_game is called
        kStarting,
        kStarted,
    };
    // what the editing stage shows
    enum class View {
        kSettings,
        // the controller tester and the videos' list, as they were
        kTestControllers,
        kVideos,
    };

    // sleeps until the display's next refresh is due
    void Pace();

    // the views from before the redesign, in a window of their own with Back
    void DrawOldView(ImGuiIO& io);
    void DrawStarting(ImGuiIO& io);

    // Play, saved (or kept unsaved): asks first when the game data check
    // fails, then Begin()
    void Play();
    // moves on to the Starting frame
    void Begin();
    // writes band3.toml and logs how it went
    SaveResult Save();

    LauncherHost host_;
    RexCvarStore store_;
    // band3's settings the table doesn't have, from the registry
    std::unique_ptr<GeneratedRows> generated_;
    // the table, then the generated rows
    std::vector<Setting> table_;
    SettingsModel model_;
    Stage stage_ = Stage::kEditing;
    View view_ = View::kSettings;
    int starting_frames_ = 0;
    // Play restarts band3 for a new input backend (RestartsForInput)
    bool restart_ = false;
    // ImGui's ConfigFlags before the launcher turned keyboard navigation on
    int saved_config_flags_ = 0;
    // the last save failed
    bool save_failed_ = false;

    // the controller tester; its readings also drive the navigation
    DevicePanel device_panel_;
    // ImGui's gamepad navigation, for the old views
    std::optional<GamepadNav> nav_;
    VideosTab videos_;

    // the RB3 textures, the Settings screen, and the page of every widget
    // (Ctrl+Shift+G or BAND3_LAUNCHER_GALLERY=1)
    std::unique_ptr<ui::Skin> skin_;
    std::unique_ptr<ui::SettingsScreen> screen_;
    std::unique_ptr<ui::WidgetGallery> gallery_;
    bool show_gallery_ = false;

    // frame pacing
    std::chrono::steady_clock::time_point last_frame_{};
    std::chrono::steady_clock::time_point next_refresh_query_{};
    double refresh_hz_ = 60.0;
};

}
