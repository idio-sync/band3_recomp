// Checks the Settings screen's rows (src/Launcher/ui/settings_rows.h): what
// each category lists, with the rows that act rather than set and the
// settings that open into several rows; the value each shows; the choices
// its list has; what left and right change it to; and search.

#include <doctest/doctest.h>
#include <map>
#include <string>
#include <vector>
#include "src/Launcher/ui/settings_rows.h"

using namespace band3::launcher;
using namespace band3::launcher::ui;

namespace {

class MapStore : public CvarStore {
public:
    std::map<std::string, std::string, std::less<>> values;
    std::string Get(std::string_view name) const override {
        const auto it = values.find(name);
        return it == values.end() ? std::string() : it->second;
    }
    bool Set(std::string_view name, std::string_view value) override {
        values[std::string(name)] = std::string(value);
        return true;
    }
};

CvarFacts Facts(ValueType type, std::string registry_default) {
    CvarFacts f;
    f.exists = true;
    f.type = type;
    f.registry_default = std::move(registry_default);
    return f;
}

// the real table, every cvar registered as text at "" unless set below
struct Screen {
    Environment env;
    MapStore store;
    std::optional<SettingsModel> model;
    DeviceLists lists;

    Screen() {
        env.windows = true;
        for (const Setting& s : SettingTable()) env.cvars[std::string(s.cvar)] = Facts(ValueType::kString, "");
        Type("renderer", ValueType::kString, "native");
        for (const char* b : {"autosave", "fast_start", "usb_mics", "midi_drums", "midi_keys",
                              "fullscreen", "fullscreen_exclusive", "native_present_pacing",
                              "debug_overlay", "http_enabled", "liveless", "stagekit_usb"}) {
            Type(b, ValueType::kBool, "false");
        }
        Type("autosave", ValueType::kBool, "true");
        Type("audio_maxqframes", ValueType::kInt, "8");
        Type("background_fps", ValueType::kInt, "0");
        Type("rnd_sync", ValueType::kInt, "1");
        Type("native_view_msaa", ValueType::kInt, "2");
        Type("monitor", ValueType::kInt, "0");
        Type("midi_keys_base_note", ValueType::kInt, "48");
        Type("song_speed", ValueType::kFloat, "1.000000");
        Type("left_stick_deadzone_percentage", ValueType::kFloat, "0.150000");
        Type("disable_hair_shader", ValueType::kBool, "false");
        Build();
    }

    void Type(const char* cvar, ValueType type, std::string value) {
        env.cvars[cvar] = Facts(type, value);
        store.values[cvar] = std::move(value);
    }
    void Build() { model.emplace(SettingTable(), env, store); }

    std::vector<Section> Rows(Category c, Where where = Where::kLauncher, bool sdk = false) {
        return CategoryRows(*model, c, RowOptions{.where = where, .sdk_settings = sdk});
    }
    Row Of(std::string_view cvar) {
        const Setting* s = FindSetting(SettingTable(), cvar);
        REQUIRE(s);
        return Row{.kind = RowKind::kSetting, .setting = s, .label = std::string(s->label)};
    }
    std::string Value(std::string_view cvar) { return ValueText(*model, Of(cvar), lists); }
};

std::vector<std::string> Labels(const Section& s) {
    std::vector<std::string> out;
    for (const Row& r : s.rows) out.push_back(r.label);
    return out;
}

const Section* Named(const std::vector<Section>& sections, std::string_view name) {
    for (const Section& s : sections) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

}

TEST_CASE("a category lists its sections' visible rows, under their labels") {
    Screen s;
    const auto game = s.Rows(Category::kGame);
    REQUIRE(game.size() == 3);
    CHECK(game[0].name == "Folders");
    CHECK(Labels(game[0]) == std::vector<std::string>{"Game data folder", "Song folders",
                                                      "User data folder", "Settings file"});
    CHECK(game[0].rows.back().kind == RowKind::kControl);
    CHECK(game[0].rows.back().control == Control::kSettingsFile);
    // MIDI's port rows show once the kit is on
    const auto midi = s.Rows(Category::kMidi);
    CHECK(Labels(midi[0]) == std::vector<std::string>{"MIDI drum kit"});
    s.store.values["midi_drums"] = "true";
    CHECK(Labels(s.Rows(Category::kMidi)[0]).size() == 6);
}

TEST_CASE("Lowest delay sits under the rows it sets") {
    Screen s;
    const auto display = s.Rows(Category::kDisplay);
    const Section* smooth = Named(display, "Smoothness");
    REQUIRE(smooth);
    CHECK(Labels(*smooth) == std::vector<std::string>{"Frame rate cap", "Smooth frame pacing",
                                                      "Lowest delay", "FPS counter"});
    // the calibration note goes with them
    CHECK(smooth->note);
}

TEST_CASE("the mics and the per-type delays open into a row each") {
    Screen s;
    s.store.values["usb_mics"] = "true";
    const auto audio = s.Rows(Category::kAudio);
    const Section* mics = Named(audio, "Microphones");
    REQUIRE(mics);
    CHECK(Labels(*mics) == std::vector<std::string>{"PC microphones", "Mic 1", "Mic 2", "Mic 3",
                                                    "Hear yourself sooner"});
    CHECK(mics->rows[1].kind == RowKind::kMicSlot);
    CHECK(mics->rows[3].index == 2);

    const auto controllers = s.Rows(Category::kControllers);
    const Section* delay = Named(controllers, "Delay");
    REQUIRE(delay);
    REQUIRE(delay->rows.size() == LagEditorTypes().size());
    CHECK(delay->rows[0].kind == RowKind::kLagType);
    CHECK(delay->rows[0].label == "Guitar");
}

TEST_CASE("rows that act show where they apply") {
    Screen s;
    // the tester reads the devices itself: the launcher's only
    auto has = [](const std::vector<Section>& sections, Control c) {
        for (const Section& section : sections) {
            for (const Row& r : section.rows) {
                if (r.kind == RowKind::kControl && r.control == c) return true;
            }
        }
        return false;
    };
    CHECK(has(s.Rows(Category::kControllers), Control::kTestControllers));
    CHECK_FALSE(has(s.Rows(Category::kControllers, Where::kInGame), Control::kTestControllers));
    CHECK(has(s.Rows(Category::kVideos), Control::kVideosList));
    const auto lights_rows = s.Rows(Category::kLights);
    const Section* lights = Named(lights_rows, "Try the lights");
    REQUIRE(lights);
    CHECK(lights->rows.size() >= 6);
    CHECK_FALSE(has(s.Rows(Category::kAdvanced), Control::kSdkSettings));
    CHECK(has(s.Rows(Category::kAdvanced, Where::kInGame, true), Control::kSdkSettings));
}

TEST_CASE("each kind of row shows its value as the screen words it") {
    Screen s;
    CHECK(s.Value("autosave") == "On");
    CHECK(s.Value("fast_start") == "Off");
    CHECK(s.Value("native_view_msaa") == "Game's (2x)");
    CHECK(s.Value("disable_hair_shader") == "On");
    CHECK(s.Value("audio_maxqframes") == "8 frames");
    CHECK(s.Value("background_fps") == "Venue's own");
    CHECK(s.Value("song_speed") == "1.00x");
    CHECK(s.Value("left_stick_deadzone_percentage") == "15%");
    CHECK(s.Value("midi_keys_base_note") == "48 (C3)");
    CHECK(s.Value("fullscreen") == "Windowed");
    s.store.values["fullscreen"] = "true";
    CHECK(s.Value("fullscreen") == "Borderless");
    // empty text, folders at their default, a password
    CHECK(s.Value("username") == "None");
    CHECK(s.Value("user_data_root") == "Default");
    s.store.values["ha_mqtt_password"] = "hunter2";
    CHECK(s.Value("ha_mqtt_password") == "Set");
    // a typed value the list doesn't have is shown as it is
    s.store.values["forced_venue"] = "arena_04";
    CHECK(s.Value("forced_venue") == "arena_04");
    s.store.values["content_folders"] = "songs|D:/more";
    CHECK(s.Value("content_folders") == "2 folders");
}

TEST_CASE("a mic slot and a controller type's delay show theirs") {
    Screen s;
    s.lists.mics = {"Rock Band USB Mic", "Headset"};
    const Setting* mics = FindSetting(SettingTable(), "usb_mic_devices");
    const Row slot0{.kind = RowKind::kMicSlot, .setting = mics, .index = 0, .label = "Mic 1"};
    const Row slot1{.kind = RowKind::kMicSlot, .setting = mics, .index = 1, .label = "Mic 2"};
    CHECK(ValueText(*s.model, slot0, s.lists) == "System default");
    CHECK(ValueText(*s.model, slot1, s.lists) == "None");
    CHECK(ApplyValue(*s.model, slot1, "Headset"));
    CHECK(s.store.values["usb_mic_devices"] == ",Headset");
    CHECK(ValueText(*s.model, slot0, s.lists) == "None");
    CHECK(ValueText(*s.model, slot1, s.lists) == "Headset");
    s.store.values["usb_mic_devices"] = "Gone";
    CHECK(ValueText(*s.model, slot0, s.lists) == "Gone (not connected)");

    const Setting* lag = FindSetting(SettingTable(), "joypad_lag");
    const Row guitar{.kind = RowKind::kLagType, .setting = lag, .index = 0, .label = "Guitar"};
    CHECK(ValueText(*s.model, guitar, s.lists) == "Game's own");
    CHECK_FALSE(RowChanged(*s.model, guitar));
    const auto first = Stepped(*s.model, guitar, 1, s.lists);
    REQUIRE(first);
    CHECK(ApplyValue(*s.model, guitar, *first));
    CHECK(ValueText(*s.model, guitar, s.lists) == "0 ms");
    CHECK(ApplyValue(*s.model, guitar, *Stepped(*s.model, guitar, 1, s.lists)));
    CHECK(ValueText(*s.model, guitar, s.lists) == "1 ms");
    CHECK(RowChanged(*s.model, guitar));
    CHECK(ResetRow(*s.model, guitar));
    CHECK(ValueText(*s.model, guitar, s.lists) == "Game's own");
}

TEST_CASE("left and right change a value, and stop at its ends") {
    Screen s;
    const Row msaa = s.Of("native_view_msaa");
    CHECK(Stepped(*s.model, msaa, 1, s.lists) == "4");
    CHECK(Stepped(*s.model, msaa, -1, s.lists) == "1");
    s.store.values["native_view_msaa"] = "4";
    CHECK_FALSE(Stepped(*s.model, msaa, 1, s.lists));
    CHECK(Stepped(*s.model, s.Of("fast_start"), 1, s.lists) == "true");
    CHECK_FALSE(Stepped(*s.model, s.Of("fast_start"), -1, s.lists));
    CHECK(Stepped(*s.model, s.Of("audio_maxqframes"), -1, s.lists) == "7");
    s.store.values["audio_maxqframes"] = "4";
    CHECK_FALSE(Stepped(*s.model, s.Of("audio_maxqframes"), -1, s.lists));
    CHECK(Stepped(*s.model, s.Of("song_speed"), 1, s.lists) == "1.05");
    CHECK(Stepped(*s.model, s.Of("left_stick_deadzone_percentage"), -1, s.lists) == "0.14");
    CHECK(Stepped(*s.model, s.Of("background_fps"), 1, s.lists) == "5");
    // text has no steps: A types it
    CHECK_FALSE(Stepped(*s.model, s.Of("username"), 1, s.lists));
    // the window's mode goes through both its settings
    const Row mode = s.Of("fullscreen");
    const auto next = Stepped(*s.model, mode, 1, s.lists);
    REQUIRE(next);
    CHECK(ApplyValue(*s.model, mode, *next));
    CHECK(s.store.values["fullscreen"] == "true");
    CHECK(s.store.values["fullscreen_exclusive"] == "false");
}

TEST_CASE("a list's choices mark the current one and the default") {
    Screen s;
    s.store.values["native_view_msaa"] = "4";
    const auto choices = Choices(*s.model, s.Of("native_view_msaa"), s.lists);
    REQUIRE(choices.size() == 3);
    CHECK(choices[2].current);
    CHECK(choices[1].is_default);
    CHECK_FALSE(choices[0].current);
    // a value typed in that the list doesn't have is a choice of its own, current
    s.store.values["forced_venue"] = "arena_04";
    const auto venues = Choices(*s.model, s.Of("forced_venue"), s.lists);
    REQUIRE_FALSE(venues.empty());
    CHECK(venues.back().current);
    CHECK(venues.back().label == "arena_04");
    // monitors by name once they're listed
    s.lists.monitors.push_back({.name = "DELL U2720Q", .primary = true});
    s.lists.monitors.push_back({.name = "LG OLED"});
    const auto monitors = Choices(*s.model, s.Of("monitor"), s.lists);
    REQUIRE(monitors.size() == 3);
    CHECK(monitors[0].label == "Default");
    CHECK(monitors[1].label == "DELL U2720Q (primary)");
    CHECK(monitors[2].value == "2");
}

TEST_CASE("search finds rows in every category by label and description") {
    Screen s;
    const auto found = SearchRows(*s.model, "anti-ALIAS", RowOptions{});
    REQUIRE(found.size() == 1);
    CHECK(found[0].name == "Graphics");
    CHECK(found[0].rows[0].label == "Anti-aliasing");
    // the description too
    const auto crackle = SearchRows(*s.model, "crackles", RowOptions{});
    REQUIRE(crackle.size() == 1);
    CHECK(crackle[0].rows[0].label == "Audio buffer");
    CHECK(SearchRows(*s.model, "", RowOptions{}).empty());
    CHECK(SearchRows(*s.model, "zzzz nothing", RowOptions{}).empty());
}

TEST_CASE("long lists open beside their row; short ones step") {
    Screen s;
    CHECK(OpensList(s.Of("monitor")));
    CHECK(OpensList(s.Of("resolution")));
    CHECK(OpensList(s.Of("midi_drums_device")));
    CHECK(OpensList(s.Of("frame_cap")));
    CHECK_FALSE(OpensList(s.Of("native_view_msaa")));
    CHECK_FALSE(OpensList(s.Of("autosave")));
}
