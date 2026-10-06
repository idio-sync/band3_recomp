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
    // the menu shortcut's Instrument Lab chord (both stick clicks and LB),
    // seen while the settings have the controllers
    std::function<void()> open_instrument_lab;
};

// band3's settings in game: what F4 (bind_settings) and the menu shortcut
// open. A window over the running game with the launcher's tabs and rows
// (settings_page.h, from the same table and model), less what reads the
// devices or the microphones the game has (the device tester, the mic
// meters), plus an Advanced tab and "More settings" sections generated from
// the registry (generated_rows.h). Every change applies to its cvar at once;
// a row whose change waits for the next start says so. Save writes band3.toml
// as the launcher does: only overrides, other keys kept. "All settings..."
// opens the SDK's own settings menu for everything else (the SDK's settings,
// the key binds).
//
// While it's open (or the SDK's menu band3 opened is), the game reads no
// input: it holds the input system's UI blocker, as the Rooms panel does, so
// the keyboard and the controllers drive the settings alone, and reads the
// controllers itself for ImGui's gamepad navigation. It doesn't pause the
// game. It shows nothing while the launcher is up (Band3App doesn't open it).
//
// Always attached; draws nothing while closed. The model is built when it
// first opens and kept for the session, so a change made and not saved is
// still unsaved when it opens again.
class InGameSettingsDialog : public rex::ui::ImGuiDialog {
public:
    InGameSettingsDialog(rex::ui::ImGuiDrawer* imgui_drawer, InGameHost host);
    ~InGameSettingsDialog() override;

    // F4: opens band3's settings; closes them (asking first when something
    // isn't saved); or closes the SDK's menu when that's what's open
    void Toggle();
    bool IsOpen() const { return open_; }

protected:
    void OnDraw(ImGuiIO& io) override;

private:
    void Open();
    // the Close button's path: asks first while there are unsaved changes
    void RequestClose();
    void Close();
    // builds the model the first time the settings open
    void EnsureModel();

    void DrawWindow(ImGuiIO& io);
    void DrawHeader();
    void DrawFooter();
    void DrawPrompts();
    // the note over the SDK's settings menu while it's open
    void DrawAllSettingsNote(ImGuiIO& io);
    void Save();

    // The input the game doesn't get while the settings are open: takes the
    // UI blocker, and turns ImGui's keyboard and gamepad navigation on, fed
    // from the controllers read for the UI. Let go of when they close.
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
    bool open_ = false;
    // the window is new this time: centre it and focus it
    bool appearing_ = false;

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
