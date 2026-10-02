// Checks the launcher's settings model (src/Launcher/launcher_settings.h):
// effective defaults, typed comparison, reset, locks, the Steam Deck toggle,
// what Save writes, and the joypad_lag and folder helpers.

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <toml++/toml.hpp>
#include "src/Input/joypad_lag_status.h"
#include "src/Launcher/launcher_settings.h"

namespace fs = std::filesystem;
using namespace band3::launcher;

namespace {

class FakeStore : public CvarStore {
public:
    std::map<std::string, std::string, std::less<>> values;
    // names that refuse every value
    std::set<std::string, std::less<>> refuse;
    int sets = 0;

    std::string Get(std::string_view name) const override {
        const auto it = values.find(name);
        return it == values.end() ? std::string() : it->second;
    }
    bool Set(std::string_view name, std::string_view value) override {
        if (refuse.contains(name)) return false;
        values[std::string(name)] = std::string(value);
        sets++;
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

fs::path Anchor() { return fs::temp_directory_path() / "band3_launcher_anchor"; }

std::string Generic(const fs::path& p) {
    const auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

constexpr Choice kOnOff[] = {{"false", "Off"}, {"true", "On"}};

constexpr Setting kTable[] = {
    {.cvar = "fullscreen", .tab = Tab::kGraphics, .section = "Display", .label = "Window mode",
     .widget = Widget::kWindowMode, .companion = "fullscreen_exclusive"},
    {.cvar = "fullscreen_exclusive", .tab = Tab::kGraphics, .section = "Display",
     .label = "Exclusive", .widget = Widget::kNone},
    {.cvar = "present_letterbox", .tab = Tab::kGraphics, .section = "Display", .label = "Aspect",
     .widget = Widget::kCombo, .choices = kOnOff},
    {.cvar = "rnd_sync", .tab = Tab::kGraphics, .section = "Rendering", .label = "Sync",
     .widget = Widget::kIntStepper},
    {.cvar = "song_speed", .tab = Tab::kGame, .section = "Game", .label = "Song speed",
     .widget = Widget::kFloatSlider},
    {.cvar = "audio_maxqframes", .tab = Tab::kAudio, .section = "Output", .label = "Buffer",
     .widget = Widget::kIntStepper},
    {.cvar = "input_backend", .tab = Tab::kControllers, .section = "Input", .label = "Backend",
     .widget = Widget::kCombo, .windows_only = true},
    {.cvar = "midi_drums", .tab = Tab::kControllers, .section = "MIDI", .label = "MIDI drums",
     .widget = Widget::kCheckbox},
    {.cvar = "midi_drums_device", .tab = Tab::kControllers, .section = "MIDI", .label = "Port",
     .widget = Widget::kText, .shown_when = {"midi_drums", "true"}},
    {.cvar = "lang", .tab = Tab::kGame, .section = "Profile", .label = "Language",
     .widget = Widget::kCombo},
    {.cvar = "game_data_root", .tab = Tab::kGame, .section = "Folders", .label = "Game data",
     .widget = Widget::kPath},
    {.cvar = "content_folders", .tab = Tab::kGame, .section = "Folders", .label = "Songs",
     .widget = Widget::kFolderList},
    {.cvar = "missing_cvar", .tab = Tab::kGame, .section = "Game", .label = "Not in this build",
     .widget = Widget::kCheckbox},
    {.cvar = "steam_deck_defaults", .tab = Tab::kSteamDeck, .section = "Steam Deck",
     .label = "Deck", .widget = Widget::kCheckbox},
    {.cvar = "show_launcher", .tab = Tab::kFooter, .section = "Footer",
     .label = "Show this screen at startup", .widget = Widget::kCheckbox},
};

// cvars as a fresh start would leave them, nothing in band3.toml
struct Fixture {
    Environment env;
    FakeStore store;

    Fixture() {
        env.anchor = Anchor();
        env.windows = true;
        auto& c = env.cvars;
        c["fullscreen"] = Facts(ValueType::kBool, "true");
        c["fullscreen"].deck_preset = "true";
        c["fullscreen_exclusive"] = Facts(ValueType::kBool, "false");
        c["present_letterbox"] = Facts(ValueType::kBool, "false");
        c["present_letterbox"].deck_preset = "true";
        c["rnd_sync"] = Facts(ValueType::kInt, "-1");
        c["rnd_sync"].deck_preset = "1";
        c["rnd_sync"].min = -1;
        c["rnd_sync"].max = 1;
        c["song_speed"] = Facts(ValueType::kFloat, "1.000000");
        c["audio_maxqframes"] = Facts(ValueType::kInt, "8");
        c["audio_maxqframes"].startup_default = "3";
        c["input_backend"] = Facts(ValueType::kString, "sdl");
        c["input_backend"].allowed = {"sdl", "xinput"};
        c["midi_drums"] = Facts(ValueType::kBool, "false");
        c["midi_drums_device"] = Facts(ValueType::kString, "");
        c["lang"] = Facts(ValueType::kString, "");
        c["lang"].allowed = {"", "eng", "esl", "fre", "ita", "deu"};
        c["game_data_root"] = Facts(ValueType::kString, "");
        c["game_data_root"].path_default = Anchor() / "assets";
        c["content_folders"] = Facts(ValueType::kString, "songs");
        c["steam_deck_defaults"] = Facts(ValueType::kBool, "true");
        c["show_launcher"] = Facts(ValueType::kBool, "true");
        c["missing_cvar"] = CvarFacts{};

        store.values = {
            {"fullscreen", "true"},
            {"fullscreen_exclusive", "false"},
            {"present_letterbox", "false"},
            {"rnd_sync", "-1"},
            {"song_speed", "1.000000"},
            {"audio_maxqframes", "3"},
            {"input_backend", "sdl"},
            {"midi_drums", "false"},
            {"midi_drums_device", ""},
            {"lang", ""},
            {"game_data_root", ""},
            {"content_folders", "songs"},
            {"steam_deck_defaults", "true"},
            {"show_launcher", "true"},
        };
    }

    // a Deck with the presets applied, as ApplyDefaults leaves it
    void OnDeck() {
        env.steam_deck = true;
        store.values["fullscreen"] = "true";
        store.values["present_letterbox"] = "true";
        store.values["rnd_sync"] = "1";
    }
};

const ConfigEdit* EditFor(const std::vector<ConfigEdit>& edits, std::string_view key) {
    for (const auto& e : edits) {
        if (e.key == key) return &e;
    }
    return nullptr;
}

}

TEST_CASE("the effective default is the first layer that applies") {
    Fixture f;
    auto& rnd = f.env.cvars["rnd_sync"];

    SUBCASE("the registry's") {
        SettingsModel m(kTable, f.env, f.store);
        CHECK(m.EffectiveDefault("rnd_sync") == "-1");
        CHECK(m.EffectiveDefault("lang") == "");
    }
    SUBCASE("band3's own default over the registry's") {
        SettingsModel m(kTable, f.env, f.store);
        CHECK(m.EffectiveDefault("audio_maxqframes") == "3");
        CHECK_FALSE(m.IsChanged("audio_maxqframes"));
    }
    SUBCASE("the ini over band3's own default, as at startup") {
        f.env.cvars["audio_maxqframes"].ini = "16";
        SettingsModel m(kTable, f.env, f.store);
        CHECK(m.EffectiveDefault("audio_maxqframes") == "16");
    }
    SUBCASE("the ini over the registry") {
        rnd.ini = "0";
        SettingsModel m(kTable, f.env, f.store);
        CHECK(m.EffectiveDefault("rnd_sync") == "0");
    }
    SUBCASE("an ini value the cvar refuses leaves the default, as ApplyLegacyIni does") {
        rnd.ini = "5";  // outside -1..1
        f.env.cvars["lang"].ini = "klingon";
        f.env.cvars["song_speed"].ini = "fast";
        SettingsModel m(kTable, f.env, f.store);
        CHECK(m.EffectiveDefault("rnd_sync") == "-1");
        CHECK(m.EffectiveDefault("lang") == "");
        CHECK(m.EffectiveDefault("song_speed") == "1.000000");
    }
    SUBCASE("the Steam Deck preset over the ini, on a Deck with the presets on") {
        rnd.ini = "0";
        f.OnDeck();
        SettingsModel m(kTable, f.env, f.store);
        CHECK(m.EffectiveDefault("rnd_sync") == "1");
        CHECK_FALSE(m.IsChanged("rnd_sync"));
    }
    SUBCASE("no preset off a Deck, or with steam_deck_defaults off") {
        rnd.ini = "0";
        SettingsModel off_deck(kTable, f.env, f.store);
        CHECK(off_deck.EffectiveDefault("rnd_sync") == "0");

        f.env.steam_deck = true;
        f.store.values["steam_deck_defaults"] = "false";
        SettingsModel presets_off(kTable, f.env, f.store);
        CHECK(presets_off.EffectiveDefault("rnd_sync") == "0");
    }
    SUBCASE("band3's startup value over everything") {
        auto& backend = f.env.cvars["input_backend"];
        backend.ini = "xinput";
        backend.startup_forced = "sdl";
        SettingsModel m(kTable, f.env, f.store);
        CHECK(m.EffectiveDefault("input_backend") == "sdl");
    }
    SUBCASE("a folder setting's default is the folder the game starts with") {
        SettingsModel m(kTable, f.env, f.store);
        CHECK(m.EffectiveDefault("game_data_root") == Generic(Anchor() / "assets"));
        // empty is that folder, so it isn't a change
        CHECK_FALSE(m.IsChanged("game_data_root"));
    }
}

TEST_CASE("values compare by type") {
    CHECK(SameValue(ValueType::kFloat, "1.000000", "1"));
    CHECK(SameValue(ValueType::kFloat, "0.150000", "0.15"));
    CHECK_FALSE(SameValue(ValueType::kFloat, "1.05", "1"));
    CHECK(SameValue(ValueType::kBool, "yes", "true"));
    CHECK(SameValue(ValueType::kBool, "1", "true"));
    CHECK(SameValue(ValueType::kBool, "false", "no"));
    CHECK_FALSE(SameValue(ValueType::kBool, "true", "false"));
    CHECK(SameValue(ValueType::kInt, "3", "3"));
    CHECK(SameValue(ValueType::kInt, "-1", "-1"));
    CHECK_FALSE(SameValue(ValueType::kInt, "3", "4"));
    CHECK(SameValue(ValueType::kString, "fxaa", "fxaa"));
    CHECK_FALSE(SameValue(ValueType::kString, "1", "1.0"));

    Fixture f;
    f.store.values["song_speed"] = "1";
    f.env.cvars["midi_drums"].ini = "true";
    f.store.values["midi_drums"] = "yes";
    SettingsModel m(kTable, f.env, f.store);
    CHECK_FALSE(m.IsChanged("song_speed"));
    CHECK_FALSE(m.IsChanged("midi_drums"));
    m.Set("song_speed", "1.25");
    CHECK(m.IsChanged("song_speed"));
}

TEST_CASE("Save writes changed settings typed and removes the rest") {
    Fixture f;
    SettingsModel m(kTable, f.env, f.store);
    m.Set("song_speed", "1.250000");
    m.Set("rnd_sync", "0");
    m.Set("midi_drums", "true");
    m.Set("lang", "fre");
    const auto edits = m.Edits();

    REQUIRE(EditFor(edits, "song_speed"));
    CHECK(*EditFor(edits, "song_speed")->value == ConfigValue(1.25));
    CHECK(*EditFor(edits, "rnd_sync")->value == ConfigValue(int64_t{0}));
    CHECK(*EditFor(edits, "midi_drums")->value == ConfigValue(true));
    CHECK(*EditFor(edits, "lang")->value == ConfigValue(std::string("fre")));
    // unchanged: the key goes
    REQUIRE(EditFor(edits, "fullscreen"));
    CHECK_FALSE(EditFor(edits, "fullscreen")->value);
    CHECK_FALSE(EditFor(edits, "audio_maxqframes")->value);
    // not in this build: left alone
    CHECK_FALSE(EditFor(edits, "missing_cvar"));
}

TEST_CASE("reset goes back to the effective default, so the key is removed") {
    Fixture f;
    f.env.cvars["rnd_sync"].ini = "0";
    f.store.values["rnd_sync"] = "1";
    SettingsModel m(kTable, f.env, f.store);
    CHECK(m.IsChanged("rnd_sync"));
    CHECK(m.Reset("rnd_sync"));
    CHECK(f.store.values["rnd_sync"] == "0");
    CHECK_FALSE(m.IsChanged("rnd_sync"));
    CHECK_FALSE(EditFor(m.Edits(), "rnd_sync")->value);

    SUBCASE("the window mode resets both its settings") {
        m.SetWindowMode(WindowMode::kExclusive);
        CHECK(m.GetWindowMode() == WindowMode::kExclusive);
        // only the companion differs, and the row still shows it
        CHECK(m.IsChanged("fullscreen"));
        m.Reset("fullscreen");
        CHECK(m.GetWindowMode() == WindowMode::kBorderless);
        CHECK_FALSE(m.IsChanged("fullscreen"));
        m.SetWindowMode(WindowMode::kWindowed);
        CHECK(f.store.values["fullscreen"] == "false");
        CHECK(f.store.values["fullscreen_exclusive"] == "false");
    }
    SUBCASE("a folder resets to empty, the default folder") {
        m.Set("game_data_root", "D:/RB3");
        CHECK(m.IsChanged("game_data_root"));
        m.Reset("game_data_root");
        CHECK(f.store.values["game_data_root"] == "");
        CHECK_FALSE(m.IsChanged("game_data_root"));
    }
    SUBCASE("a value already at the default isn't set again") {
        const int sets = f.store.sets;
        m.Reset("song_speed");
        CHECK(f.store.sets == sets);
    }
}

TEST_CASE("settings from the command line or the environment are locked") {
    Fixture f;
    f.env.cvars["rnd_sync"].lock = Lock::kCommandLine;
    f.store.values["rnd_sync"] = "1";
    f.env.cvars["lang"].lock = Lock::kEnvironment;
    f.store.values["lang"] = "deu";
    SettingsModel m(kTable, f.env, f.store);

    CHECK(m.IsLocked("rnd_sync"));
    CHECK(std::string(LockReason(m.LockOf("rnd_sync"))) == "Set on the command line");
    CHECK(std::string(LockReason(m.LockOf("lang"))) == "Set by an environment variable");
    CHECK_FALSE(m.IsLocked("song_speed"));
    CHECK(std::string(LockReason(Lock::kNone)).empty());

    CHECK_FALSE(m.Set("rnd_sync", "0"));
    CHECK_FALSE(m.Reset("rnd_sync"));
    CHECK(f.store.values["rnd_sync"] == "1");
    // the file keeps whatever it has for them
    const auto edits = m.Edits();
    CHECK_FALSE(EditFor(edits, "rnd_sync"));
    CHECK_FALSE(EditFor(edits, "lang"));
    CHECK_FALSE(m.HasUnsavedChanges());
}

TEST_CASE("a refused value says why when the limits tell") {
    Fixture f;
    SettingsModel m(kTable, f.env, f.store);
    CHECK(m.Refusal("rnd_sync", "fast") == "Not accepted: \"fast\" isn't a number");
    CHECK(m.Refusal("rnd_sync", "5") == "Not accepted: \"5\" isn't from -1 to 1");
    CHECK(m.Refusal("song_speed", "x") == "Not accepted: \"x\" isn't a number");
    CHECK(m.Refusal("lang", "klingon") ==
          "Not accepted: \"klingon\" isn't one of (empty), eng, esl, fre, ita, deu");
    // a refusal the facts don't explain
    CHECK(m.Refusal("midi_drums_device", "loopMIDI") == "Not accepted: \"loopMIDI\"");
    CHECK(m.Refusal("no_such_cvar", "1") == "Not accepted: \"1\"");

    f.store.refuse.insert("midi_drums_device");
    CHECK_FALSE(m.Set("midi_drums_device", "loopMIDI"));
}

TEST_CASE("turning the Steam Deck presets off doesn't save them as overrides") {
    Fixture f;
    f.OnDeck();
    f.env.cvars["rnd_sync"].ini = "0";
    // band3.toml set windowed over the preset
    f.store.values["fullscreen"] = "false";
    SettingsModel m(kTable, f.env, f.store);
    CHECK_FALSE(m.IsChanged("rnd_sync"));
    CHECK_FALSE(m.IsChanged("present_letterbox"));
    CHECK(m.IsChanged("fullscreen"));

    // the player picks the preset's frame sync themselves
    m.Set("rnd_sync", "1");
    CHECK(m.Set("steam_deck_defaults", "false"));

    // at the old default and not edited: follows to the new one
    CHECK(f.store.values["present_letterbox"] == "false");
    CHECK_FALSE(m.IsChanged("present_letterbox"));
    // edited by the player: kept, and now an override of the ini's 0
    CHECK(f.store.values["rnd_sync"] == "1");
    CHECK(m.IsChanged("rnd_sync"));
    // not at the old default: kept
    CHECK(f.store.values["fullscreen"] == "false");

    const auto edits = m.Edits();
    CHECK_FALSE(EditFor(edits, "present_letterbox")->value);
    CHECK(*EditFor(edits, "rnd_sync")->value == ConfigValue(int64_t{1}));
    CHECK(*EditFor(edits, "fullscreen")->value == ConfigValue(false));
    CHECK(*EditFor(edits, "steam_deck_defaults")->value == ConfigValue(false));

    // and back on: the preset returns where the player didn't choose
    SUBCASE("ticked again") { CHECK(m.Set("steam_deck_defaults", "true")); }
    SUBCASE("reset") {
        CHECK(m.IsChanged("steam_deck_defaults"));
        CHECK(m.Reset("steam_deck_defaults"));
        CHECK(f.store.values["steam_deck_defaults"] == "true");
        CHECK_FALSE(m.IsChanged("steam_deck_defaults"));
    }
    CHECK(f.store.values["present_letterbox"] == "true");
    CHECK_FALSE(m.IsChanged("present_letterbox"));
    CHECK(f.store.values["rnd_sync"] == "1");
    CHECK(f.store.values["fullscreen"] == "false");
}

TEST_CASE("the Deck toggle does nothing off a Deck") {
    Fixture f;
    f.env.cvars["rnd_sync"].ini = "0";
    f.store.values["rnd_sync"] = "0";
    SettingsModel m(kTable, f.env, f.store);
    m.Set("steam_deck_defaults", "false");
    CHECK(f.store.values["rnd_sync"] == "0");
    SettingsModel m2(kTable, f.env, f.store);
    CHECK_FALSE(m2.Visible(*m2.Find("steam_deck_defaults")));
}

TEST_CASE("unsaved changes are what differs from the last save") {
    Fixture f;
    SettingsModel m(kTable, f.env, f.store);
    CHECK_FALSE(m.HasUnsavedChanges());
    m.Set("lang", "ita");
    CHECK(m.HasUnsavedChanges());
    m.Set("lang", "");
    CHECK_FALSE(m.HasUnsavedChanges());
    // 1 and 1.000000 are the same speed
    m.Set("song_speed", "1");
    CHECK_FALSE(m.HasUnsavedChanges());

    m.Set("lang", "ita");
    const fs::path file = fs::temp_directory_path() / "band3_launcher_unsaved.toml";
    fs::remove(file);
    const SaveResult saved = m.Save(file);
    CHECK(saved.ok);
    CHECK_FALSE(m.HasUnsavedChanges());
    fs::remove(file);
}

TEST_CASE("Save leaves keys outside the table alone") {
    Fixture f;
    const fs::path file = fs::temp_directory_path() / "band3_launcher_keep.toml";
    {
        std::ofstream out(file, std::ios::binary);
        out << "# the player's comment\n"
               "log_level = \"debug\"\n"
               "rnd_sync = 1\n"
               "lang = \"fre\"\n"
               "window_width = 1600\n"
               "missing_cvar = true\n";
    }
    f.store.values["rnd_sync"] = "1";
    f.store.values["lang"] = "fre";
    SettingsModel m(kTable, f.env, f.store);
    m.Set("lang", "");
    m.Set("song_speed", "0.75");
    REQUIRE(m.Save(file).ok);

    const toml::table saved = toml::parse_file(file.string());
    CHECK(saved["log_level"].value<std::string>() == "debug");
    CHECK(saved["window_width"].value<int64_t>() == 1600);
    CHECK(saved["missing_cvar"].value<bool>() == true);
    CHECK(saved["rnd_sync"].value<int64_t>() == 1);
    CHECK_FALSE(saved.contains("lang"));
    CHECK(saved["song_speed"].value<double>() == 0.75);
    CHECK(saved["song_speed"].is_floating_point());
    fs::remove(file);
}

TEST_CASE("the startup box is ticked only when band3.toml has show_launcher = true") {
    Fixture f;
    SUBCASE("the default, on, as on a first run") {
        SettingsModel m(kTable, f.env, f.store);
        CHECK_FALSE(m.ShowAtStartup());
    }
    SUBCASE("ticked before: band3.toml says true") {
        f.env.cvars["show_launcher"].from_config = true;
        SettingsModel m(kTable, f.env, f.store);
        CHECK(m.ShowAtStartup());
    }
    SUBCASE("band3.toml says false") {
        f.env.cvars["show_launcher"].from_config = true;
        f.store.values["show_launcher"] = "false";
        SettingsModel m(kTable, f.env, f.store);
        CHECK_FALSE(m.ShowAtStartup());
    }
    SUBCASE("locked, it shows the command line's value and can't change") {
        f.env.cvars["show_launcher"].lock = Lock::kCommandLine;
        SettingsModel m(kTable, f.env, f.store);
        CHECK(m.ShowAtStartup());
        CHECK_FALSE(m.SetShowAtStartup(false));
        CHECK(m.ShowAtStartup());
        CHECK_FALSE(EditFor(m.Edits(), "show_launcher"));
    }
}

TEST_CASE("show_launcher is saved whatever it is, from the startup box") {
    Fixture f;
    SettingsModel m(kTable, f.env, f.store);
    CHECK_FALSE(m.HasUnsavedChanges());

    // unticked: false, though nothing was touched
    REQUIRE(EditFor(m.Edits(), "show_launcher"));
    CHECK(*EditFor(m.Edits(), "show_launcher")->value == ConfigValue(false));

    // ticked: true, the default, is written too, so the box is ticked next time
    CHECK(m.SetShowAtStartup(true));
    CHECK(m.HasUnsavedChanges());
    CHECK(*EditFor(m.Edits(), "show_launcher")->value == ConfigValue(true));
    // the cvar waits for the save
    CHECK(f.store.values["show_launcher"] == "true");

    const fs::path file = fs::temp_directory_path() / "band3_launcher_show.toml";
    fs::remove(file);
    m.SetShowAtStartup(false);
    REQUIRE(m.Save(file).ok);
    CHECK(f.store.values["show_launcher"] == "false");
    CHECK_FALSE(m.HasUnsavedChanges());
    m.SetShowAtStartup(true);
    CHECK(m.HasUnsavedChanges());
    REQUIRE(m.Save(file).ok);
    CHECK_FALSE(m.HasUnsavedChanges());
    CHECK(f.store.values["show_launcher"] == "true");
    const toml::table saved = toml::parse_file(file.string());
    CHECK(saved["show_launcher"].value<bool>() == true);
    fs::remove(file);
}

TEST_CASE("a failed save leaves show_launcher and the unsaved changes as they were") {
    Fixture f;
    SettingsModel m(kTable, f.env, f.store);
    m.SetShowAtStartup(false);
    m.Set("lang", "fre");
    // a folder where the file should be: it can't be written
    const fs::path dir = fs::temp_directory_path() / "band3_launcher_unwritable";
    fs::create_directories(dir / "band3.toml");
    const SaveResult result = m.Save(dir / "band3.toml");
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.error.empty());
    CHECK(f.store.values["show_launcher"] == "true");
    CHECK(m.HasUnsavedChanges());
    fs::remove_all(dir);
}

TEST_CASE("which settings are shown") {
    Fixture f;
    SettingsModel m(kTable, f.env, f.store);
    CHECK(m.Visible(*m.Find("midi_drums")));
    // only with MIDI drums on
    CHECK_FALSE(m.Visible(*m.Find("midi_drums_device")));
    m.Set("midi_drums", "yes");
    CHECK(m.Visible(*m.Find("midi_drums_device")));
    // drawn by the window mode row
    CHECK_FALSE(m.Visible(*m.Find("fullscreen_exclusive")));
    CHECK(m.Available(*m.Find("fullscreen_exclusive")));
    // not in this build
    CHECK_FALSE(m.Available(*m.Find("missing_cvar")));
    CHECK(m.Visible(*m.Find("input_backend")));

    f.env.windows = false;
    SettingsModel linux_model(kTable, f.env, f.store);
    CHECK_FALSE(linux_model.Visible(*linux_model.Find("input_backend")));
    CHECK_FALSE(EditFor(linux_model.Edits(), "input_backend"));
}

TEST_CASE("choices match by type") {
    Fixture f;
    SettingsModel m(kTable, f.env, f.store);
    const Setting& aspect = *m.Find("present_letterbox");
    CHECK(m.ChoiceIndex(aspect) == 0);
    m.Set("present_letterbox", "yes");
    CHECK(m.ChoiceIndex(aspect) == 1);
}

TEST_CASE("a refused value isn't recorded as an edit") {
    Fixture f;
    f.store.refuse.insert("lang");
    SettingsModel m(kTable, f.env, f.store);
    CHECK_FALSE(m.Set("lang", "klingon"));
    CHECK_FALSE(m.Set("missing_cvar", "true"));
    CHECK_FALSE(m.HasUnsavedChanges());
}

TEST_CASE("Deck warning for a render scale above 1") {
    Fixture f;
    f.env.cvars["resolution_scale"] = Facts(ValueType::kInt, "1");
    f.store.values["resolution_scale"] = "2";
    const Setting table[] = {{.cvar = "resolution_scale", .tab = Tab::kGraphics,
                              .section = "Rendering", .label = "Render scale",
                              .widget = Widget::kIntStepper}};
    SettingsModel desktop(table, f.env, f.store);
    CHECK_FALSE(desktop.Warning("resolution_scale"));
    f.env.steam_deck = true;
    SettingsModel deck(table, f.env, f.store);
    CHECK(deck.Warning("resolution_scale"));
    f.store.values["resolution_scale"] = "1";
    CHECK_FALSE(deck.Warning("resolution_scale"));
}

TEST_CASE("folders are written relative inside the anchor, absolute outside") {
    const fs::path anchor = Anchor();
    CHECK(PathForConfig("songs", anchor) == "songs");
    CHECK(PathForConfig(Generic(anchor / "songs" / "dlc"), anchor) == "songs/dlc");
    CHECK(PathForConfig("songs/", anchor) == "songs");
    CHECK(PathForConfig("./songs/../assets", anchor) == "assets");
    const fs::path outside = fs::temp_directory_path() / "elsewhere" / "rb3";
    CHECK(PathForConfig(outside.string(), anchor) == Generic(outside.lexically_normal()));
    CHECK(PathForConfig("../shared/rb3", anchor) ==
          Generic((anchor / ".." / "shared" / "rb3").lexically_normal()));
    CHECK(PathForConfig("", anchor) == "");
#ifdef _WIN32
    // backslashes become forward slashes
    CHECK(PathForConfig("D:\\Games\\RB3", anchor) == "D:/Games/RB3");
    CHECK(PathForConfig(anchor.string() + "\\assets", anchor) == "assets");
#endif

    CHECK(FolderListForConfig(" songs | " + Generic(anchor / "dlc") + " ||" + outside.string(),
                              anchor) ==
          "songs|dlc|" + Generic(outside.lexically_normal()));
}

TEST_CASE("folder settings are compared and saved as the launcher writes them") {
    Fixture f;
    SettingsModel m(kTable, f.env, f.store);
    // the default folder typed in full isn't a change
    m.Set("game_data_root", Generic(Anchor() / "assets"));
    CHECK_FALSE(m.IsChanged("game_data_root"));
    m.Set("game_data_root", Generic(Anchor() / "rb3"));
    CHECK(m.IsChanged("game_data_root"));
    const fs::path outside = fs::temp_directory_path() / "elsewhere" / "songs";
    m.Set("content_folders", Generic(Anchor() / "songs"));
    CHECK_FALSE(m.IsChanged("content_folders"));
    m.Set("content_folders", "songs|" + outside.string());
    CHECK(m.IsChanged("content_folders"));

    const auto edits = m.Edits();
    CHECK(*EditFor(edits, "game_data_root")->value == ConfigValue(std::string("rb3")));
    CHECK(*EditFor(edits, "content_folders")->value ==
          ConfigValue("songs|" + Generic(outside.lexically_normal())));
}

TEST_CASE("joypad_lag's editor changes one type's game lag and keeps the rest") {
    CHECK(LagFor("5=20, 8=30/43/19", 5) == 20.0f);
    CHECK(LagFor("5=20, 8=30/43/19", 8) == 30.0f);
    CHECK_FALSE(LagFor("5=20", 8));
    CHECK_FALSE(LagFor("8=/30/", 8));

    // a new type goes at the end
    CHECK(WithLag("", 5, 20.0f) == "5=20");
    CHECK(WithLag("5=20", 8, 12.5f) == "5=20,8=12.5");
    // the calibration parts stay
    CHECK(WithLag("5=20, 8=30/43/19", 8, 35.0f) == "5=20,8=35/43/19");
    CHECK(WithLag("8=/30/", 8, 10.0f) == "8=10/30/");
    // clearing keeps the parts it doesn't edit, and drops an empty entry
    CHECK(WithLag("8=30/43/19", 8, std::nullopt) == "8=/43/19");
    CHECK(WithLag("5=20,8=30", 8, std::nullopt) == "5=20");
    CHECK(WithLag("8=30//", 8, std::nullopt) == "");
    CHECK(WithLag("5=20", 8, std::nullopt) == "5=20");
    // entries it can't read stay as they are
    CHECK(WithLag("oops, 5=20", 5, 25.0f) == "oops,5=25");
    // the last entry for a type is the one the game reads
    CHECK(WithLag("5=10,5=20", 5, 30.0f) == "5=10,5=30");

    // round trip: what the editor writes reads back the same
    std::string text = "8=30/43/19, 99=bad";
    text = WithLag(text, 5, 22.0f);
    text = WithLag(text, 33, -4.0f);
    CHECK(LagFor(text, 5) == 22.0f);
    CHECK(LagFor(text, 33) == -4.0f);
    CHECK(LagFor(text, 8) == 30.0f);
    band3::input::JoypadLagOverrides overrides{};
    const auto bad = band3::input::ParseJoypadLagOverrides(text, overrides);
    REQUIRE(bad.size() == 1);
    CHECK(bad[0] == "99=bad");
    CHECK(overrides[8]->video_calibration == 43.0f);
    CHECK(overrides[8]->audio_calibration == 19.0f);

    for (const auto& t : LagEditorTypes()) CHECK(t.type < band3::input::kLagJoypadTypes);
}

TEST_CASE("mic slots join without trailing blanks") {
    const std::string slots[] = {"Yeti", "", " USB Mic ", ""};
    CHECK(JoinMicSlots(slots) == "Yeti,,USB Mic");
    const std::string empty[] = {"", "", "", ""};
    CHECK(JoinMicSlots(empty) == "");
    const std::string second[] = {"", "Yeti"};
    CHECK(JoinMicSlots(second) == ",Yeti");
}

TEST_CASE("the launcher's table is consistent") {
    const auto table = SettingTable();
    std::set<std::string_view> names;
    for (const auto& s : table) {
        INFO(s.cvar);
        CHECK(names.insert(s.cvar).second);
        CHECK_FALSE(s.label.empty());
        CHECK_FALSE(s.section.empty());
        if (s.widget == Widget::kCombo || s.widget == Widget::kComboText) {
            CHECK_FALSE(s.choices.empty());
        }
        if (s.widget == Widget::kIntSlider || s.widget == Widget::kIntStepper ||
            s.widget == Widget::kFloatSlider || s.widget == Widget::kPercentSlider) {
            REQUIRE(s.range);
            CHECK(s.range->min < s.range->max);
        }
    }
    for (const auto& s : table) {
        INFO(s.cvar);
        if (!s.shown_when.cvar.empty()) CHECK(FindSetting(table, s.shown_when.cvar));
        if (!s.companion.empty()) CHECK(FindSetting(table, s.companion));
    }
    // debug settings stay in F4
    for (const char* debug : {"autoplay", "test_port", "native_view_backend",
                              "virtual_instrument", "main_heap_size", "char_heap_size"}) {
        CHECK_FALSE(FindSetting(table, debug));
    }
    for (const char* wanted : {"game_data_root", "user_data_root", "content_folders",
                               "show_launcher", "steam_deck_defaults", "joypad_lag"}) {
        CHECK(FindSetting(table, wanted));
    }
    const auto sections = SectionsOf(table, Tab::kGame);
    REQUIRE_FALSE(sections.empty());
    CHECK(sections.front() == "Folders");
}
