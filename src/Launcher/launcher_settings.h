#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "config_file.h"

// The launcher's settings: the curated table of what each tab shows, and the
// override model that decides what Save writes to band3.toml.
//
// The launcher writes only overrides: a setting goes into band3.toml when its
// value differs from its effective default (what it would be with no key
// there), and its key is removed otherwise. The model is pure: it reads and
// sets values through a CvarStore and is told everything else (types,
// defaults, sources) in an Environment, which launcher_cvars.h fills in from
// the SDK in the game and tests write by hand.

namespace band3::launcher {

enum class Tab {
    kGame,
    kGraphics,
    kAudio,
    kControllers,
    kOnline,
    // the Steam Deck banner's toggle, shown only on a Deck
    kSteamDeck,
    // the footer's "Show this screen at startup"
    kFooter,
};

enum class Widget {
    kCheckbox,
    // one of the choices
    kCombo,
    // one of the choices, or a typed value (resolution's WxH, forced_venue's lists)
    kComboText,
    // a number with - and + buttons; choices, if any, name special values
    kIntStepper,
    kIntSlider,
    kFloatSlider,
    // a 0..1 fraction shown as 0..100%
    kPercentSlider,
    kText,
    // a folder: an editable path with Browse...; empty means the default folder
    kPath,
    // folders separated by '|' (paths::SplitList), shown resolved
    kFolderList,
    // up to four names, comma separated, one per mic slot (ParseDeviceList)
    kMicSlots,
    // one game lag number per controller type (LagEditorTypes, WithLag)
    kJoypadLag,
    // windowed / borderless / exclusive, over this setting (fullscreen) and its
    // companion (fullscreen_exclusive); see GetWindowMode and SetWindowMode
    kWindowMode,
    // not drawn: another row's widget sets it (the companion)
    kNone,
};

struct Choice {
    std::string_view value;
    std::string_view label;
};

struct Range {
    double min = 0;
    double max = 0;
    double step = 1;
};

// shown only while `cvar` has `value`, compared by type; an empty cvar is always
struct Condition {
    std::string_view cvar;
    std::string_view value;
};

struct Setting {
    std::string_view cvar;
    Tab tab;
    std::string_view section;
    std::string_view label;
    Widget widget;
    std::span<const Choice> choices = {};
    std::optional<Range> range = {};
    // after the number, e.g. "ms"
    std::string_view unit = {};
    Condition shown_when = {};
    // set by this row's widget too, and reset with it
    std::string_view companion = {};
    // input_backend's xinput is Windows only
    bool windows_only = false;
};

// every setting the launcher shows, in display order: tab, then section, then row
std::span<const Setting> SettingTable();
const Setting* FindSetting(std::span<const Setting> table, std::string_view cvar);
// a tab's sections, in order
std::vector<std::string_view> SectionsOf(std::span<const Setting> table, Tab tab);

// Values

enum class ValueType { kBool, kInt, kFloat, kString };

// as the SDK reads them: true, 1 and yes are true, anything else false
bool AsBool(std::string_view value);
// a leading integer, as the SDK's int setters read it
std::optional<int64_t> AsInt(std::string_view value);
std::optional<double> AsFloat(std::string_view value);

// whether a and b are the same value of `type`: 1.000000 is 1, yes is true
bool SameValue(ValueType type, std::string_view a, std::string_view b);

// A folder as the launcher writes it: relative to the anchor when it's inside
// it, absolute otherwise, always with forward slashes. Empty stays empty.
std::string PathForConfig(std::string_view value, const std::filesystem::path& anchor);
// each of a '|' list's folders as PathForConfig writes it, blanks dropped
std::string FolderListForConfig(std::string_view value, const std::filesystem::path& anchor);

// usb_mic_devices from the mic slots: comma separated, blank slots after the
// last named one dropped (ParseDeviceList splits it again)
std::string JoinMicSlots(std::span<const std::string> slots);

// joypad_lag's editor

struct LagType {
    uint32_t type;
    std::string_view label;
};

// the controller types the editor offers a number for
std::span<const LagType> LagEditorTypes();
// the game lag joypad_lag (`text`) sets for `type`, if it sets one
std::optional<float> LagFor(std::string_view text, uint32_t type);
// `text` with `type`'s game lag set to `ms`, or cleared with nullopt. The
// type's video and audio calibration parts, other types' entries and entries
// it can't read are kept; an entry left with no parts is dropped.
std::string WithLag(std::string_view text, uint32_t type, std::optional<float> ms);

enum class WindowMode { kWindowed, kBorderless, kExclusive };

// The model

// where a setting's value came from when the launcher opened, if a saved value
// couldn't change it
enum class Lock { kNone, kCommandLine, kEnvironment };

// "Set on the command line", ...; empty for kNone
const char* LockReason(Lock lock);

// What the model knows about one cvar, besides its value.
struct CvarFacts {
    // registered in this build; the launcher hides a setting whose cvar isn't
    // and leaves its key alone
    bool exists = false;
    ValueType type = ValueType::kString;
    // the registry's, for the footer
    std::string description;
    std::optional<double> min;
    std::optional<double> max;
    std::vector<std::string> allowed;

    // The effective default's layers, first that applies wins:
    // band3 sets this at startup whatever else does (input_backend = sdl off Windows)
    std::optional<std::string> startup_forced;
    // the Steam Deck preset (steam_deck::Preset), on a Deck with steam_deck_defaults on
    std::optional<std::string> deck_preset;
    // band3_config.ini's value (LegacyIniValue); ignored if the cvar wouldn't take it
    std::optional<std::string> ini;
    // band3's own default, set at startup when nothing else did (audio_maxqframes = 3)
    std::optional<std::string> startup_default;
    std::string registry_default;
    // a folder setting's default instead of all the above: the folder the game
    // starts with when the setting is empty (OnFinalizePaths' defaults)
    std::optional<std::filesystem::path> path_default;

    // where the value came from when the launcher opened
    Lock lock = Lock::kNone;
};

struct Environment {
    std::map<std::string, CvarFacts, std::less<>> cvars;
    bool steam_deck = false;
#ifdef _WIN32
    bool windows = true;
#else
    bool windows = false;
#endif
    // what relative folders are relative to (IniAnchor)
    std::filesystem::path anchor;
};

// the cvars' live values; the game's sets them with SetFlagByName
class CvarStore {
public:
    virtual ~CvarStore() = default;
    virtual std::string Get(std::string_view name) const = 0;
    // false when the cvar refuses the value
    virtual bool Set(std::string_view name, std::string_view value) = 0;
};

class SettingsModel {
public:
    // The current values are taken as saved. Build it when the launcher opens,
    // before it sets anything, so the locks are the startup sources.
    SettingsModel(std::span<const Setting> table, Environment env, CvarStore& store);

    std::span<const Setting> Table() const { return table_; }
    const Setting* Find(std::string_view cvar) const;
    const CvarFacts* Facts(std::string_view cvar) const;
    const Environment& Env() const { return env_; }
    std::string_view Description(std::string_view cvar) const;

    // the cvar exists and the platform has it; an unavailable setting isn't
    // shown and its key is left alone
    bool Available(const Setting& setting) const;
    // Available, drawn (not kNone), on a Deck for the Deck banner, and its
    // shown_when holds
    bool Visible(const Setting& setting) const;

    std::string Value(std::string_view cvar) const;
    // sets the cvar now, so HotReload settings apply at once; false if it's
    // locked or the cvar refused the value. Changing steam_deck_defaults moves
    // every preset setting the player hasn't edited and that's at the old
    // default to the new one.
    bool Set(std::string_view cvar, std::string_view value);

    // what the setting is with no band3.toml key (a folder setting's default
    // folder for kPath)
    std::string EffectiveDefault(std::string_view cvar) const;
    // differs from the effective default, or its companion does: the row
    // shows a reset button, and Save writes it
    bool IsChanged(std::string_view cvar) const;
    // back to the effective default (empty for a folder), companion too, so
    // Save removes the key; false if locked
    bool Reset(std::string_view cvar);

    Lock LockOf(std::string_view cvar) const;
    bool IsLocked(std::string_view cvar) const { return LockOf(cvar) != Lock::kNone; }

    // the index of the choice matching the value by type, or -1 (a typed value)
    int ChoiceIndex(const Setting& setting) const;

    WindowMode GetWindowMode() const;
    bool SetWindowMode(WindowMode mode);

    // a warning for the row, if it needs one (render scale above 1 on a Deck)
    std::optional<std::string> Warning(std::string_view cvar) const;
    // what the row says when Set(cvar, value) was refused: "Not accepted:
    // "<value>"", and why when the limits tell (not a number, out of range,
    // not one of the allowed values)
    std::string Refusal(std::string_view cvar, std::string_view value) const;

    // something differs from what was last saved (or loaded)
    bool HasUnsavedChanges() const;

    // what Save does to band3.toml, in table order: a changed setting's typed
    // value, or removing an unchanged one's key. Locked and unavailable
    // settings, and keys outside the table, are left as they are.
    std::vector<ConfigEdit> Edits() const;
    // writes Edits() into the file (config_file.h); on success the values count as saved
    SaveResult Save(const std::filesystem::path& file);
    void MarkSaved();

private:
    bool DeckPresetsOn() const;
    std::string DefaultFor(std::string_view cvar, bool deck_on) const;
    bool Same(std::string_view cvar, std::string_view a, std::string_view b) const;
    std::optional<ConfigValue> TypedValue(const Setting& setting) const;

    std::span<const Setting> table_;
    Environment env_;
    CvarStore& store_;
    // set by the player this session (Reset clears it), for the Deck toggle
    std::set<std::string, std::less<>> edited_;
    // the values when last saved or loaded
    std::map<std::string, std::string, std::less<>> saved_;
};

}
