// Checks the launcher's settings model (src/Launcher/launcher_settings.h):
// effective defaults, typed comparison, reset, locks, the Steam Deck toggle,
// what Save writes, the renderer's per-platform default, the native
// resolution limit's row, and the joypad_lag
// and folder helpers.

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <toml++/toml.hpp>
#include "src/Input/joypad_lag_status.h"
#include "src/Launcher/generated_rows.h"
#include "src/Launcher/launcher_settings.h"
#include "src/renderer_default.h"

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

TEST_CASE("GoCentral warns until the profile name is one of your own") {
    Fixture f;
    f.env.cvars["gocentral"] = Facts(ValueType::kBool, "false");
    f.env.cvars["username"] = Facts(ValueType::kString, "User");
    f.store.values["gocentral"] = "false";
    f.store.values["username"] = "User";
    const Setting table[] = {
        {.cvar = "username", .tab = Tab::kGame, .section = "Profile", .label = "Profile name",
         .widget = Widget::kText},
        {.cvar = "gocentral", .tab = Tab::kOnline, .section = "GoCentral", .label = "GoCentral",
         .widget = Widget::kCheckbox},
    };
    SettingsModel m(table, f.env, f.store);
    // off, nothing to warn about
    CHECK_FALSE(m.Warning("gocentral"));
    m.Set("gocentral", "true");
    CHECK(m.Warning("gocentral"));
    m.Set("username", "  ");
    CHECK(m.Warning("gocentral"));
    m.Set("username", "Stagehand");
    CHECK_FALSE(m.Warning("gocentral"));
    CHECK_FALSE(m.Warning("username"));
}

TEST_CASE("Liveless warns when the game to join isn't an address") {
    Fixture f;
    f.env.cvars["liveless"] = Facts(ValueType::kBool, "false");
    f.env.cvars["liveless_connect"] = Facts(ValueType::kString, "127.0.0.1");
    f.env.cvars["liveless_port"] = Facts(ValueType::kInt, "9103");
    f.store.values["liveless"] = "false";
    f.store.values["liveless_connect"] = "";
    f.store.values["liveless_port"] = "9103";
    const Setting table[] = {
        {.cvar = "liveless", .tab = Tab::kOnline, .section = "Online play", .label = "Liveless",
         .widget = Widget::kCheckbox},
        {.cvar = "liveless_connect", .tab = Tab::kOnline, .section = "Online play",
         .label = "Game to join", .widget = Widget::kText, .shown_when = {"liveless", "true"}},
        {.cvar = "liveless_port", .tab = Tab::kOnline, .section = "Online play",
         .label = "Port", .widget = Widget::kIntStepper, .shown_when = {"liveless", "true"}},
    };
    SettingsModel m(table, f.env, f.store);
    // hidden while Liveless is off
    CHECK_FALSE(m.Visible(*m.Find("liveless_connect")));
    CHECK_FALSE(m.Warning("liveless_connect"));
    m.Set("liveless", "true");
    CHECK(m.Visible(*m.Find("liveless_connect")));
    CHECK(m.Warning("liveless_connect"));
    m.Set("liveless_connect", "192.168.1.20:");
    CHECK(m.Warning("liveless_connect"));
    m.Set("liveless_connect", "192.168.1.20:9203");
    CHECK_FALSE(m.Warning("liveless_connect"));
    m.Set("liveless_connect", "127.0.0.1");
    CHECK_FALSE(m.Warning("liveless_connect"));
}

TEST_CASE("a row shown while a setting isn't empty follows any value of it") {
    Fixture f;
    f.env.cvars["ha_mqtt_host"] = Facts(ValueType::kString, "");
    f.env.cvars["ha_mqtt_port"] = Facts(ValueType::kInt, "1883");
    f.store.values["ha_mqtt_host"] = "";
    f.store.values["ha_mqtt_port"] = "1883";
    const Setting table[] = {
        {.cvar = "ha_mqtt_host", .tab = Tab::kOnline, .section = "Home Assistant",
         .label = "MQTT broker", .widget = Widget::kText},
        {.cvar = "ha_mqtt_port", .tab = Tab::kOnline, .section = "Home Assistant",
         .label = "Port", .widget = Widget::kIntStepper,
         .shown_when = {.cvar = "ha_mqtt_host", .not_empty = true}},
    };
    SettingsModel m(table, f.env, f.store);
    CHECK_FALSE(m.Visible(*m.Find("ha_mqtt_port")));
    m.Set("ha_mqtt_host", "homeassistant.local");
    CHECK(m.Visible(*m.Find("ha_mqtt_port")));
    // "false" or "0" are still a host name
    m.Set("ha_mqtt_host", "0");
    CHECK(m.Visible(*m.Find("ha_mqtt_port")));
    m.Set("ha_mqtt_host", "");
    CHECK_FALSE(m.Visible(*m.Find("ha_mqtt_port")));
}

TEST_CASE("a password row's refusal doesn't repeat what was typed") {
    Fixture f;
    f.env.cvars["ha_mqtt_username"] = Facts(ValueType::kString, "");
    f.env.cvars["ha_mqtt_password"] = Facts(ValueType::kString, "");
    const Setting table[] = {
        {.cvar = "ha_mqtt_username", .tab = Tab::kOnline, .section = "Home Assistant",
         .label = "User name", .widget = Widget::kText},
        {.cvar = "ha_mqtt_password", .tab = Tab::kOnline, .section = "Home Assistant",
         .label = "Password", .widget = Widget::kPassword},
    };
    SettingsModel m(table, f.env, f.store);
    CHECK(m.Secret("ha_mqtt_password"));
    CHECK_FALSE(m.Secret("ha_mqtt_username"));
    CHECK_FALSE(m.Secret("not_in_the_table"));
    CHECK(m.Refusal("ha_mqtt_password", "hunter2") == "Not accepted");
    CHECK(m.Refusal("ha_mqtt_username", "pat") == "Not accepted: \"pat\"");
    // it's still saved as any text is
    m.Set("ha_mqtt_password", "hunter2");
    CHECK(m.Value("ha_mqtt_password") == "hunter2");
    CHECK(m.IsChanged("ha_mqtt_password"));
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

TEST_CASE("the renderer's default is native on Windows, and an untouched one isn't saved") {
#ifdef _WIN32
    CHECK(std::string_view(band3::settings::kDefaultRenderer) == "native");
#else
    CHECK(std::string_view(band3::settings::kDefaultRenderer) == "emulated");
#endif
    // the registry's default is the build's, as ReadEnvironment reads it, so a
    // fresh start leaves the key out of band3.toml on either platform
    const std::string other =
        std::string_view(band3::settings::kDefaultRenderer) == "native" ? "emulated" : "native";
    Fixture f;
    f.env.cvars["renderer"] = Facts(ValueType::kString, band3::settings::kDefaultRenderer);
    f.env.cvars["renderer"].allowed = {"native", "emulated", "both"};
    f.store.values["renderer"] = band3::settings::kDefaultRenderer;
    const Setting* row = FindSetting(SettingTable(), "renderer");
    REQUIRE(row);
    const Setting table[] = {*row};
    SettingsModel m(table, f.env, f.store);
    CHECK(m.EffectiveDefault("renderer") == band3::settings::kDefaultRenderer);
    CHECK_FALSE(m.IsChanged("renderer"));
    CHECK(m.ChoiceIndex(table[0]) >= 0);
    REQUIRE(EditFor(m.Edits(), "renderer"));
    CHECK_FALSE(EditFor(m.Edits(), "renderer")->value);
    // the other one is a choice the player made, and is saved
    CHECK(m.Set("renderer", other));
    CHECK(m.IsChanged("renderer"));
    CHECK(m.ChoiceIndex(table[0]) >= 0);
    CHECK(*EditFor(m.Edits(), "renderer")->value == ConfigValue(other));
    // and Reset takes it out again
    CHECK(m.Reset("renderer"));
    CHECK_FALSE(EditFor(m.Edits(), "renderer")->value);
}

namespace {

// the real Graphics tab, every cvar there registered (`plugin`: the emulated
// GPU's own too, as in a run with it), renderer set to `renderer`
struct GraphicsTab {
    Fixture f;
    std::optional<SettingsModel> model;

    GraphicsTab(const char* renderer, bool plugin = true) {
        for (const Setting& s : SettingTable()) {
            if (s.tab != Tab::kGraphics) continue;
            f.env.cvars[std::string(s.cvar)] = Facts(ValueType::kString, "");
        }
        f.env.cvars["renderer"].registry_default = band3::settings::kDefaultRenderer;
        f.env.cvars["renderer"].allowed = {"native", "emulated", "both"};
        f.store.values["renderer"] = renderer;
        // VSync shows only with the frame cap off
        f.store.values["frame_cap"] = "off";
        if (!plugin) {
            for (const char* cvar :
                 {"vsync", "resolution_scale", "swap_post_effect", "anisotropic_override"}) {
                f.env.cvars[cvar].exists = false;
            }
        }
        f.env.emulated_gpu_running = plugin;
        model.emplace(SettingTable(), f.env, f.store);
    }

    std::set<std::string> Shown() const {
        std::set<std::string> shown;
        for (const Setting& s : SettingTable()) {
            if (s.tab == Tab::kGraphics && model->Visible(s)) shown.insert(std::string(s.cvar));
        }
        return shown;
    }
};

const std::set<std::string> kEveryRenderer = {
    "monitor", "fullscreen", "resolution", "present_letterbox", "frame_cap", "renderer",
    "rnd_sync", "background_fps", "disable_hair_shader", "disable_approximate_lights"};
const std::set<std::string> kNativeRows = {"native_fill_window", "native_view_msaa",
                                           "native_anisotropic", "native_max_height",
                                           "native_present_pacing"};
const std::set<std::string> kEmulatedRows = {"resolution_scale", "swap_post_effect",
                                             "anisotropic_override", "vsync",
                                             "compress_character_textures"};

std::set<std::string> Union(std::initializer_list<std::set<std::string>> sets) {
    std::set<std::string> out;
    for (const auto& s : sets) out.insert(s.begin(), s.end());
    return out;
}

}  // namespace

TEST_CASE("the Graphics tab shows each group only for the renderers it applies to") {
    CHECK(GraphicsTab("native").Shown() == Union({kEveryRenderer, kNativeRows}));
    CHECK(GraphicsTab("emulated").Shown() == Union({kEveryRenderer, kEmulatedRows}));
    // both: everything, and what the emulated GPU does while the native picture shows
    CHECK(GraphicsTab("both").Shown() ==
          Union({kEveryRenderer, kNativeRows, kEmulatedRows, {"emulated_gpu_while_native"}}));
    // the groups have their own sections
    const auto sections = SectionsOf(SettingTable(), Tab::kGraphics);
    CHECK(sections == std::vector<std::string_view>{"Latency", "Display", "Renderer",
                                                    "Native renderer", "Emulated GPU", "Game"});
}

TEST_CASE("the Latency section says what its rows do and to calibrate again") {
    for (const char* cvar : {"frame_cap", "native_present_pacing"}) {
        const Setting* row = FindSetting(SettingTable(), cvar);
        REQUIRE(row);
        CHECK(row->section == "Latency");
        CHECK_FALSE(row->note.empty());
    }
    const auto note = GraphicsTab("native").model->SectionNote(Tab::kGraphics, "Latency");
    REQUIRE(note);
    CHECK(note->find("calibration") != std::string::npos);
}

TEST_CASE("the Lowest latency cap is the most whole refreshes' worth up to 240") {
    CHECK(LowestLatencyCap(60) == "240");
    CHECK(LowestLatencyCap(59.94) == "240");
    CHECK(LowestLatencyCap(120) == "240");
    CHECK(LowestLatencyCap(144) == "144");
    CHECK(LowestLatencyCap(165) == "165");
    CHECK(LowestLatencyCap(75) == "225");
    CHECK(LowestLatencyCap(100) == "200");
    // the cap's top, for a display at or past it, or one that can't be told
    CHECK(LowestLatencyCap(240) == "240");
    CHECK(LowestLatencyCap(360) == "240");
    CHECK(LowestLatencyCap(0) == "240");
}

TEST_CASE("the emulated GPU's own settings, chosen in a run without it, wait for the restart") {
    // renderer native at startup: the plugin's settings don't exist
    GraphicsTab tab("emulated", false);
    CHECK(tab.Shown() == Union({kEveryRenderer, {"compress_character_textures"}}));
    const auto note = tab.model->SectionNote(Tab::kGraphics, "Emulated GPU");
    REQUIRE(note);
    CHECK(note->find("Play restarts band3") != std::string::npos);
    // nothing to say for native, nor where the emulated GPU runs
    CHECK_FALSE(GraphicsTab("native", false).model->SectionNote(Tab::kGraphics, "Emulated GPU"));
    CHECK_FALSE(GraphicsTab("emulated").model->SectionNote(Tab::kGraphics, "Emulated GPU"));
    CHECK_FALSE(tab.model->SectionNote(Tab::kGraphics, "Native renderer"));
}

TEST_CASE("renderer offers native on Windows only") {
    const Setting* row = FindSetting(SettingTable(), "renderer");
    REQUIRE(row);
    REQUIRE(row->choices.size() == 3);
    GraphicsTab windows("emulated");
    for (const Choice& c : row->choices) CHECK(windows.model->Offered(c));
    Fixture f;
    f.env.windows = false;
    SettingsModel elsewhere(SettingTable(), f.env, f.store);
    for (const Choice& c : row->choices) {
        INFO(c.value);
        CHECK(elsewhere.Offered(c) == (c.value != "native"));
    }
}

TEST_CASE("Save removes the retired emulated_gpu's key") {
    // MigrateRendererSettings has cleared it: at its default, so Save takes
    // the key out of band3.toml
    GraphicsTab tab("both");
    const auto edits = tab.model->Edits();
    REQUIRE(EditFor(edits, "emulated_gpu"));
    CHECK_FALSE(EditFor(edits, "emulated_gpu")->value);
    const Setting* row = FindSetting(SettingTable(), "emulated_gpu");
    REQUIRE(row);
    CHECK_FALSE(tab.model->Visible(*row));
}

TEST_CASE("the native resolution limit shows with the native renderer, and saves a number") {
    Fixture f;
    f.env.cvars["renderer"] = Facts(ValueType::kString, "native");
    f.env.cvars["native_max_height"] = Facts(ValueType::kInt, "0");
    f.env.cvars["native_max_height"].min = 0;
    f.env.cvars["native_max_height"].max = 4320;
    f.store.values["renderer"] = "native";
    f.store.values["native_max_height"] = "0";
    const Setting* renderer = FindSetting(SettingTable(), "renderer");
    const Setting* height = FindSetting(SettingTable(), "native_max_height");
    REQUIRE(renderer);
    REQUIRE(height);
    const Setting table[] = {*renderer, *height};
    SettingsModel m(table, f.env, f.store);
    CHECK(m.Visible(table[1]));
    CHECK(m.ChoiceIndex(table[1]) == 0);
    CHECK_FALSE(m.IsChanged("native_max_height"));
    // a preset, then a typed height, saved as numbers
    CHECK(m.Set("native_max_height", "1080"));
    CHECK(m.ChoiceIndex(table[1]) >= 0);
    CHECK(*EditFor(m.Edits(), "native_max_height")->value == ConfigValue(int64_t{1080}));
    CHECK(m.Set("native_max_height", "900"));
    CHECK(m.ChoiceIndex(table[1]) == -1);
    CHECK(*EditFor(m.Edits(), "native_max_height")->value == ConfigValue(int64_t{900}));
    // the emulated GPU draws at its own resolution (Render scale); both has
    // the native renderer too
    CHECK(m.Set("renderer", "emulated"));
    CHECK_FALSE(m.Visible(table[1]));
    CHECK(m.Set("renderer", "both"));
    CHECK(m.Visible(table[1]));
}

TEST_CASE("the launcher's table is consistent") {
    const auto table = SettingTable();
    std::set<std::string_view> names;
    for (const auto& s : table) {
        INFO(s.cvar);
        CHECK(names.insert(s.cvar).second);
        CHECK_FALSE(s.label.empty());
        CHECK_FALSE(s.section.empty());
        if (s.widget == Widget::kCombo || s.widget == Widget::kComboText ||
            s.widget == Widget::kMonitor || s.widget == Widget::kResolution) {
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
    // debug settings aren't the launcher's: the in-game Advanced tab has them
    for (const char* debug : {"autoplay", "test_port", "native_view_backend",
                              "virtual_instrument", "main_heap_size", "char_heap_size"}) {
        CHECK_FALSE(FindSetting(table, debug));
    }
    for (const auto& s : table) CHECK(s.tab != Tab::kAdvanced);
    for (const char* wanted : {"game_data_root", "user_data_root", "content_folders",
                               "show_launcher", "steam_deck_defaults", "joypad_lag"}) {
        CHECK(FindSetting(table, wanted));
    }
    // the online settings, on the Online tab; what they need shows only with them on
    for (const char* online : {"gocentral", "gocentral_address", "liveless", "liveless_connect",
                               "liveless_external_ip", "liveless_port"}) {
        INFO(online);
        const Setting* s = FindSetting(table, online);
        REQUIRE(s);
        CHECK(s->tab == Tab::kOnline);
        CHECK(s->windows_only);
    }
    CHECK(FindSetting(table, "gocentral_address")->shown_when.cvar == "gocentral");
    for (const char* needs_liveless : {"liveless_connect", "liveless_external_ip", "liveless_port"}) {
        CHECK(FindSetting(table, needs_liveless)->shown_when.cvar == "liveless");
    }
    // Home Assistant: the broker's settings show once there's a broker; the
    // webhook doesn't need one
    for (const char* needs_broker : {"ha_mqtt_port", "ha_mqtt_username", "ha_mqtt_password",
                                     "ha_discovery_prefix", "ha_stagekit"}) {
        INFO(needs_broker);
        const Setting* s = FindSetting(table, needs_broker);
        REQUIRE(s);
        CHECK(s->tab == Tab::kOnline);
        CHECK(s->shown_when.cvar == "ha_mqtt_host");
        CHECK(s->shown_when.not_empty);
    }
    CHECK(FindSetting(table, "ha_mqtt_host")->shown_when.cvar.empty());
    CHECK(FindSetting(table, "ha_webhook_url")->shown_when.cvar.empty());
    CHECK(FindSetting(table, "ha_mqtt_password")->widget == Widget::kPassword);
    // liveless_port's own range (settings.cpp)
    const Setting* port = FindSetting(table, "liveless_port");
    REQUIRE(port->range);
    CHECK(port->range->min == 1024);
    CHECK(port->range->max == 65000);
    const auto sections = SectionsOf(table, Tab::kGame);
    REQUIRE_FALSE(sections.empty());
    CHECK(sections.front() == "Folders");
}

// In game (F4)

TEST_CASE("in game, the page leaves out what reads the devices or the microphones itself") {
    const PageFeatures launcher = FeaturesFor(Where::kLauncher);
    CHECK(launcher.device_tester);
    CHECK(launcher.mic_meters);
    CHECK_FALSE(launcher.generated_rows);
    CHECK_FALSE(launcher.next_start_notes);

    const PageFeatures in_game = FeaturesFor(Where::kInGame);
    CHECK_FALSE(in_game.device_tester);
    CHECK_FALSE(in_game.mic_meters);
    CHECK(in_game.generated_rows);
    CHECK(in_game.next_start_notes);

    // the mic slots themselves stay: they're a setting, not a device
    Fixture f;
    f.env.in_game = true;
    f.env.cvars["usb_mics"] = Facts(ValueType::kBool, "false");
    f.env.cvars["usb_mic_devices"] = Facts(ValueType::kString, "");
    f.store.values["usb_mics"] = "true";
    f.store.values["usb_mic_devices"] = "";
    SettingsModel m(SettingTable(), f.env, f.store);
    CHECK(m.Visible(*m.Find("usb_mic_devices")));
}

namespace {

// a fresh start's values, in game: what the game started with is what they are
struct InGame : Fixture {
    InGame() {
        env.in_game = true;
        for (auto& [name, facts] : env.cvars) {
            if (facts.exists) facts.started_with = store.Get(name);
        }
        // the registry's lifecycles (settings.cpp)
        env.cvars["lang"].lifecycle = Lifecycle::kRequiresRestart;
        env.cvars["midi_drums"].lifecycle = Lifecycle::kRequiresRestart;
        env.cvars["content_folders"].lifecycle = Lifecycle::kRequiresRestart;
        // HotReload, but read once (ReadEnvironment's in-game list)
        env.cvars["game_data_root"].read_once = true;
        env.cvars["input_backend"].read_once = true;
    }
};

}  // namespace

TEST_CASE("in game, a row whose change waits for the next start says so") {
    InGame f;
    SettingsModel m(kTable, f.env, f.store);

    SUBCASE("a restart-only setting, by its lifecycle, once it differs from the start's") {
        CHECK(m.NextStartOnly("lang"));
        CHECK_FALSE(m.WaitsForNextStart("lang"));
        CHECK(m.Set("lang", "fre"));
        CHECK(m.WaitsForNextStart("lang"));
        // back to what the game started with: nothing waits
        CHECK(m.Set("lang", ""));
        CHECK_FALSE(m.WaitsForNextStart("lang"));
    }
    SUBCASE("a setting the game reads once as it starts, though it's HotReload") {
        CHECK(m.NextStartOnly("input_backend"));
        CHECK(m.Set("input_backend", "xinput"));
        CHECK(m.WaitsForNextStart("input_backend"));
        CHECK(m.NextStartOnly("game_data_root"));
        CHECK(m.Set("game_data_root", "D:/rb3"));
        CHECK(m.WaitsForNextStart("game_data_root"));
    }
    SUBCASE("a setting that applies at once never waits") {
        CHECK_FALSE(m.NextStartOnly("song_speed"));
        CHECK(m.Set("song_speed", "1.5"));
        CHECK_FALSE(m.WaitsForNextStart("song_speed"));
    }
    SUBCASE("compared by type, as the row shows it") {
        f.env.cvars["midi_drums"].started_with = "false";
        SettingsModel typed(kTable, f.env, f.store);
        CHECK(typed.Set("midi_drums", "0"));
        CHECK_FALSE(typed.WaitsForNextStart("midi_drums"));
    }
    SUBCASE("on the launcher nothing waits: Play applies it, or restarts band3") {
        f.env.in_game = false;
        SettingsModel launcher(kTable, f.env, f.store);
        CHECK_FALSE(launcher.NextStartOnly("lang"));
        CHECK(launcher.Set("lang", "fre"));
        CHECK_FALSE(launcher.WaitsForNextStart("lang"));
    }
}

TEST_CASE("in game, renderer waits for the next start only for the other GPU") {
    InGame f;
    f.env.cvars["renderer"] = Facts(ValueType::kString, "native");
    f.env.cvars["renderer"].allowed = {"native", "emulated", "both"};
    const Setting row{.cvar = "renderer", .tab = Tab::kGraphics, .section = "Renderer",
                      .label = "Renderer", .widget = Widget::kCombo};
    const Setting table[] = {row};

    SUBCASE("a native run: emulated and both need the emulated GPU") {
        f.env.emulated_gpu_running = false;
        f.store.values["renderer"] = "native";
        SettingsModel m(table, f.env, f.store);
        CHECK_FALSE(m.NextStartOnly("renderer"));
        CHECK_FALSE(m.WaitsForNextStart("renderer"));
        CHECK(m.Set("renderer", "both"));
        CHECK(m.WaitsForNextStart("renderer"));
        CHECK(m.Set("renderer", "emulated"));
        CHECK(m.WaitsForNextStart("renderer"));
    }
    SUBCASE("an emulated run: emulated and both switch at once, native waits") {
        f.env.emulated_gpu_running = true;
        f.store.values["renderer"] = "emulated";
        SettingsModel m(table, f.env, f.store);
        CHECK(m.Set("renderer", "both"));
        CHECK_FALSE(m.WaitsForNextStart("renderer"));
        CHECK(m.Set("renderer", "native"));
        CHECK(m.WaitsForNextStart("renderer"));
    }
    SUBCASE("a build that can't present native alone runs it as emulated") {
        f.env.emulated_gpu_running = true;
        f.env.native_presentable = false;
        f.store.values["renderer"] = "emulated";
        SettingsModel m(table, f.env, f.store);
        CHECK(m.Set("renderer", "native"));
        CHECK_FALSE(m.WaitsForNextStart("renderer"));
    }
}

TEST_CASE("in game, the window mode's row waits when its companion does") {
    InGame f;
    f.env.cvars["fullscreen_exclusive"].lifecycle = Lifecycle::kRequiresRestart;
    SettingsModel m(kTable, f.env, f.store);
    CHECK_FALSE(m.WaitsForNextStart("fullscreen"));
    CHECK(m.SetWindowMode(WindowMode::kExclusive));
    CHECK(m.WaitsForNextStart("fullscreen"));
}

TEST_CASE("in game, a start-only setting is shown read-only and Save leaves its key") {
    InGame f;
    f.env.cvars["midi_drums_device"].lifecycle = Lifecycle::kInitOnly;
    f.store.values["midi_drums_device"] = "kit";
    SettingsModel m(kTable, f.env, f.store);
    CHECK(m.ReadOnly("midi_drums_device"));
    CHECK_FALSE(m.Set("midi_drums_device", "other"));
    CHECK_FALSE(m.Reset("midi_drums_device"));
    CHECK(f.store.values["midi_drums_device"] == "kit");
    CHECK_FALSE(EditFor(m.Edits(), "midi_drums_device"));
    CHECK_FALSE(m.HasUnsavedChanges());

    // on the launcher, before the game starts, it's set as any other
    f.env.in_game = false;
    SettingsModel launcher(kTable, f.env, f.store);
    CHECK_FALSE(launcher.ReadOnly("midi_drums_device"));
}

TEST_CASE("in game, the folders say they apply at the next start") {
    InGame f;
    SettingsModel in_game(kTable, f.env, f.store);
    CHECK(in_game.SectionNote(Tab::kGame, "Folders"));
    CHECK_FALSE(in_game.SectionNote(Tab::kGame, "Game"));
    f.env.in_game = false;
    SettingsModel launcher(kTable, f.env, f.store);
    CHECK_FALSE(launcher.SectionNote(Tab::kGame, "Folders"));
}

TEST_CASE("in game, Save writes generated rows as the launcher would: overrides only") {
    InGame f;
    std::vector<RegistryCvar> registry(2);
    registry[0].name = "dred";
    registry[0].category = "Band3/Advanced/Logging";
    registry[0].type = ValueType::kBool;
    registry[1].name = "native_slow_frame_ms";
    registry[1].category = "Band3/Advanced/Native renderer";
    registry[1].type = ValueType::kInt;
    const GeneratedRows generated(registry, kTable);
    const std::vector<Setting> table = JoinTables(kTable, generated.Rows());
    f.env.cvars["dred"] = Facts(ValueType::kBool, "false");
    f.env.cvars["native_slow_frame_ms"] = Facts(ValueType::kInt, "12");
    f.store.values["dred"] = "false";
    f.store.values["native_slow_frame_ms"] = "20";

    const fs::path file = fs::temp_directory_path() / "band3_ingame_save.toml";
    {
        std::ofstream out(file, std::ios::binary);
        out << "log_level = \"debug\"\n"
               "native_slow_frame_ms = 20\n"
               "bind_settings = \"F5\"\n";
    }
    SettingsModel m(table, f.env, f.store);
    CHECK(m.Set("dred", "true"));
    CHECK(m.Reset("native_slow_frame_ms"));
    CHECK(m.HasUnsavedChanges());
    REQUIRE(m.Save(file).ok);
    CHECK_FALSE(m.HasUnsavedChanges());

    const toml::table saved = toml::parse_file(file.string());
    // keys outside the page, the SDK's and the key binds, are kept
    CHECK(saved["log_level"].value<std::string>() == "debug");
    CHECK(saved["bind_settings"].value<std::string>() == "F5");
    // the change is written, typed; the reset one's key goes
    CHECK(saved["dred"].value<bool>() == true);
    CHECK_FALSE(saved.contains("native_slow_frame_ms"));
    // settings at their defaults aren't written
    CHECK_FALSE(saved.contains("lang"));
    CHECK_FALSE(saved.contains("song_speed"));
    fs::remove(file);
}
