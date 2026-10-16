#pragma once
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <imgui.h>
#include "src/Launcher/config_file.h"
#include "src/Launcher/folder_pick.h"
#include "src/Launcher/game_data_check.h"
#include "src/Launcher/launcher_settings.h"
#include "src/Launcher/settings_devices.h"
#include "box.h"
#include "navigation.h"
#include "onscreen_keyboard.h"
#include "settings_rows.h"
#include "skin.h"

// The Settings screen, after Rock Band 3's Music Library, shared by the
// launcher and the in-game settings: the twelve categories on the left with
// how many of each differ from their defaults, the category's sections as
// teal bands with their rows, and a framed detail panel on the right saying
// what the focused row does, its notes and its choices. Long lists open
// beside their row; text and search are typed on the on-screen keyboard in
// the panel's place. Every change applies to its cvar at once; leaving
// saves, and a failed save asks what to do. In game, rows the game took as
// it started are locked, and leaving with changes that wait for a restart
// says so.
//
// Driven by controllers, the keyboard and the mouse alike (navigation.h's
// actions); draws over the whole window.

namespace band3::launcher::ui {

struct SettingsScreenHost {
    Where where = Where::kLauncher;
    // band3.toml, for the Settings file row
    std::function<std::filesystem::path()> config_path;
    // the game data folder the game would start with now (runs with, in game)
    std::function<std::filesystem::path()> game_data_root;
    // the game window's native handle (an HWND), for its display
    std::function<void*()> native_window;
    // writes the model's changes to band3.toml
    std::function<SaveResult()> save;
    // after leaving, saved or kept unsaved: the launcher's menu, the pause menu
    std::function<void()> left;
    // B's name in the help bar: where leaving goes ("Back to menu" without one)
    std::function<std::string()> back_label;
    // Start, F5 or PLAY, once saved (or kept): the launcher plays, in game it's
    // back to the game
    std::function<void()> play;
    // the rows that open other views until they're screens of their own;
    // without one the row isn't shown
    std::function<void()> test_controllers;
    std::function<void()> videos_list;
    // the Game data folder row opens Find Rock Band 3 (the launcher's)
    std::function<void()> find_game;
    std::function<void()> sdk_settings;
    // in game: restarts band3 now, for changes that wait for the next start
    std::function<void()> restart;
};

class SettingsScreen {
public:
    SettingsScreen(SettingsModel& model, Skin& skin, SettingsScreenHost host);
    ~SettingsScreen();
    SettingsScreen(const SettingsScreen&) = delete;
    SettingsScreen& operator=(const SettingsScreen&) = delete;

    // the screen, covering the window; `pads` this frame's controllers.
    // `over_game`: drawn over the running game, which shows dimmed through it
    void Draw(ImGuiIO& io, std::span<const NavPad> pads, bool over_game);

    // starts on `category`'s rows
    void Open(Category category);
    // shown again where it was left: the press that opened it isn't one
    void Show() {
        pads_ = PadActions{};
        shown_frame_ = ImGui::GetFrameCount();
    }
    // leaving as B would on the categories: saves, then `left`
    void RequestLeave();
    // B or Escape, from elsewhere (the pause menu's bind when it isn't Escape)
    void Back() { Handle(Action::kBack); }
    // a box over the screen, which has the input until it's answered
    void ShowBox(Box box);
    bool HasBox() const { return box_.has_value(); }
    // closes the mic meters and lets go of SDL's audio (for Play, or leaving)
    void CloseMeters() { devices_.CloseMeters(); }
    const GameDataCheck& GameData(bool now);

private:
    enum class Focus { kCategories, kRows };
    // what the on-screen keyboard is typing into
    struct Typing {
        bool search = false;
        Row row;
        // what's typed is shown as dots, and starts empty
        bool secret = false;
    };
    // a list open beside its row
    struct DropList {
        Row row;
        std::vector<ChoiceItem> items;
        int selected = 0;
        int scroll = 0;
        // its top, in layout pixels
        float top = 0;
    };
    // what leaving goes on to, once saved
    enum class After { kLeave, kPlay };

    // input
    void Handle(Action action);
    void HandleRows(Action action);
    void HandleCategories(Action action);
    void HandleDrop(Action action);
    void HandleBox(Action action);
    void HandleTyping(OnScreenKeyboard::Result result);
    void TypeFromKeyboard(ImGuiIO& io);

    // what A does on a row
    void Activate(const Row& row);
    void Step(const Row& row, int delta);
    void ResetRow(const Row& row);
    void Apply(const Row& row, const std::string& value);
    void RunControl(const Row& row, int delta = 0);
    void OpenDrop(const Row& row, float top);
    void PickDrop(int index);
    void OpenTyping(const Row& row);
    void OpenSearch();
    void CloseSearch();
    void SetCategory(Category category);
    void Leave(After after);
    void Finish(After after);
    void TakeFolderPick();

    // drawing
    void DrawCategories(ImDrawList* dl);
    void DrawRows(ImDrawList* dl, ImGuiIO& io);
    void DrawPanel(ImDrawList* dl);
    void DrawRowDetail(ImDrawList* dl, const Row& row, float x, float& y, float width);
    void DrawCategoryDetail(ImDrawList* dl, float x, float& y, float width);
    void DrawKeyboard(ImDrawList* dl, float x, float& y, float width);
    void DrawDrop(ImDrawList* dl);
    void DrawStrip(ImDrawList* dl);
    void DrawHelp(ImDrawList* dl, ImGuiIO& io);
    // a paragraph wrapped to `width`, in `color`; moves `y` past it
    void Paragraph(ImDrawList* dl, std::string_view text, float x, float& y, float width,
                   ImU32 color);
    // a status line on the strip for a few seconds
    void Note(std::string text, ImU32 color);

    // the rows showing: the category's, or the search's
    const std::vector<Section>& Sections() const { return sections_; }
    const Row* SelectedRow() const;
    int RowCount() const;
    bool Locked(const Row& row) const;
    // why a locked row can't be changed, for its chip and the strip
    std::string LockedWhy(const Row& row) const;
    // the value as the row shows it, a password as dots while it's typed
    std::string RowValue(const Row& row) const;
    // a layout position (1280x720) in screen pixels
    ImVec2 At(float x, float y) const;

    SettingsModel& model_;
    Skin& skin_;
    SettingsScreenHost host_;
    RowOptions options_;
    SettingsDevices devices_;
    FolderPicker picker_;

    Focus focus_ = Focus::kCategories;
    Category category_ = Category::kGame;
    int row_ = 0;
    // the rows' scroll, in layout pixels
    float scroll_ = 0;
    // scroll so the selected row shows, after a move by the controller or keys
    bool reveal_ = false;
    std::vector<Section> sections_;
    std::optional<std::string> search_;
    std::optional<Typing> typing_;
    OnScreenKeyboard keyboard_;
    std::optional<DropList> drop_;
    std::optional<Box> box_;

    // the input
    PadActions pads_;
    bool keyboard_hints_ = false;
    double now_ = 0;

    // the strip's message, and until when
    std::string note_;
    ImU32 note_color_ = 0;
    std::chrono::steady_clock::time_point note_until_{};
    // the last save failed and the player kept the changes for this session
    std::optional<std::string> kept_unsaved_;
    // Lowest delay's changes, for Y to undo: each setting and what it was
    std::vector<std::pair<std::string, std::string>> undo_;
    // a row's last refused change, until one goes through
    std::map<std::string, std::string, std::less<>> problems_;

    // the Lights' test rows: the kit (by key; every kit when empty) and choices
    std::optional<std::string> lights_target_;
    int lights_colour_ = 0;
    int lights_led_ = 0;
    int lights_pattern_ = 0;
    int lights_fog_ = 0;
    int lights_strobe_ = 0;

    // the game data folder's last check
    std::filesystem::path checked_root_;
    GameDataCheck check_;
    bool check_done_ = false;
    std::chrono::steady_clock::time_point next_check_{};

    // the frame a box, a list or the keyboard opened in: its own click doesn't
    // reach it
    int opened_frame_ = -1;
    // the frame it was shown in (Show): that frame's keys opened it
    int shown_frame_ = -1;
    // a left click this frame, but not the one that opened what's on top
    bool Clicked() const {
        return ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::GetFrameCount() != opened_frame_;
    }

    // this frame's layout, for the mouse: the 1280x720 page's origin and scale
    ImVec2 origin_{};
    float scale_ = 1;
    bool over_game_ = false;
};

}
