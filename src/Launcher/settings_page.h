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
#include <imgui.h>
#include "device_lists.h"
#include "game_data_check.h"
#include "launcher_settings.h"
#include "mic_meter.h"

namespace band3::launcher {

// a folder chosen in the system's folder dialog, which answers on a thread of
// its own; shared with the dialog's callback, which may outlive the page
struct FolderPick;

// What a settings page needs from the app.
struct PageHost {
    // the game data folder the game would start with now, for the Game tab's
    // check; the folder the game runs with, in game
    std::function<std::filesystem::path()> game_data_root;
    // the game window's native handle (an HWND), for which monitor it's on
    std::function<void*()> native_window;
};

// The settings' rows, drawn the same on the launcher (launcher_dialog.h) and
// in game (ingame_settings_dialog.h): a tab's sections of labelled rows, each
// with its control, lock, warnings, notes and Reset; the Steam Deck banner;
// the footer's description of the setting pointed at, and the startup box.
// Every edit goes to its cvar at once (SettingsModel::Set). The page around
// them (tabs, footer buttons, prompts) is the dialog's.
//
// Draw it with the launcher's style and scale (launcher_style.h) applied.
class SettingsPage {
public:
    SettingsPage(SettingsModel& model, Where where, PageHost host);
    ~SettingsPage();
    SettingsPage(const SettingsPage&) = delete;
    SettingsPage& operator=(const SettingsPage&) = delete;

    SettingsModel& Model() { return model_; }
    const SettingsModel& Model() const { return model_; }
    const PageFeatures& Features() const { return features_; }

    // Once a frame, before drawing: takes a folder the folder dialog picked,
    // checks the game data folder again when it's due, and moves on to the
    // setting pointed at in the last frame for the description.
    void BeginFrame();
    // once a frame, after drawing: closes the mic meters, and lets go of SDL's
    // audio, when the mic slots weren't drawn
    void EndFrame();

    // lists the devices `tab` shows again, at most every couple of seconds:
    // listing starts SDL's audio, or walks every display mode, which mustn't
    // happen every frame. Call it for the tab shown, before drawing it.
    void RefreshDeviceLists(Tab tab);

    // the Steam Deck banner, on a Deck
    void DrawDeckBanner();
    // the tab's sections (the Controllers tab's device panel is the dialog's)
    void DrawSections(Tab tab);
    // the setting pointed at: its label, name and description, three lines of
    // small text at most; `idle` draws what shows when nothing is pointed at
    void DrawDescription(const std::function<void()>& idle);
    // the footer's "Show this screen at startup" (show_launcher)
    void DrawStartupBox();
    // the footer's description's height
    static float DescriptionHeight();

    // the setting under the mouse (or the keyboard) is the footer's to describe
    void NoteHovered(std::string_view cvar);

    // the game data folder's check: the last one, or a new one with `now`
    const GameDataCheck& GameData(bool now);
    // closes every mic slot's meter and lets go of SDL's audio (HoldAudio)
    void CloseMeters();

private:
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

    void DrawSection(Tab tab, std::string_view section);
    void DrawRow(const Setting& setting);
    void DrawRowNotes(const Setting& setting);
    // the Graphics tab's Latency section's button: frame_cap and
    // native_present_pacing for the least lag (LowestLatencyCap)
    void DrawLowestLatency();
    void DrawControl(const Setting& setting);

    // the widgets, one per Widget kind
    void DrawCheckbox(const Setting& s);
    void DrawCombo(const Setting& s);
    void DrawComboText(const Setting& s);
    void DrawIntStepper(const Setting& s);
    void DrawIntSlider(const Setting& s);
    void DrawFloatSlider(const Setting& s, bool percent);
    void DrawFloatInput(const Setting& s);
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
    // keeps band3's SDL audio subsystem started while the mic slots show, so
    // listing the microphones every couple of seconds doesn't start and stop
    // it (and its device enumeration) each time, and SDL sees them come and go
    void HoldAudio(bool hold);
    void DrawJoypadLag(const Setting& s);
    void DrawWindowMode(const Setting& s);
    void DrawGameDataCheck();
    // checks the game data folder again when it has changed, when a second
    // has passed (the folder can appear or go while the page is open), or
    // when `now` asks
    void RefreshGameDataCheck(bool now);
    // whether a song folder exists, from a check at most a few seconds old: a
    // folder on a network share that doesn't answer can take seconds to say
    // so, which mustn't happen every frame
    bool FolderExists(const std::filesystem::path& folder);

    // a one-line text field over `value`, applied when the field is left (or
    // Enter is pressed): returns true then, with `value` the new text
    bool EditText(const char* id, const char* hint, std::string& value, float width);
    // sets a row's setting; a value its cvar refuses is named under the row
    // rather than dropped without a word. False if it was refused.
    bool Apply(std::string_view cvar, std::string_view value);

    void StartFolderPick(std::string target, const std::string& from);
    void TakeFolderPick();

    SettingsModel& model_;
    Where where_;
    PageFeatures features_;
    PageHost host_;

    // the setting the footer describes: the one hovered in the last frame,
    // and the one hovered in this one so far
    std::string hovered_;
    std::string hovered_next_;
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
    // counts listings of monitors_, for resolution_lists_
    unsigned monitors_generation_ = 0;
    // a resolution dropdown entry (DrawResolution)
    struct ResolutionEntry {
        std::string label;
        std::string value;
        int width = 0, height = 0;
    };
    // DrawResolution's entries for a monitor: its sizes, once each, then the
    // presets it doesn't have; made again when the monitors are listed again
    // or another monitor is chosen, rather than every frame
    struct ResolutionLists {
        unsigned monitors_generation = 0;
        int monitor = 0;
        std::vector<ResolutionEntry> modes;
        std::vector<ResolutionEntry> presets;
    };
    std::optional<ResolutionLists> resolution_lists_;
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
};

}
