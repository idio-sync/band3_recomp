#pragma once
#include <span>
#include <string>
#include <string_view>
#include "generated_rows.h"
#include "launcher_settings.h"
#include "src/config.h"

// The settings reference (docs/settings-reference.md), made from the registry
// so it can't fall behind it:
//
// - every setting the in-game settings (F4) show, tab by tab and section by
//   section in their order, with its name, default, the values it takes and
//   its description;
// - the SDK's settings band3_config.ini sets that F4 leaves to All settings...;
// - band3_config.ini's keys and the settings they set.
//
// Pure: band3 --settings_reference=<file> fills it in from rex::cvar
// (launcher_cvars.h's WriteSettingsReference), tests by hand.

namespace band3::launcher {

struct ReferenceInputs {
    // F4's whole table: SettingTable() joined with the generated rows
    std::span<const Setting> page;
    // every setting `page` and `ini_keys` name, as registered; a name missing
    // here is left out. default_value is the default the player gets (band3's
    // startup defaults over the registry's)
    std::span<const RegistryCvar> cvars;
    std::span<const LegacyIniKey> ini_keys;
    // the build the defaults are from: "Windows" or "Linux"
    std::string_view platform;
};

std::string SettingsReference(const ReferenceInputs& in);

// `text` as a table cell of the reference holds it: a | would end the cell, a
// <...> would read as HTML and a * as emphasis
std::string ReferenceText(std::string_view text);

}
