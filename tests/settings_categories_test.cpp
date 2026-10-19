// Checks the settings' twelve categories (src/Launcher/launcher_settings.h):
// every setting the launcher and the in-game settings show has its place, in
// the order and sections the redesign laid out, under a label no other row
// of its category shares; the rows generated from the registry land in them
// too; and what the Settings screen asks of the model per category.

#include <doctest/doctest.h>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "src/Launcher/generated_rows.h"
#include "src/Launcher/launcher_settings.h"

using namespace band3::launcher;

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

// every cvar of the real table registered, as a string at "" (renderer at
// `renderer`); the emulated GPU's own settings too, as in a run with it
struct RealTable {
    Environment env;
    MapStore store;
    std::optional<SettingsModel> model;

    explicit RealTable(const char* renderer = "native", bool in_game = false) {
        env.windows = true;
        env.in_game = in_game;
        for (const Setting& s : SettingTable()) {
            CvarFacts f;
            f.exists = true;
            env.cvars[std::string(s.cvar)] = f;
        }
        store.values["renderer"] = renderer;
        model.emplace(SettingTable(), env, store);
    }
};

std::vector<std::string_view> Sections(Category category) {
    return SectionsOf(SettingTable(), category);
}

using Names = std::vector<std::string_view>;

}

TEST_CASE("there are twelve categories, in the redesign's order") {
    const std::span<const Category> all = Categories();
    REQUIRE(all.size() == 12);
    std::vector<std::string_view> names;
    for (const Category c : all) names.push_back(CategoryName(c));
    CHECK(names == Names{"Game", "Gameplay", "Display", "Graphics", "Audio", "Controllers", "MIDI",
                         "Music Videos", "Lights", "Online", "Other Apps", "Advanced"});
}

TEST_CASE("each category says in a line what it's for") {
    for (const Category c : Categories()) {
        const std::string_view about = CategoryAbout(c);
        CHECK_MESSAGE(!about.empty(), CategoryName(c));
        // one line in the panel, roughly: a sentence, not a paragraph
        CHECK_MESSAGE(about.size() < 110, CategoryName(c));
        CHECK_MESSAGE(about.back() == '.', CategoryName(c));
    }
}

TEST_CASE("each category's sections are the redesign's, in order") {
    CHECK(Sections(Category::kGame) == Names{"Folders", "Profile", "Startup"});
    CHECK(Sections(Category::kGameplay) == Names{"Speed", "Unlocks", "Song list", "Venue"});
    CHECK(Sections(Category::kDisplay) == Names{"Screen", "Smoothness", "Menus"});
    CHECK(Sections(Category::kGraphics) ==
          Names{"Renderer", "Quality", "Effects", "Timing", "Performance"});
    CHECK(Sections(Category::kAudio) == Names{"Output", "Microphones"});
    CHECK(Sections(Category::kControllers) == Names{"Instruments", "Menus", "Sticks", "Delay"});
    CHECK(Sections(Category::kMidi) == Names{"Drums", "Keyboard"});
    CHECK(Sections(Category::kVideos) == Names{"Playback", "Finding videos"});
    CHECK(Sections(Category::kLights) == Names{"Stage Kits"});
    CHECK(Sections(Category::kOnline) == Names{"Online play", "GoCentral"});
    CHECK(Sections(Category::kOtherApps) ==
          Names{"Web song browser", "Scrobbling", "RB3Enhanced", "Home Assistant", "Discord"});
    // the table has none of Advanced's: they're generated from the registry
    CHECK(Sections(Category::kAdvanced).empty());
}

TEST_CASE("the settings once generated into More settings have places of their own") {
    const std::map<std::string_view, std::pair<Category, std::string_view>> places = {
        {"mouse_menus", {Category::kControllers, "Menus"}},
        {"keyboard_search", {Category::kControllers, "Menus"}},
        {"menu_shortcut", {Category::kControllers, "Menus"}},
        {"midi_drums_notes", {Category::kMidi, "Drums"}},
        {"midi_drums_pulse_ms", {Category::kMidi, "Drums"}},
        {"liveless_port_mapping", {Category::kOnline, "Online play"}},
        {"liveless_rooms", {Category::kOnline, "Online play"}},
        {"liveless_rooms_server", {Category::kOnline, "Online play"}},
        {"http_address", {Category::kOtherApps, "Web song browser"}},
        {"http_allow_cors", {Category::kOtherApps, "Web song browser"}},
        {"http_allow_scripts", {Category::kOtherApps, "Web song browser"}},
    };
    for (const auto& [cvar, place] : places) {
        INFO(cvar);
        const Setting* s = FindSetting(SettingTable(), cvar);
        REQUIRE(s);
        CHECK(s->category == place.first);
        CHECK(s->section == place.second);
    }
}

TEST_CASE("settings sit in the categories the redesign gave them") {
    const std::map<std::string_view, Category> places = {
        {"show_launcher", Category::kGame},
        {"launcher_style", Category::kDisplay},
        {"score_history", Category::kGame},
        {"song_speed", Category::kGameplay},
        {"forced_venue", Category::kGameplay},
        {"monitor", Category::kDisplay},
        {"frame_cap", Category::kDisplay},
        {"debug_overlay", Category::kDisplay},
        {"steam_deck_defaults", Category::kDisplay},
        {"renderer", Category::kGraphics},
        {"vsync", Category::kGraphics},
        {"midi_drums", Category::kMidi},
        {"midi_keys_base_note", Category::kMidi},
        {"music_video_fit", Category::kVideos},
        {"stagekit_usb", Category::kLights},
        {"gocentral", Category::kOnline},
        {"http_enabled", Category::kOtherApps},
        {"lastfm_enabled", Category::kOtherApps},
        {"rb3e_mode", Category::kOtherApps},
        {"ha_mqtt_host", Category::kOtherApps},
        {"discord_enabled", Category::kOtherApps},
    };
    for (const auto& [cvar, category] : places) {
        INFO(cvar);
        const Setting* s = FindSetting(SettingTable(), cvar);
        REQUIRE(s);
        CHECK(s->category == category);
    }
}

TEST_CASE("no two rows a category shows share a label, whichever renderer runs") {
    for (const char* renderer : {"native", "emulated", "both"}) {
        RealTable t(renderer);
        // what turns rows on, on
        for (const char* on : {"midi_drums", "midi_keys", "usb_mics", "http_enabled",
                               "events_enabled", "gocentral", "liveless", "liveless_rooms",
                               "lastfm_enabled", "listenbrainz_enabled"}) {
            t.store.values[on] = "true";
        }
        t.store.values["ha_mqtt_host"] = "broker";
        t.store.values["frame_cap"] = "off";
        for (const Category c : Categories()) {
            std::set<std::string> labels;
            for (const Setting& s : SettingTable()) {
                if (s.category != c || !t.model->Visible(s)) continue;
                INFO(renderer, " ", s.cvar);
                CHECK(labels.insert(t.model->Label(s)).second);
            }
        }
    }
}

TEST_CASE("with both renderers, a setting each has twice says which renderer's it is") {
    RealTable both("both");
    const Setting* native = FindSetting(SettingTable(), "native_view_msaa");
    const Setting* emulated = FindSetting(SettingTable(), "swap_post_effect");
    REQUIRE(native);
    REQUIRE(emulated);
    CHECK(native->label == emulated->label);
    CHECK(both.model->Label(*native) == std::string(native->label) + " (native)");
    CHECK(both.model->Label(*emulated) == std::string(emulated->label) + " (emulated)");
    // with one renderer, its own label alone
    RealTable alone("native");
    CHECK(alone.model->Label(*native) == native->label);
    // a row the other renderer has no twin of keeps its label under both
    const Setting* scale = FindSetting(SettingTable(), "resolution_scale");
    REQUIRE(scale);
    CHECK(both.model->Label(*scale) == scale->label);
}

TEST_CASE("the Steam Deck's row shows only on a Deck") {
    const Setting* deck = FindSetting(SettingTable(), "steam_deck_defaults");
    REQUIRE(deck);
    CHECK(deck->deck_only);
    RealTable desktop;
    CHECK_FALSE(desktop.model->Visible(*deck));
    RealTable on_deck;
    on_deck.env.steam_deck = true;
    on_deck.model.emplace(SettingTable(), on_deck.env, on_deck.store);
    CHECK(on_deck.model->Visible(*deck));
}

TEST_CASE("a row whose controlling row is hidden is hidden too") {
    // the Rooms server shows with Rooms on, and Rooms only with online play on
    RealTable t;
    t.store.values["liveless"] = "false";
    t.store.values["liveless_rooms"] = "true";
    const Setting* server = FindSetting(SettingTable(), "liveless_rooms_server");
    REQUIRE(server);
    CHECK(server->shown_when.cvar == "liveless_rooms");
    CHECK_FALSE(t.model->Visible(*server));
    t.store.values["liveless"] = "true";
    CHECK(t.model->Visible(*server));
}

TEST_CASE("in game, the folders are changed from the launcher only") {
    for (const char* cvar : {"game_data_root", "content_folders", "user_data_root"}) {
        INFO(cvar);
        const Setting* s = FindSetting(SettingTable(), cvar);
        REQUIRE(s);
        CHECK(s->launcher_only);
        RealTable launcher;
        CHECK_FALSE(launcher.model->ReadOnly(cvar));
        RealTable in_game("native", true);
        CHECK(in_game.model->ReadOnly(cvar));
        CHECK_FALSE(in_game.model->Set(cvar, "elsewhere"));
    }
    // the videos folder is read as each song plays: it can change in game
    RealTable in_game("native", true);
    CHECK_FALSE(in_game.model->ReadOnly("music_videos_folder"));
}

TEST_CASE("a category counts its shown settings that differ from their defaults") {
    RealTable t;
    t.env.cvars["autosave"].type = ValueType::kBool;
    t.env.cvars["autosave"].registry_default = "true";
    t.env.cvars["fast_start"].type = ValueType::kBool;
    t.env.cvars["fast_start"].registry_default = "false";
    t.store.values["autosave"] = "true";
    t.store.values["fast_start"] = "false";
    t.model.emplace(SettingTable(), t.env, t.store);
    const int before = t.model->ChangedIn(Category::kGame);
    CHECK(t.model->Set("fast_start", "true"));
    CHECK(t.model->ChangedIn(Category::kGame) == before + 1);
    CHECK(t.model->Set("autosave", "false"));
    CHECK(t.model->ChangedIn(Category::kGame) == before + 2);
    CHECK(t.model->ChangedIn(Category::kGameplay) == 0);
    CHECK(t.model->Reset("fast_start"));
    CHECK(t.model->ChangedIn(Category::kGame) == before + 1);
}

TEST_CASE("the generated rows go into the twelve categories") {
    CHECK(PlaceFor("Band3/Advanced/Native renderer")->category == Category::kAdvanced);
    CHECK(PlaceFor("Band3/Advanced/Native renderer")->section == "Native renderer");
    CHECK(PlaceFor("Band3/Controllers/MIDI drums")->category == Category::kMidi);
    CHECK(PlaceFor("Band3/Controllers/MIDI keyboard")->category == Category::kMidi);
    CHECK(PlaceFor("Band3/Controllers")->category == Category::kControllers);
    CHECK(PlaceFor("Band3/Videos")->category == Category::kVideos);
    CHECK(PlaceFor("Band3/Graphics")->category == Category::kGraphics);
    // the retired settings show nowhere
    CHECK_FALSE(PlaceFor("Band3/Advanced/Retired"));
}

TEST_CASE("the launcher shows the generated rows too") {
    CHECK(FeaturesFor(Where::kLauncher).generated_rows);
    CHECK(FeaturesFor(Where::kInGame).generated_rows);
}

TEST_CASE("every row the table draws says what it does in plain words") {
    for (const Setting& s : SettingTable()) {
        if (s.widget == Widget::kNone) continue;
        INFO(s.cvar);
        CHECK_FALSE(s.about.empty());
    }
}

TEST_CASE("the startup box is never a change to reset or one waiting for a restart") {
    // after a save that unticked it, show_launcher's cvar is false and its default true
    const Setting* show = FindSetting(SettingTable(), "show_launcher");
    REQUIRE(show);
    RealTable t("native", true);
    t.env.cvars["show_launcher"].type = ValueType::kBool;
    t.env.cvars["show_launcher"].registry_default = "true";
    t.env.cvars["show_launcher"].lifecycle = Lifecycle::kRequiresRestart;
    t.env.cvars["show_launcher"].started_with = "true";
    t.store.values["show_launcher"] = "false";
    t.model.emplace(SettingTable(), t.env, t.store);
    CHECK_FALSE(t.model->IsChanged("show_launcher"));
    CHECK_FALSE(t.model->WaitsForNextStart("show_launcher"));
    CHECK(t.model->ChangedIn(Category::kGame) == 0);
}