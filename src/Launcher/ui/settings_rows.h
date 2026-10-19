#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "src/Launcher/device_lists.h"
#include "src/Launcher/launcher_settings.h"

// The Settings screen's rows, apart from drawing them: what a category lists
// (its settings' rows, a setting that opens into several rows, and the rows
// that act rather than set), the value each shows, the choices its list has,
// what left and right change it to, and search across every category. Pure:
// the model holds the values, DeviceLists what's plugged in.

namespace band3::launcher::ui {

// the devices the rows that pick one list, as the screen last found them
struct DeviceLists {
    std::vector<std::string> mics;
    std::vector<MidiPort> midi_ports;
    // empty where they can't be listed: then the setting's choices number them
    std::vector<Monitor> monitors;
    // the controllers that could play, for left_out_controllers
    struct Controller {
        std::string name;
        // input::LeftOutKey
        std::string key;
        // 1-4, 0 for none
        int player = 0;
        bool left_out = false;
        // a Steam Deck's own controls, left out by leave_out_deck_controls
        bool deck_aside = false;
    };
    std::vector<Controller> controllers;
};

enum class RowKind {
    kSetting,
    // one singer's microphone: a part of usb_mic_devices (`index` 0 to 2)
    kMicSlot,
    // one controller type's delay: a part of joypad_lag (`index` into
    // LagEditorTypes)
    kLagType,
    // does something rather than set something (`control`)
    kControl,
};

enum class Control {
    // shows where band3.toml is, and A opens its folder
    kSettingsFile,
    // frame_cap and native_present_pacing for the least delay (LowestLatencyCap)
    kLowestDelay,
    kTestControllers,
    kVideosList,
    // the Lights' test rows: which kit, then what to send it
    kLightsDevice,
    kLightsCheck,
    // how long the colour check holds each colour
    kLightsHold,
    kFog,
    kStrobe,
    kLightsColour,
    kLeds,
    kPattern,
    kChase,
    kLightsOff,
    // the SDK's own settings dialog (in game)
    kSdkSettings,
};

struct Row {
    RowKind kind = RowKind::kSetting;
    // kSetting's setting, or the one kMicSlot and kLagType are parts of
    const Setting* setting = nullptr;
    int index = 0;
    Control control = Control::kSettingsFile;
    std::string label;
};

struct Section {
    std::string name;
    // a line under the band (SettingsModel::SectionNote)
    std::optional<std::string> note;
    std::vector<Row> rows;
};

struct RowOptions {
    Where where = Where::kLauncher;
    // a row that opens the SDK's settings dialog, where the host can
    bool sdk_settings = false;
};

// the category's sections with the rows visible now; sections without any
// are left out
std::vector<Section> CategoryRows(const SettingsModel& model, Category category,
                                  const RowOptions& options);
// every category's rows whose label, description, section or setting name has
// `query` (ignoring case), a section per category; none for an empty query
std::vector<Section> SearchRows(const SettingsModel& model, std::string_view query,
                                const RowOptions& options);

// a row's value as the screen shows it: "On", "8 frames", "Game's (2x)"
std::string ValueText(const SettingsModel& model, const Row& row, const DeviceLists& lists);

struct ChoiceItem {
    std::string label;
    // what picking it applies (ApplyValue)
    std::string value;
    bool current = false;
    bool is_default = false;
};
// A list row's choices, in order, the current one marked (a value typed in
// that none of them is comes last, as itself). Empty for a row without a
// list: a number, text, a folder.
std::vector<ChoiceItem> Choices(const SettingsModel& model, const Row& row,
                                const DeviceLists& lists);
// The value left (-1) or right (+1) changes the row to: the list's choice
// before or after, the number a step down or up. nullopt at the list's or
// the range's end, and for a row without steps (text, folders, controls).
std::optional<std::string> Stepped(const SettingsModel& model, const Row& row, int delta,
                                   const DeviceLists& lists);
// sets the row to `value`: the setting's, or its part of usb_mic_devices or
// joypad_lag; the window mode's two settings by "windowed", "borderless" or
// "exclusive"
bool ApplyValue(SettingsModel& model, const Row& row, std::string_view value);
// differs from its default (a lag type's: set at all)
bool RowChanged(const SettingsModel& model, const Row& row);
bool ResetRow(SettingsModel& model, const Row& row);
// A picks from a list beside the row rather than stepping through it: the
// devices, the window sizes, a list with room for a value typed in, a long one
bool OpensList(const Row& row);
// A types the value on the on-screen keyboard
bool TypesText(const Row& row);

}
