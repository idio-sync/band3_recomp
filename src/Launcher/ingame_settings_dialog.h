#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>
#include "src/Input/instruments.h"
#include "src/Input/menu_shortcut.h"
#include "device_view_model.h"
#include "gamepad_nav.h"
#include "generated_rows.h"
#include "launcher_cvars.h"
#include "launcher_settings.h"
#include "ui/pause_screen.h"
#include "ui/settings_screen.h"
#include "ui/videos_screen.h"
#include "ui/skin.h"

namespace band3::launcher {

// What the in-game settings need from the app (Band3App).
struct InGameHost {
    // Known once OnFinalizePaths has the folders, after the dialog is made, so
    // asked for when the settings first open:
    // band3.toml, where Save writes
    std::function<std::filesystem::path()> config_path;
    // the folders the folder settings fall back to (OnFinalizePaths' defaults)
    std::function<PathDefaults()> path_defaults;
    // what relative folders are relative to (IniAnchor)
    std::function<std::filesystem::path()> anchor;
    // the game data folder the game runs with
    std::function<std::filesystem::path()> game_data_root;
    // the game window's native handle (an HWND), for which monitor it's on
    std::function<void*()> native_window;
    // "Every SDK setting": the SDK's own settings menu, which band3 opens and
    // closes. Opening or closing a dialog can't happen while the dialogs draw,
    // so the app defers both.
    std::function<void()> open_all_settings;
    std::function<void()> close_all_settings;
    std::function<bool()> all_settings_open;
    // the pause menu's Instrument Lab, and the menu shortcut's Instrument Lab
    // chord (both stick clicks and LB), seen while the menu has the
    // controllers
    std::function<void()> open_instrument_lab;
    // the pause menu's Quit game: closes band3
    std::function<void()> quit;
    // the drawer the RB3 textures are made with (ui/skin.h)
    std::function<rex::ui::ImmediateDrawer*()> immediate_drawer;
};

// band3's pause menu, and band3's settings in game, which its Settings opens.
// Escape (bind_pause_menu) and the menu shortcut open the pause menu: Resume,
// Settings, Instrument Lab and Quit game. During a song it pauses the song
// under it, as the player's Start would (src/Hooks/song_pause.h), and Resume
// resumes it; elsewhere the game keeps running underneath.
//
// The settings are the launcher's Settings screen (ui/settings_screen.h) over
// the dimmed game, from the same table and model, less what reads the devices
// or the microphones the game has (the controller tester, the mic meters).
// Every change applies to its cvar at once; a row whose change waits for the
// next start says so, and the folders the game took as it started are locked.
// Leaving saves band3.toml as the launcher does: only overrides, other keys
// kept. Start goes straight back to the game.
//
// While any of it is open (the SDK's menu too, when band3 opened it), the
// game reads no input: it holds the input system's UI blocker, as the Rooms
// panel does, so the keyboard and the controllers drive the menus alone, read
// by the menus themselves. It shows nothing while the launcher is up
// (Band3App doesn't open it).
//
// Always attached; draws nothing while closed. The settings' model is built
// when they first open and kept for the session, so a change kept unsaved is
// still unsaved when they open again.
class InGameSettingsDialog : public rex::ui::ImGuiDialog {
public:
    InGameSettingsDialog(rex::ui::ImGuiDrawer* imgui_drawer, InGameHost host);
    ~InGameSettingsDialog() override;

    // Escape: opens the pause menu, or goes back a step: from the pause menu
    // to the game, from the settings to the pause menu (saving), from the
    // SDK's menu to the pause menu
    void TogglePause();

protected:
    void OnDraw(ImGuiIO& io) override;

private:
    // what's showing
    enum class View {
        kClosed,
        kPause,
        kSettings,
        // the Music Videos screen, over the game
        kVideos,
        // the SDK's settings menu, which band3 opened: only the note over it
        kAllSettings,
    };
    void SetView(View view);

    // opens the pause menu, pausing a song
    void OpenPause();
    // closes the pause menu, resuming the song it paused
    void Resume();
    void OpenSettings();
    // builds the model the first time the settings open
    void EnsureModel();
    SaveResult Save();

    // the note over the SDK's settings menu while it's open
    void DrawAllSettingsNote(ImGuiIO& io);

    // The input the game doesn't get while a menu is open: takes the UI
    // blocker, so the menus read the keyboard and the controllers alone. Let
    // go of when it closes.
    void HoldInput(ImGuiIO& io, bool hold);
    // every player's controller as navigation, and the shortcut's buttons
    // (controllers only: an instrument's solo frets are stick clicks)
    struct PadReading {
        std::vector<NavPad> nav;
        uint16_t chord_buttons = 0;
    };
    PadReading ReadPads();

    InGameHost host_;
    // the host's config_path, from when the settings first opened
    std::filesystem::path config_path_;
    View view_ = View::kClosed;
    // ImGui's frame when the view changed, whose Escape opened it
    int view_frame_ = 0;
    // the window is new this time: centre it and focus it
    bool appearing_ = false;
    // the bind was pressed while a menu was open: go back once the frame
    // knows whether a prompt had Escape
    bool back_requested_ = false;
    // kAllSettings: the SDK's menu has opened since it was asked for
    bool all_settings_seen_ = false;

    RexCvarStore store_;
    std::unique_ptr<GeneratedRows> generated_;
    // the launcher's table, then the generated rows
    std::vector<Setting> table_;
    std::optional<SettingsModel> model_;
    std::unique_ptr<ui::Skin> skin_;
    std::unique_ptr<ui::PauseScreen> pause_;
    std::unique_ptr<ui::SettingsScreen> screen_;
    std::unique_ptr<ui::VideosScreen> videos_;

    // HoldInput's state
    bool input_held_ = false;
    bool blocker_taken_ = false;
    input::MenuShortcut shortcut_;
    // each player's controller type (XINPUT SubType), read every couple of
    // seconds: reading it makes the SDK look for devices again
    std::array<std::optional<input::Caps360>, 4> caps_{};
    std::chrono::steady_clock::time_point next_caps_{};
};

}
