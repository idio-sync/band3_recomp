#pragma once
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "launcher_settings.h"

// Rows for the in-game settings (F4) made from the cvar registry rather than
// the launcher's table, so a setting band3 adds shows up without anyone
// editing a table:
//
// - Band3/Advanced/<group>: the Advanced tab, a section per group, groups and
//   rows by name.
// - Band3/<tab>[/<sub>] settings the table doesn't have: a "More settings"
//   section at the end of their tab, by name. Band3/Graphics/Native's and
//   Band3/Graphics/Emulated's join the tab's Native renderer and Emulated GPU
//   sections, shown for the same renderers as the table's rows there.
//
// Each row is labelled with the setting's name (the footer gives its
// description) and drawn with a widget for its type: a checkbox, a number, a
// dropdown of its allowed values, or a text field. Pure: the game fills the
// registry in from rex::cvar (ingame_settings_dialog.cpp), tests by hand.

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

// where a band3 category's generated rows go: their tab and section, and the
// renderers they show for
struct RowPlace {
    Tab tab = Tab::kGame;
    std::string section;
    uint8_t renderers = kForAnyRenderer;
};

// nullopt for a category band3's page doesn't show (the SDK's, Keybinds...)
std::optional<RowPlace> PlaceFor(std::string_view category);

// the section the generated rows of a tab other than Advanced go in
inline constexpr std::string_view kMoreSettings = "More settings";

class GeneratedRows {
public:
    // rows for every cvar in `registry` that PlaceFor places and `table`
    // doesn't already have
    GeneratedRows(std::span<const RegistryCvar> registry, std::span<const Setting> table);
    GeneratedRows(const GeneratedRows&) = delete;
    GeneratedRows& operator=(const GeneratedRows&) = delete;

    // Advanced's rows by group, then the other tabs' by tab, each by name.
    // Their string_views point into this object.
    std::span<const Setting> Rows() const { return rows_; }

private:
    // the rows' text, kept where it doesn't move
    std::string_view Keep(std::string text);

    std::deque<std::string> strings_;
    std::deque<std::vector<Choice>> choices_;
    std::vector<Setting> rows_;
};

// `table`'s rows, then `extra`'s: the in-game page's whole table
std::vector<Setting> JoinTables(std::span<const Setting> table, std::span<const Setting> extra);

}
