#pragma once
#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <rex/ui/imgui_dialog.h>
#include "device_lists.h"
#include "game_data_check.h"
#include "gamepad_nav.h"
#include "instrument_view.h"
#include "launcher_cvars.h"
#include "launcher_settings.h"
#include "mic_meter.h"

namespace band3::launcher {

// a folder chosen in the system's folder dialog, which answers on a thread of
// its own; shared with the dialog's callback, which may outlive the launcher
struct FolderPick;

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
// keyboard or a controller (gamepad_nav.h). Tabs of settings (launcher_settings.h's table) over a footer with
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

    // a text field's text while it's being edited
    struct TextField {
        std::vector<char> text;
        bool editing = false;
    };

    // an entry in a device dropdown: what it shows, and what picking it saves
    struct DeviceEntry {
        std::string label;
        std::string value;
    };

    // a mic slot's level meter, while the Audio tab shows it (DrawMicSlots)
    struct SlotMeter {
        std::unique_ptr<MicMeter> meter;
        // the slot's setting it records ("" for the default microphone)
        std::string device;
        // mic_list_generation_ when it was opened
        unsigned generation = 0;
    };

    // sleeps until the display's next refresh is due
    void Pace();

    void DrawPage(ImGuiIO& io);
    void DrawHeader();
    void DrawBanners();
    void DrawDeckBanner();
    void DrawTab(Tab tab);
    void DrawSection(Tab tab, std::string_view section);
    void DrawRow(const Setting& setting);
    void DrawControl(const Setting& setting);
    void DrawFooter();
    void DrawPrompts();
    void DrawStarting(ImGuiIO& io);
    // a controller's Start plays, and its bumpers switch tabs
    void HandleNav(const NavEdges& edges);

    // the widgets, one per Widget kind
    void DrawCheckbox(const Setting& s);
    void DrawCombo(const Setting& s);
    void DrawComboText(const Setting& s);
    void DrawIntStepper(const Setting& s);
    void DrawIntSlider(const Setting& s);
    void DrawFloatSlider(const Setting& s, bool percent);
    void DrawText(const Setting& s);
    void DrawPath(const Setting& s);
    void DrawFolderList(const Setting& s);
    void DrawMicSlots(const Setting& s);
    void DrawMidiPort(const Setting& s);
    void DrawMonitor(const Setting& s);
    void DrawResolution(const Setting& s);
    // A dropdown over a setting that names a device (a mic slot, the MIDI
    // port): `none` (the empty value) first, then `devices`, then the saved
    // name as "(not connected)" when it selects none of them, then "Other..."
    // for typing a name or part of one. `selected` is the device the saved
    // name picks. Returns true with `value` set when the player picks or types
    // another. `key` keeps the row's typed mode (custom_rows_).
    bool DeviceCombo(std::string_view key, const std::string& none,
                     const std::vector<DeviceEntry>& devices, std::optional<size_t> selected,
                     std::string& value, const char* empty_list, float width);
    // a mic slot's meter: opens, keeps or closes it for `device` (nullopt for
    // none), and draws it `width` wide where the cursor is
    void DrawMicMeter(int slot, const std::optional<std::string>& device, float width);
    // closes every mic slot's meter and lets go of SDL's audio (HoldAudio)
    void CloseMeters();
    // keeps band3's SDL audio subsystem started while the mic slots show, so
    // listing the microphones every couple of seconds doesn't start and stop
    // it (and its device enumeration) each time, and SDL sees them come and go
    void HoldAudio(bool hold);
    // lists the devices again for the tab that shows them, at most every
    // couple of seconds: listing starts SDL's audio, or walks every display
    // mode, which mustn't happen every frame
    void RefreshDeviceLists();
    void DrawJoypadLag(const Setting& s);
    void DrawWindowMode(const Setting& s);
    void DrawGameDataCheck();
    // checks the game data folder again when it has changed, when a second
    // has passed (the folder can appear or go while the launcher is open), or
    // when `now` asks
    void RefreshGameDataCheck(bool now);
    // whether a song folder exists, from a check at most a few seconds old: a
    // folder on a network share that doesn't answer can take seconds to say
    // so, which mustn't happen every frame
    bool FolderExists(const std::filesystem::path& folder);

    // a one-line text field over `value`, applied when the field is left (or
    // Enter is pressed): returns true then, with `value` the new text
    bool EditText(const char* id, const char* hint, std::string& value, float width);
    // the setting under the mouse (or the keyboard) is the footer's to describe
    void NoteHovered(std::string_view cvar);
    // sets a row's setting; a value its cvar refuses is named under the row
    // rather than dropped without a word. False if it was refused.
    bool Apply(std::string_view cvar, std::string_view value);

    void StartFolderPick(std::string target, const std::string& from);
    void TakeFolderPick();

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
    // the setting the footer describes: the one hovered in the last frame,
    // and the one hovered in this one so far
    std::string hovered_;
    std::string hovered_next_;
    // band3.toml couldn't be read when the launcher opened
    std::optional<std::string> config_problem_;
    // the last save's outcome, for the footer
    std::string save_message_;
    bool save_failed_ = false;
    // why the last save failed, for the "Play anyway?" prompt
    std::string save_error_;
    // what went wrong with a row's last change, shown under it until one goes
    // through: a value its cvar refused, or a folder dialog that didn't open
    std::map<std::string, std::string, std::less<>> row_problems_;

    // the text fields' buffers while they're being edited
    std::map<unsigned, TextField> text_fields_;
    // combo-with-text rows showing their text field (device rows by
    // DeviceCombo's key)
    std::set<std::string, std::less<>> custom_rows_;

    // the devices on this PC, listed again every couple of seconds while
    // their tab shows (RefreshDeviceLists), and when each list is due
    std::vector<std::string> mics_;
    std::vector<MidiPort> midi_ports_;
    std::vector<Monitor> monitors_;
    bool mics_listed_ = false;
    bool midi_listed_ = false;
    std::chrono::steady_clock::time_point next_mics_{};
    std::chrono::steady_clock::time_point next_midi_{};
    std::chrono::steady_clock::time_point next_monitors_{};
    // counts changes to mics_, so a meter that couldn't record tries again
    // once the microphones change
    unsigned mic_list_generation_ = 0;
    std::array<SlotMeter, 4> meters_;
    // the mic slots were drawn this frame; their meters close, and SDL's
    // audio is let go, when they aren't
    bool meters_drawn_ = false;
    // HoldAudio started SDL's audio subsystem, and hasn't stopped it
    bool audio_held_ = false;
    // it couldn't be started; not tried again until the slots show again
    bool audio_hold_failed_ = false;
    // content_folders' "add a folder" field
    std::string new_folder_;

    std::shared_ptr<FolderPick> pick_;
    // the setting the pick is for
    std::string pick_target_;

    // the last check of the game data folder (RefreshGameDataCheck)
    std::filesystem::path checked_root_;
    GameDataCheck check_;
    bool check_done_ = false;
    std::chrono::steady_clock::time_point next_check_{};

    // FolderExists' answers, and when each was found
    struct FolderState {
        bool exists = false;
        std::chrono::steady_clock::time_point checked;
    };
    std::map<std::filesystem::path, FolderState> folder_states_;
    // the song folder list they're for; a new list checks its folders again
    std::string folder_states_for_;

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
