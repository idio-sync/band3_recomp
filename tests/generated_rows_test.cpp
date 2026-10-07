// Checks the in-game settings' rows made from the cvar registry
// (src/Launcher/generated_rows.h): where each goes, in what order, and with
// which widget.

#include <doctest/doctest.h>
#include <string>
#include <vector>
#include "src/Launcher/generated_rows.h"

using namespace band3::launcher;

namespace {

RegistryCvar Cvar(std::string name, std::string category, ValueType type = ValueType::kBool) {
    RegistryCvar c;
    c.name = std::move(name);
    c.category = std::move(category);
    c.type = type;
    return c;
}

constexpr Setting kTable[] = {
    {.cvar = "lang", .tab = Tab::kGame, .section = "Profile", .label = "Language",
     .widget = Widget::kCombo},
    {.cvar = "native_view_msaa", .tab = Tab::kGraphics, .section = "Native renderer",
     .label = "Anti-aliasing", .widget = Widget::kCombo},
};

std::vector<std::string> Names(std::span<const Setting> rows, Tab tab) {
    std::vector<std::string> names;
    for (const Setting& s : rows) {
        if (s.tab == tab) names.emplace_back(s.cvar);
    }
    return names;
}

const Setting* Row(std::span<const Setting> rows, std::string_view cvar) {
    return FindSetting(rows, cvar);
}

}

TEST_CASE("generated rows go where their category says") {
    CHECK(PlaceFor("Band3/Advanced/Native renderer")->tab == Tab::kAdvanced);
    CHECK(PlaceFor("Band3/Advanced/Native renderer")->section == "Native renderer");
    CHECK(PlaceFor("Band3/Advanced")->section == "Other");
    CHECK(PlaceFor("Band3/Game")->tab == Tab::kGame);
    CHECK(PlaceFor("Band3/Game")->section == kMoreSettings);
    CHECK(PlaceFor("Band3/Controllers/MIDI drums")->tab == Tab::kControllers);
    CHECK(PlaceFor("Band3/Controllers/MIDI drums")->section == kMoreSettings);
    CHECK(PlaceFor("Band3/Online")->tab == Tab::kOnline);
    CHECK(PlaceFor("Band3/Lights")->tab == Tab::kLights);
    CHECK(PlaceFor("Band3/Audio")->tab == Tab::kAudio);

    // the renderer groups join the table's sections, for the same renderers
    const auto native = PlaceFor("Band3/Graphics/Native");
    CHECK(native->tab == Tab::kGraphics);
    CHECK(native->section == "Native renderer");
    CHECK(native->renderers == (kForNative | kForBoth));
    const auto emulated = PlaceFor("Band3/Graphics/Emulated");
    CHECK(emulated->section == "Emulated GPU");
    CHECK(emulated->renderers == (kForEmulated | kForBoth));
    CHECK(PlaceFor("Band3/Graphics")->renderers == kForAnyRenderer);

    // band3's page doesn't show the SDK's settings
    CHECK_FALSE(PlaceFor("GPU"));
    CHECK_FALSE(PlaceFor("Keybinds"));
    CHECK_FALSE(PlaceFor("Band3"));
    CHECK_FALSE(PlaceFor("Band3x/Game"));
    // a band3 category no tab is for still shows, under Advanced
    CHECK(PlaceFor("Band3/Experiments")->tab == Tab::kAdvanced);
    CHECK(PlaceFor("Band3/Experiments")->section == "Experiments");
}

TEST_CASE("the Advanced tab lists Band3/Advanced's settings by group, then by name") {
    const std::vector<RegistryCvar> registry = {
        Cvar("test_port", "Band3/Advanced/Test harness", ValueType::kInt),
        Cvar("native_view_backend", "Band3/Advanced/Native renderer", ValueType::kString),
        Cvar("autoplay", "Band3/Advanced/Test harness"),
        Cvar("dred", "Band3/Advanced/Logging"),
        Cvar("native_math", "Band3/Advanced/Game code"),
        Cvar("main_heap_size", "Band3/Advanced/Memory", ValueType::kInt),
        Cvar("char_heap_size", "Band3/Advanced/Memory", ValueType::kInt),
        // not Advanced, or not band3's
        Cvar("fast_start", "Band3/Game"),
        Cvar("vsync", "GPU"),
    };
    const GeneratedRows generated(registry, kTable);
    const auto rows = generated.Rows();
    CHECK(Names(rows, Tab::kAdvanced) ==
          std::vector<std::string>{"native_math", "dred", "char_heap_size", "main_heap_size",
                                   "native_view_backend", "autoplay", "test_port"});
    CHECK(SectionsOf(rows, Tab::kAdvanced) ==
          std::vector<std::string_view>{"Game code", "Logging", "Memory", "Native renderer",
                                        "Test harness"});
    // labelled by name, the footer giving the description
    CHECK(Row(rows, "dred")->label == "dred");
    CHECK_FALSE(Row(rows, "vsync"));
}

TEST_CASE("a new band3 setting shows without a table entry") {
    std::vector<RegistryCvar> registry = {Cvar("dred", "Band3/Advanced/Logging")};
    {
        const GeneratedRows before(registry, kTable);
        CHECK_FALSE(Row(before.Rows(), "native_new_switch"));
    }
    registry.push_back(Cvar("native_new_switch", "Band3/Advanced/Native renderer"));
    const GeneratedRows after(registry, kTable);
    REQUIRE(Row(after.Rows(), "native_new_switch"));
    CHECK(Row(after.Rows(), "native_new_switch")->section == "Native renderer");
}

TEST_CASE("the table's own settings aren't generated again; the rest go to More settings") {
    const std::vector<RegistryCvar> registry = {
        Cvar("lang", "Band3/Game", ValueType::kString),
        Cvar("native_view_msaa", "Band3/Graphics/Native", ValueType::kInt),
        Cvar("native_new_limit", "Band3/Graphics/Native", ValueType::kInt),
        Cvar("menu_shortcut", "Band3/Controllers"),
        Cvar("midi_drums_notes", "Band3/Controllers/MIDI drums", ValueType::kString),
        Cvar("liveless_rooms", "Band3/Online"),
        Cvar("debug_overlay", "Band3/Graphics"),
    };
    const GeneratedRows generated(registry, kTable);
    const auto rows = generated.Rows();
    CHECK_FALSE(Row(rows, "lang"));
    CHECK_FALSE(Row(rows, "native_view_msaa"));
    CHECK(Names(rows, Tab::kControllers) ==
          std::vector<std::string>{"menu_shortcut", "midi_drums_notes"});
    CHECK(Row(rows, "menu_shortcut")->section == kMoreSettings);
    CHECK(Row(rows, "liveless_rooms")->tab == Tab::kOnline);
    CHECK(Row(rows, "debug_overlay")->section == kMoreSettings);
    CHECK(Row(rows, "native_new_limit")->section == "Native renderer");
    CHECK(Row(rows, "native_new_limit")->renderers == (kForNative | kForBoth));

    // joined after the table, they extend its sections and add their own at the end
    const auto joined = JoinTables(kTable, rows);
    CHECK(SectionsOf(joined, Tab::kGraphics) ==
          std::vector<std::string_view>{"Native renderer", kMoreSettings});
    CHECK(joined.front().cvar == "lang");
}

TEST_CASE("each type gets its widget") {
    RegistryCvar ranged = Cvar("native_view_shadow_scale", "Band3/Advanced/Native renderer",
                               ValueType::kInt);
    ranged.min = 1;
    ranged.max = 4;
    RegistryCvar open = Cvar("native_slow_frame_ms", "Band3/Advanced/Native renderer",
                             ValueType::kInt);
    open.min = 0;
    RegistryCvar choices = Cvar("emulated_gpu", "Band3/Advanced/Retired", ValueType::kString);
    choices.allowed = {"", "on", "off"};
    RegistryCvar speed = Cvar("speed", "Band3/Advanced/Game code", ValueType::kFloat);
    speed.min = 0.1;
    speed.max = 10;
    const std::vector<RegistryCvar> registry = {
        Cvar("dred", "Band3/Advanced/Logging"),
        ranged,
        open,
        choices,
        Cvar("liveless_gateway", "Band3/Advanced/Test harness", ValueType::kString),
        speed,
    };
    const GeneratedRows generated(registry, kTable);
    const auto rows = generated.Rows();
    CHECK(Row(rows, "dred")->widget == Widget::kCheckbox);

    const Setting* shadow = Row(rows, "native_view_shadow_scale");
    CHECK(shadow->widget == Widget::kIntStepper);
    REQUIRE(shadow->range);
    CHECK(shadow->range->min == 1);
    CHECK(shadow->range->max == 4);
    // a limit on one side only isn't a range to clamp to
    CHECK(Row(rows, "native_slow_frame_ms")->widget == Widget::kIntStepper);
    CHECK_FALSE(Row(rows, "native_slow_frame_ms")->range);

    const Setting* combo = Row(rows, "emulated_gpu");
    CHECK(combo->widget == Widget::kCombo);
    REQUIRE(combo->choices.size() == 3);
    CHECK(combo->choices[0].value == "");
    CHECK(combo->choices[0].label == "(empty)");
    CHECK(combo->choices[2].label == "off");

    CHECK(Row(rows, "liveless_gateway")->widget == Widget::kText);
    CHECK(Row(rows, "speed")->widget == Widget::kFloatInput);
    CHECK(Row(rows, "speed")->range->max == 10);
}
