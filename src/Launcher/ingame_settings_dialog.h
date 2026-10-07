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
#include "src/Input/instruments.h"
#include "src/Input/menu_shortcut.h"
#include "device_view_model.h"
#include "gamepad_nav.h"
#include "generated_rows.h"
#include "launcher_cvars.h"
#include "launcher_settings.h"
#include "settings_page.h"

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
    // "All settings...": the SDK's own settings menu, which band3 opens and
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
};

// band3's pause menu, and band3's settings in game, which its Settings opens.
// Escape (bind_pause_menu) and the menu shortcut open the pause menu: Resume,
// Settings, Instrument Lab and Quit game. During a song it pauses the song
// under it, as the player's Start would (src/Hooks/song_pause.h), and Resume
// resumes it; elsewhere the game keeps running underneath.
//
// The settings are a window over the game with the launcher's tabs and rows
// (settings_page.h, from the same table and model), less what reads the
// devices or the microphones the game has (the device tester, the mic
// meters), plus an Advanced tab and "More settings" sections generated from
// the registry (generated_rows.h). Every change applies to its cvar at once;
// a row whose change waits for the next start says so. Save writes band3.toml
// as the launcher does: only overrides, other keys kept. "All settings..."
// opens the SDK's own settings menu for everything else (the SDK's settings,
// the key binds).
//
// Closing the settings goes back to the pause menu, as does closing the SDK's.
//
// While any of them is open (the SDK's menu too, when band3 opened it), the
// game reads no input: it holds the input system's UI blocker, as the Rooms
// panel does, so the keyboard and the controllers drive the menus alone, and
// reads the controllers itself for ImGui's gamepad navigation. It shows
// nothing while the launcher is up (Band3App doesn't open it).
//
// Always attached; draws nothing while closed. The settings' model is built
// when they first open and kept for the session, so a change made and not
// saved is still unsaved when they open again.
class InGameSettingsDialog : public rex::ui::ImGuiDialog {
public:
    InGameSettingsDialog(rex::ui::ImGuiDrawer* imgui_drawer, InGameHost host);
    ~InGameSettingsDialog() override;

    // Escape: opens the pause menu, or goes back a step: from the pause menu
    // to the game, from the settings to the pause menu (asking first when
    // something isn't saved), from the SDK's menu to the pause menu
    void TogglePause();

protected:
    void OnDraw(ImGuiIO& io) override;

private:
    // what's showing
    enum class View {
        kClosed,
        kPause,
        kSettings,
        // the SDK's settings menu, which band3 opened: only the note over it
        kAllSettings,
    };
    void SetView(View view);

    // opens the pause menu, pausing a song
    void OpenPause();
    // closes the pause menu, resuming the song it paused
    void Resume();
    void OpenSettings();
    // the settings' Back: asks first while there are unsaved changes
    void RequestCloseSettings();
    // back to the pause menu
    void CloseSettings();
    // B, Escape or the bind went back, not with the press that opened the
    // view (consumes the bind's)
    bool BackPressed();
    // builds the model the first time the settings open
    void EnsureModel();

    void DrawPauseWindow(ImGuiIO& io);
    // `back`: B or Escape this frame, which cancels it
    void DrawQuitPrompt(bool back);
    void DrawWindow(ImGuiIO& io);
    void DrawHeader();
    void DrawFooter();
    void DrawPrompts();
    // the note over the SDK's settings menu while it's open
    void DrawAllSettingsNote(ImGuiIO& io);
    void Save();

    // The input the game doesn't get while a menu is open: takes the UI
    // blocker, and turns ImGui's keyboard and gamepad navigation on, fed from
    // the controllers read for the UI. Let go of when it closes.
    void HoldInput(ImGuiIO& io, bool hold);
    // every player's controller as ImGui navigation, and the shortcut's
    // buttons (controllers only: an instrument's solo frets are stick clicks)
    struct PadReading {
        std::vector<NavPad> nav;
        uint16_t chord_buttons = 0;
    };
    PadReading ReadPads();
    void HandleNav(const NavEdges& edges);

    InGameHost host_;
    // the host's config_path, from when the settings first opened
    std::filesystem::path config_path_;
    View view_ = View::kClosed;
    // ImGui's frame when the view changed, whose Escape opened it
    int view_frame_ = 0;
    // the window is new this time: centre it and focus it
    bool appearing_ = false;
    // the bind was pressed while a menu was open: go back once the frame
    // knows whether a dropdown or a field had Escape
    bool back_requested_ = false;
    // kAllSettings: the SDK's menu has opened since it was asked for
    bool all_settings_seen_ = false;
    bool open_quit_prompt_ = false;

    RexCvarStore store_;
    std::unique_ptr<GeneratedRows> generated_;
    // the launcher's table, then the generated rows
    std::vector<Setting> table_;
    std::optional<SettingsModel> model_;
    std::optional<SettingsPage> page_;
    // band3.toml couldn't be read when the settings first opened
    std::optional<std::string> config_problem_;

    Tab current_tab_ = Tab::kGame;
    std::optional<Tab> pending_tab_;
    // the tab the settings were last scrolled for
    std::optional<Tab> scrolled_tab_;
    // the last save's outcome, for the footer
    std::string save_message_;
    bool save_failed_ = false;
    bool open_close_prompt_ = false;
    // a dropdown was open or a field was being edited at the end of the last
    // frame: B and Escape closed that, not the settings
    bool busy_last_frame_ = false;

    // HoldInput's state
    bool input_held_ = false;
    bool blocker_taken_ = false;
    int saved_config_flags_ = 0;
    std::optional<GamepadNav> nav_;
    input::MenuShortcut shortcut_;
    // each player's controller type (XINPUT SubType), read every couple of
    // seconds: reading it makes the SDK look for devices again
    std::array<std::optional<input::Caps360>, 4> caps_{};
    std::chrono::steady_clock::time_point next_caps_{};
};

}
