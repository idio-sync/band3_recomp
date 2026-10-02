#pragma once
#include <filesystem>
#include <functional>
#include <rex/ui/imgui_dialog.h>
#include "game_data_check.h"

namespace band3::launcher {

// What the launcher needs from the app (Band3App).
struct LauncherHost {
    // the game data folder the game would start with now
    std::function<std::filesystem::path()> game_data_root;
    // Play, once the "Starting" frame is on screen: starts the game. Runs inside
    // the launcher's draw, so it defers removing the launcher and building the
    // runtime.
    std::function<void()> start_game;
    // quits band3 without starting the game; runs inside the draw too
    std::function<void()> quit;
};

// The launcher: band3's setup screen, one full-window ImGui page shown before
// the game starts (Band3App::OnFinalizePaths), driven with the mouse or the
// keyboard. For now it shows the game data folder and its check, with Close
// and Play; the settings tabs come later.
//
// Band3App owns it and removes it itself: ImGuiDialog::Close() deletes the
// dialog it's called on.
class LauncherDialog : public rex::ui::ImGuiDialog {
public:
    LauncherDialog(rex::ui::ImGuiDrawer* imgui_drawer, LauncherHost host);
    ~LauncherDialog() override;

    // settings changed since the last save; quitting asks first while there are
    bool HasUnsavedChanges() const;

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

    void DrawPage(ImGuiIO& io);
    void DrawGameData();
    void DrawFooter();
    void DrawPrompts();
    void DrawStarting(ImGuiIO& io);

    // Play: asks first when the game data check fails, then Start()
    void Play();
    // saves, then moves on to the Starting frame; false if saving failed
    bool Start();
    // writes band3.toml; a later task fills this in
    bool Save();

    LauncherHost host_;
    Stage stage_ = Stage::kEditing;
    int starting_frames_ = 0;
    // ImGui's ConfigFlags before the launcher turned keyboard navigation on
    int saved_config_flags_ = 0;

    // the last check of the game data folder, redone when the folder changes
    std::filesystem::path checked_root_;
    GameDataCheck check_;
    bool check_done_ = false;

    // popups to open on the next frame (OpenPopup has to run inside the window)
    bool open_quit_prompt_ = false;
    bool open_play_prompt_ = false;
};

// whether Shift is held right now; Windows only (false elsewhere)
bool ShiftHeld();

}
