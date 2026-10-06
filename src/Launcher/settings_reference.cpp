#include "settings_reference.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace band3::launcher {

namespace {

constexpr std::pair<Tab, std::string_view> kTabNames[] = {
    {Tab::kGame, "Game"},
    {Tab::kGraphics, "Graphics"},
    {Tab::kAudio, "Audio"},
    {Tab::kControllers, "Controllers"},
    {Tab::kOnline, "Online"},
    {Tab::kAdvanced, "Advanced"},
    {Tab::kSteamDeck, "Steam Deck"},
    {Tab::kFooter, "Launcher"},
};

constexpr std::string_view kTableHead =
    "| Setting | Default | Takes | What it does |\n|---|---|---|---|\n";

// text in a table cell: a | would end the cell, a <...> would read as HTML and
// a * as emphasis
std::string Cell(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '|': out += "\\|"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '*': out += "\\*"; break;
        case '\n': out += ' '; break;
        default: out += c;
        }
    }
    return out;
}

// a value, as typed: in a code span, where a | still ends the cell
std::string Code(std::string_view value) {
    if (value.empty()) return "*(empty)*";
    std::string out = "`";
    for (const char c : value) {
        if (c == '|') out += '\\';
        out += c;
    }
    return out + "`";
}

// whole numbers as integers (105000000, not 1.05e+08), others as short as they go
std::string Number(double v) {
    if (std::floor(v) == v && std::fabs(v) < 1e15) {
        return std::to_string(static_cast<int64_t>(v));
    }
    char buffer[32];
    const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), v);
    return ec == std::errc() ? std::string(buffer, end) : std::to_string(v);
}

// the default as the player would type it: 1, not 1.000000
std::string DefaultText(const RegistryCvar& c) {
    if (c.type == ValueType::kFloat) {
        if (const auto v = AsFloat(c.default_value)) return Code(Number(*v));
    }
    return Code(c.default_value);
}

std::string Join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (const std::string& part : parts) {
        if (!out.empty()) out += separator;
        out += part;
    }
    return out;
}

// the values a setting takes: its allowed values, or the dropdown's choices,
// or its range or type with the values the launcher names
std::string Takes(const Setting* row, const RegistryCvar& c) {
    if (c.type == ValueType::kBool) return "`true`, `false`";
    const std::span<const Choice> choices = row ? row->choices : std::span<const Choice>{};
    auto with_label = [&](std::string_view value) {
        std::string text = Code(value);
        const auto it = std::ranges::find(choices, value, &Choice::value);
        if (it != choices.end() && !it->label.empty() && it->label != value) {
            text += " " + Cell(it->label);
        }
        return text;
    };
    auto choice_list = [&] {
        std::vector<std::string> parts;
        for (const Choice& choice : choices) parts.push_back(with_label(choice.value));
        return Join(parts, ", ");
    };

    if (!c.allowed.empty()) {
        std::vector<std::string> parts;
        for (const std::string& value : c.allowed) parts.push_back(with_label(value));
        return Join(parts, ", ");
    }
    // a dropdown offers only its choices
    if (row && row->widget == Widget::kCombo && !choices.empty()) return choice_list();

    std::string text;
    if (c.min && c.max) {
        text = Code(Number(*c.min)) + " to " + Code(Number(*c.max));
    } else if (c.min) {
        text = Code(Number(*c.min)) + " or more";
    } else if (c.max) {
        text = "up to " + Code(Number(*c.max));
    } else {
        switch (c.type) {
        case ValueType::kInt: text = "a whole number"; break;
        case ValueType::kFloat: text = "a number"; break;
        default: text = "text"; break;
        }
    }
    if (row && !row->unit.empty() && c.type != ValueType::kString) {
        text += " (" + Cell(row->unit) + ")";
    }
    if (!choices.empty()) text += "; " + choice_list();
    return text;
}

std::string Description(const Setting* row, const RegistryCvar& c) {
    std::string text = Cell(c.description);
    if (c.lifecycle == Lifecycle::kRequiresRestart) text += " *Applies at the next start.*";
    if (c.lifecycle == Lifecycle::kInitOnly) text += " *Read as band3 starts.*";
    if (row && row->windows_only) text += " *Windows only.*";
    return text;
}

std::string Line(const Setting* row, const RegistryCvar& c) {
    std::string name = Code(c.name);
    if (row && !row->label.empty() && row->label != c.name) name += "<br>" + Cell(row->label);
    return "| " + name + " | " + DefaultText(c) + " | " + Takes(row, c) + " | " +
           Description(row, c) + " |\n";
}

const RegistryCvar* Find(std::span<const RegistryCvar> cvars, std::string_view name) {
    const auto it = std::ranges::find(cvars, name, &RegistryCvar::name);
    return it == cvars.end() ? nullptr : &*it;
}

}

std::string SettingsReference(const ReferenceInputs& in) {
    std::string out =
        "# Settings reference\n"
        "\n"
        "<!-- Generated by band3 --settings_reference (tools/settings_reference.py) from the\n"
        "     settings registry: don't edit it by hand. A setting's description is in\n"
        "     src/settings.cpp (or the SDK), its label and section in\n"
        "     src/Launcher/launcher_settings.cpp. -->\n"
        "\n"
        "Every setting the launcher and the in-game settings (F4) show, tab by tab, as this "
        "build of band3 registers them. Each can be saved in `band3.toml` as `name = value` "
        "or passed on the command line as `--name=value`; [Settings, folders and "
        "songs](settings.md#config-files) says which wins when several places set one. The "
        "defaults are the ";
    out += in.platform;
    out += " build's.\n";

    for (const auto& [tab, tab_name] : kTabNames) {
        const std::vector<std::string_view> sections = SectionsOf(in.page, tab);
        std::string tab_text;
        for (const std::string_view section : sections) {
            std::string rows;
            for (const Setting& row : in.page) {
                if (row.tab != tab || row.section != section) continue;
                if (const RegistryCvar* c = Find(in.cvars, row.cvar)) rows += Line(&row, *c);
            }
            if (rows.empty()) continue;
            // a tab with one section needs no heading for it
            if (sections.size() > 1) tab_text += "\n### " + Cell(section) + "\n";
            tab_text += "\n" + std::string(kTableHead) + rows;
        }
        if (!tab_text.empty()) out += "\n## " + std::string(tab_name) + "\n" + tab_text;
    }

    // the ini's settings F4 doesn't show, once each
    std::vector<std::string_view> seen;
    std::string sdk_rows;
    for (const LegacyIniKey& key : in.ini_keys) {
        const std::string_view name = key.cvar;
        if (FindSetting(in.page, name) || std::ranges::find(seen, name) != seen.end()) continue;
        seen.push_back(name);
        if (const RegistryCvar* c = Find(in.cvars, name)) sdk_rows += Line(nullptr, *c);
    }
    if (!sdk_rows.empty()) {
        out += "\n## More of the SDK's settings\n\n"
               "The SDK's settings band3_config.ini sets that the launcher and F4's tabs "
               "don't show. F4's **All settings...** lists these with the rest of the "
               "SDK's.\n\n";
        out += kTableHead;
        out += sdk_rows;
    }

    if (!in.ini_keys.empty()) {
        out += "\n## band3_config.ini\n\n"
               "band3 still reads a `band3_config.ini` from the folder it starts in, or the "
               "one beside it, as it did before `band3.toml`. Each key sets the setting "
               "beside it, but only where nothing else did, and an empty value leaves the "
               "setting alone.\n\n"
               "| Section | Key | Sets |\n|---|---|---|\n";
        for (const LegacyIniKey& key : in.ini_keys) {
            out += "| `[" + std::string(key.section) + "]` | `" + key.key + "` | `" + key.cvar +
                   "` |\n";
        }
    }
    return out;
}

}
