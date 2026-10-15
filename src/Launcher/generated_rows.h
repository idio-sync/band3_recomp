#pragma once
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "launcher_settings.h"

// Rows for the settings made from the cvar registry rather than the table,
// so a setting band3 adds shows up without anyone editing a table:
//
// - Band3/Advanced/<group>: the Advanced category, a section per group,
//   groups and rows by name. Band3/Advanced/Retired's show nowhere.
// - Band3/<category>[/<sub>] settings the table doesn't have: a "More
//   settings" section at the end of their category, by name.
//   Band3/Controllers/MIDI drums' and MIDI keyboard's go to MIDI;
//   Band3/Graphics/Native's and Band3/Graphics/Emulated's show for the same
//   renderers as the table's rows of theirs.
//
// Each row is labelled with the setting's name (the detail panel gives its
// description) and drawn with a widget for its type: on and off, a number, a
// list of its allowed values, or text. Pure: the game fills the registry in
// from rex::cvar (launcher_cvars.h), tests by hand.

namespace band3::launcher {

// one registered cvar, as the registry describes it
struct RegistryCvar {
    std::string name;
    std::string category;
    ValueType type = ValueType::kString;
    std::optional<double> min;
    std::optional<double> max;
    std::vector<std::string> allowed;
    // for the settings reference (settings_reference.h)
    std::string description;
    std::string default_value;
    Lifecycle lifecycle = Lifecycle::kHotReload;
};

// where a band3 registry category's generated rows go: their category and
// section, and the renderers they show for
struct RowPlace {
    Category category = Category::kGame;
    std::string section;
    uint8_t renderers = kForAnyRenderer;
};

// nullopt for a registry category band3's settings don't show (the SDK's,
// Keybinds..., band3's retired ones)
std::optional<RowPlace> PlaceFor(std::string_view category);

// the section the generated rows of a category other than Advanced go in
inline constexpr std::string_view kMoreSettings = "More settings";

class GeneratedRows {
public:
    // rows for every cvar in `registry` that PlaceFor places and `table`
    // doesn't already have
    GeneratedRows(std::span<const RegistryCvar> registry, std::span<const Setting> table);
    GeneratedRows(const GeneratedRows&) = delete;
    GeneratedRows& operator=(const GeneratedRows&) = delete;

    // the rows by category, section and name (Advanced's last).
    // Their string_views point into this object.
    std::span<const Setting> Rows() const { return rows_; }

private:
    // the rows' text, kept where it doesn't move
    std::string_view Keep(std::string text);

    std::deque<std::string> strings_;
    std::deque<std::vector<Choice>> choices_;
    std::vector<Setting> rows_;
};

// `table`'s rows, then `extra`'s: the Settings screen's whole table
std::vector<Setting> JoinTables(std::span<const Setting> table, std::span<const Setting> extra);

}
