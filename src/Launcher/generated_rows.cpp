#include "generated_rows.h"
#include <algorithm>
#include <tuple>

namespace band3::launcher {

namespace {

constexpr std::string_view kBand3 = "Band3/";
constexpr std::string_view kAdvancedPrefix = "Advanced/";

// the section a top-level band3 category's rows go in, by tab
struct TabCategory {
    std::string_view category;
    Tab tab;
};
constexpr TabCategory kTabCategories[] = {
    {"Game", Tab::kGame},
    {"Graphics", Tab::kGraphics},
    {"Audio", Tab::kAudio},
    {"Controllers", Tab::kControllers},
    {"Online", Tab::kOnline},
    {"Lights", Tab::kLights},
};

}

std::optional<RowPlace> PlaceFor(std::string_view category) {
    if (!category.starts_with(kBand3)) return std::nullopt;
    const std::string_view rest = category.substr(kBand3.size());
    if (rest.empty()) return std::nullopt;
    if (rest == "Advanced") return RowPlace{Tab::kAdvanced, "Other"};
    if (rest.starts_with(kAdvancedPrefix)) {
        return RowPlace{Tab::kAdvanced, std::string(rest.substr(kAdvancedPrefix.size()))};
    }
    // the native renderer's and the emulated GPU's, as the table's rows there
    if (rest == "Graphics/Native") {
        return RowPlace{Tab::kGraphics, "Native renderer", kForNative | kForBoth};
    }
    if (rest == "Graphics/Emulated") {
        return RowPlace{Tab::kGraphics, "Emulated GPU", kForEmulated | kForBoth};
    }
    const std::string_view top = rest.substr(0, rest.find('/'));
    for (const TabCategory& tc : kTabCategories) {
        if (top == tc.category) return RowPlace{tc.tab, std::string(kMoreSettings)};
    }
    // a category of band3's no tab is for: Advanced, so nothing of band3's is
    // left to the SDK's settings alone
    return RowPlace{Tab::kAdvanced, std::string(rest)};
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
        return std::tie(a.place.tab, a.place.section, a.cvar->name) <
               std::tie(b.place.tab, b.place.section, b.cvar->name);
    });

    for (const Placed& p : placed) {
        const RegistryCvar& c = *p.cvar;
        Setting row{.cvar = Keep(c.name),
                    .tab = p.place.tab,
                    .section = Keep(p.place.section),
                    .label = {},
                    .widget = Widget::kText,
                    .renderers = p.place.renderers};
        row.label = row.cvar;
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
