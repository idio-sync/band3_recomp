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
#include "instrument_view.h"
#include "launcher_cvars.h"
#include "launcher_settings.h"
#include "settings_page.h"

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
};

// The launcher: band3's setup screen, one full-window ImGui page shown before
// the game starts (Band3App::OnFinalizePaths), driven with the mouse, the
// keyboard or a controller (gamepad_nav.h). Tabs of settings (settings_page.h's rows
// of launcher_settings.h's table) over a footer with
// the hovered setting's description and Save, Close and Play. Every edit is
// applied to its cvar at once; Save writes the overrides to band3.toml.
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

    // the Close button's path, and the window's close button's: asks "Quit
    // without saving?" while there are unsaved changes, otherwise quits
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

    // sleeps until the display's next refresh is due
    void Pace();

    void DrawPage(ImGuiIO& io);
    void DrawHeader();
    void DrawBanners();
    void DrawFooter();
    void DrawPrompts();
    void DrawStarting(ImGuiIO& io);
    // a controller's Start plays, and its bumpers switch tabs
    void HandleNav(const NavEdges& edges);

    // Play: asks first when the game data check fails, then Start()
    void Play();
    // saves, then starts the game; when saving fails, asks whether to play
    // anyway with the settings as they are
    void Start();
    // moves on to the Starting frame
    void Begin();
    // writes band3.toml (show_launcher from the footer's box too) and reports
    // how it went in the footer
    bool Save();

    LauncherHost host_;
    RexCvarStore store_;
    SettingsModel model_;
    // the tabs' rows, the Deck banner, the description and the startup box
    SettingsPage page_;
    Stage stage_ = Stage::kEditing;
    int starting_frames_ = 0;
    // Play restarts band3 for a new input backend (RestartsForInput)
    bool restart_ = false;
    // ImGui's ConfigFlags before the launcher turned keyboard navigation on
    int saved_config_flags_ = 0;

    Tab current_tab_ = Tab::kGame;
    // the tab a controller's bumper switched to, selected at the next frame
    std::optional<Tab> pending_tab_;
    // the Controllers tab's device list and test view; its readings also
    // drive the gamepad navigation
    DevicePanel device_panel_;
    std::optional<GamepadNav> nav_;
    // band3.toml couldn't be read when the launcher opened
    std::optional<std::string> config_problem_;
    // the last save's outcome, for the footer
    std::string save_message_;
    bool save_failed_ = false;
    // why the last save failed, for the "Play anyway?" prompt
    std::string save_error_;

    // frame pacing
    std::chrono::steady_clock::time_point last_frame_{};
    std::chrono::steady_clock::time_point next_refresh_query_{};
    double refresh_hz_ = 60.0;

    // popups to open on the next frame (OpenPopup has to run inside the window)
    bool open_quit_prompt_ = false;
    bool open_play_prompt_ = false;
    bool open_save_failed_prompt_ = false;
};

}
