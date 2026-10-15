#include "generated_rows.h"
#include <algorithm>
#include <tuple>

namespace band3::launcher {

namespace {

constexpr std::string_view kBand3 = "Band3/";
constexpr std::string_view kAdvancedPrefix = "Advanced/";

// where a registry category's rows go: the first that starts it
struct RegistryPlace {
    std::string_view prefix;
    Category category;
};
constexpr RegistryPlace kRegistryPlaces[] = {
    {"Controllers/MIDI", Category::kMidi},
    {"Game", Category::kGame},
    {"Graphics", Category::kGraphics},
    {"Audio", Category::kAudio},
    {"Controllers", Category::kControllers},
    {"Online", Category::kOnline},
    {"Lights", Category::kLights},
    {"Videos", Category::kVideos},
};

}

std::optional<RowPlace> PlaceFor(std::string_view category) {
    if (!category.starts_with(kBand3)) return std::nullopt;
    const std::string_view rest = category.substr(kBand3.size());
    if (rest.empty() || rest == "Advanced/Retired") return std::nullopt;
    if (rest == "Advanced") return RowPlace{Category::kAdvanced, "Other"};
    if (rest.starts_with(kAdvancedPrefix)) {
        return RowPlace{Category::kAdvanced, std::string(rest.substr(kAdvancedPrefix.size()))};
    }
    // the native renderer's and the emulated GPU's, for the renderers the
    // table's rows of theirs are
    if (rest == "Graphics/Native") {
        return RowPlace{Category::kGraphics, std::string(kMoreSettings), kForNative | kForBoth};
    }
    if (rest == "Graphics/Emulated") {
        return RowPlace{Category::kGraphics, std::string(kMoreSettings), kForEmulated | kForBoth};
    }
    for (const RegistryPlace& place : kRegistryPlaces) {
        if (rest.starts_with(place.prefix)) {
            return RowPlace{place.category, std::string(kMoreSettings)};
        }
    }
    // a category of band3's none is for: Advanced, so nothing of band3's is
    // left to the SDK's settings alone
    return RowPlace{Category::kAdvanced, std::string(rest)};
}

std::string_view GeneratedRows::Keep(std::string text) {
    return strings_.emplace_back(std::move(text));
}

GeneratedRows::GeneratedRows(std::span<const RegistryCvar> registry,
                             std::span<const Setting> table) {
    struct Placed {
        const RegistryCvar* cvar;
        RowPlace place;
    };
    std::vector<Placed> placed;
    for (const RegistryCvar& cvar : registry) {
        if (FindSetting(table, cvar.name)) continue;
        if (auto place = PlaceFor(cvar.category)) placed.push_back({&cvar, std::move(*place)});
    }
    std::ranges::sort(placed, [](const Placed& a, const Placed& b) {
        return std::tie(a.place.category, a.place.section, a.cvar->name) <
               std::tie(b.place.category, b.place.section, b.cvar->name);
    });

    for (const Placed& p : placed) {
        const RegistryCvar& c = *p.cvar;
        Setting row{.cvar = Keep(c.name),
                    .category = p.place.category,
                    .section = Keep(p.place.section),
                    .label = {},
                    .widget = Widget::kText,
                    .renderers = p.place.renderers};
        row.label = row.cvar;
        row.command_line_only = c.lifecycle == Lifecycle::kInitOnly;
        const bool bounded = c.min && c.max && *c.min < *c.max;
        switch (c.type) {
        case ValueType::kBool: row.widget = Widget::kCheckbox; break;
        case ValueType::kInt:
            row.widget = Widget::kIntStepper;
            if (bounded) row.range = Range{*c.min, *c.max, 1};
            break;
        case ValueType::kFloat:
            row.widget = Widget::kFloatInput;
            if (bounded) row.range = Range{*c.min, *c.max, 0};
            break;
        case ValueType::kString:
            if (!c.allowed.empty()) {
                std::vector<Choice>& choices = choices_.emplace_back();
                for (const std::string& value : c.allowed) {
                    const std::string_view kept = Keep(value);
                    choices.push_back({kept, value.empty() ? Keep("(empty)") : kept});
                }
                row.widget = Widget::kCombo;
                row.choices = choices;
            }
            break;
        }
        rows_.push_back(row);
    }
}

std::vector<Setting> JoinTables(std::span<const Setting> table, std::span<const Setting> extra) {
    std::vector<Setting> out(table.begin(), table.end());
    out.insert(out.end(), extra.begin(), extra.end());
    return out;
}

}
