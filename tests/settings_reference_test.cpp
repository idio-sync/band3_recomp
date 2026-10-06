// Checks the settings reference (src/Launcher/settings_reference.h): tabs and
// sections in the page's order, what each row says, and what a table cell
// can't hold; and that the checked-in docs/settings-reference.md has every
// setting src/settings.cpp defines, with its description, so CI catches a
// reference nobody regenerated.

#include <doctest/doctest.h>
#include <cctype>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "src/Launcher/settings_reference.h"

using namespace band3::launcher;

namespace {

RegistryCvar Cvar(std::string name, ValueType type, std::string default_value,
                  std::string description = "What it does") {
    RegistryCvar c;
    c.name = std::move(name);
    c.type = type;
    c.default_value = std::move(default_value);
    c.description = std::move(description);
    return c;
}

constexpr Choice kTypes[] = {{"-1", "Don't override"}, {"7", "Guitar"}};
constexpr Choice kCaps[] = {{"display", "The display's"}, {"60", "60 fps"}};

constexpr Setting kPage[] = {
    {.cvar = "fast_start", .tab = Tab::kGame, .section = "Game", .label = "Skip the splash screens",
     .widget = Widget::kCheckbox},
    {.cvar = "lang", .tab = Tab::kGame, .section = "Profile", .label = "Language",
     .widget = Widget::kCombo},
    {.cvar = "controller_type", .tab = Tab::kControllers, .section = "Instruments",
     .label = "Gamepads play as", .widget = Widget::kCombo, .choices = kTypes},
    {.cvar = "frame_cap", .tab = Tab::kGraphics, .section = "Display", .label = "Frame rate cap",
     .widget = Widget::kComboText, .choices = kCaps},
    {.cvar = "song_speed", .tab = Tab::kGame, .section = "Game", .label = "Song speed",
     .widget = Widget::kFloatSlider, .unit = "x"},
    // registered in another build only
    {.cvar = "missing", .tab = Tab::kGame, .section = "Game", .label = "Missing",
     .widget = Widget::kCheckbox},
    {.cvar = "test_port", .tab = Tab::kAdvanced, .section = "Test harness", .label = "test_port",
     .widget = Widget::kIntStepper},
};

constexpr band3::LegacyIniKey kIni[] = {
    {"game", "fast_start", "fast_start"},
    {"window", "width", "window_width"},
    {"window", "height", "window_height"},
};

std::vector<RegistryCvar> Cvars() {
    std::vector<RegistryCvar> cvars = {
        Cvar("fast_start", ValueType::kBool, "false"),
        Cvar("lang", ValueType::kString, ""),
        Cvar("controller_type", ValueType::kInt, "7"),
        Cvar("frame_cap", ValueType::kString, "display"),
        Cvar("song_speed", ValueType::kFloat, "1.000000"),
        Cvar("test_port", ValueType::kInt, "0", "The harness's port"),
        Cvar("window_width", ValueType::kInt, "1280"),
        Cvar("window_height", ValueType::kInt, "720"),
    };
    cvars[1].allowed = {"", "eng", "fre"};
    cvars[1].lifecycle = Lifecycle::kRequiresRestart;
    cvars[4].min = 0.5;
    cvars[4].max = 2;
    cvars[5].min = 0;
    cvars[5].max = 65535;
    return cvars;
}

std::string Reference(std::span<const Setting> page = kPage) {
    const std::vector<RegistryCvar> cvars = Cvars();
    return SettingsReference({.page = page, .cvars = cvars, .ini_keys = kIni, .platform = "Windows"});
}

// the line about `cvar` (its row starts with its name)
std::string Row(const std::string& text, std::string_view cvar) {
    const size_t at = text.find("| `" + std::string(cvar) + "`");
    if (at == std::string::npos) return {};
    return text.substr(at, text.find('\n', at) - at);
}

}

TEST_CASE("the reference goes tab by tab, then section by section, in the page's order") {
    const std::string text = Reference();
    const size_t game = text.find("\n## Game\n");
    const size_t graphics = text.find("\n## Graphics\n");
    const size_t controllers = text.find("\n## Controllers\n");
    const size_t advanced = text.find("\n## Advanced\n");
    REQUIRE(game != std::string::npos);
    CHECK(game < graphics);
    CHECK(graphics < controllers);
    CHECK(controllers < advanced);
    // Game has two sections, so each has a heading; the others have one, and none
    CHECK(text.find("### Game\n") < text.find("### Profile\n"));
    CHECK(text.find("### Display") == std::string::npos);
    CHECK(text.find("### Test harness") == std::string::npos);
    // rows in the page's order within a section
    CHECK(text.find("`fast_start`") < text.find("`song_speed`"));
    CHECK(text.find("The defaults are the Windows build's.") != std::string::npos);
}

TEST_CASE("a setting this build doesn't register is left out") {
    CHECK(Row(Reference(), "missing").empty());
}

TEST_CASE("each row gives the name, the label, the default and the values it takes") {
    const std::string text = Reference();
    CHECK(Row(text, "fast_start") ==
          "| `fast_start`<br>Skip the splash screens | `false` | `true`, `false` | What it does |");
    // allowed values, the empty one shown as such, and the lifecycle
    CHECK(Row(text, "lang") ==
          "| `lang`<br>Language | *(empty)* | *(empty)*, `eng`, `fre` | What it does "
          "*Applies at the next start.* |");
    // a dropdown's choices, with their labels
    CHECK(Row(text, "controller_type") ==
          "| `controller_type`<br>Gamepads play as | `7` | `-1` Don't override, `7` Guitar | "
          "What it does |");
    // a typed value with suggestions
    CHECK(Row(text, "frame_cap") ==
          "| `frame_cap`<br>Frame rate cap | `display` | text; `display` The display's, `60` 60 "
          "fps | What it does |");
    // a range, the unit, and the default as typed
    CHECK(Row(text, "song_speed") ==
          "| `song_speed`<br>Song speed | `1` | `0.5` to `2` (x) | What it does |");
    // a generated row is labelled with its name, so no label after it
    CHECK(Row(text, "test_port") ==
          "| `test_port` | `0` | `0` to `65535` | The harness's port |");
}

TEST_CASE("the ini's settings the page doesn't show get a section, and its keys a table") {
    const std::string text = Reference();
    const size_t sdk = text.find("\n## More of the SDK's settings\n");
    const size_t ini = text.find("\n## band3_config.ini\n");
    REQUIRE(sdk != std::string::npos);
    REQUIRE(ini != std::string::npos);
    CHECK(sdk < ini);
    // window_width once, in the SDK's section; fast_start is on the page
    CHECK(text.find("| `window_width` | `1280` |", sdk) < ini);
    CHECK(text.substr(sdk, ini - sdk).find("| `fast_start` |") == std::string::npos);
    CHECK(text.find("| `[window]` | `width` | `window_width` |", ini) != std::string::npos);
}

TEST_CASE("a cell keeps its text whole: |, <...> and * are escaped") {
    const Setting page[] = {{.cvar = "http_address", .tab = Tab::kOnline, .section = "Web",
                             .label = "Address", .widget = Widget::kText}};
    std::vector<RegistryCvar> cvars = {
        Cvar("http_address", ValueType::kString, "a|b", "Open http://<this PC>/ on *.local\nthen")};
    const std::string text =
        SettingsReference({.page = page, .cvars = cvars, .ini_keys = {}, .platform = "Linux"});
    CHECK(Row(text, "http_address") ==
          "| `http_address`<br>Address | `a\\|b` | text | Open http://&lt;this PC&gt;/ on "
          "\\*.local then |");
    CHECK(text.find("The defaults are the Linux build's.") != std::string::npos);
    // no ini keys, no ini sections
    CHECK(text.find("band3_config.ini\n") == std::string::npos);
}

namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // git may check the reference out with \r\n
    std::erase(text, '\r');
    return text;
}

struct Defined {
    std::string name;
    std::string description;
};

// one C string literal at `at` (a '"'), its escapes undone; `at` moves past it
std::string Literal(const std::string& src, size_t& at) {
    std::string out;
    for (at++; at < src.size() && src[at] != '"'; at++) {
        if (src[at] == '\\' && at + 1 < src.size()) {
            at++;
            out += src[at] == 'n' ? '\n' : src[at];
        } else {
            out += src[at];
        }
    }
    at++;
    return out;
}

void SkipSpace(const std::string& src, size_t& at) {
    while (at < src.size() && std::isspace(static_cast<unsigned char>(src[at]))) at++;
}

// each REXCVAR_DEFINE_<type>(name, default, "category", "description" "...")
std::vector<Defined> DefinedSettings(const std::string& src) {
    std::vector<Defined> out;
    constexpr std::string_view kMacro = "REXCVAR_DEFINE_";
    for (size_t at = src.find(kMacro); at != std::string::npos; at = src.find(kMacro, at)) {
        at = src.find('(', at) + 1;
        const size_t comma = src.find(',', at);
        Defined d{.name = src.substr(at, comma - at), .description = {}};
        at = comma + 1;
        SkipSpace(src, at);
        // the default: a literal, or a number or name up to the comma
        if (src[at] == '"') Literal(src, at);
        at = src.find(',', at) + 1;
        SkipSpace(src, at);
        Literal(src, at);  // the category
        at = src.find(',', at) + 1;
        for (SkipSpace(src, at); at < src.size() && src[at] == '"'; SkipSpace(src, at)) {
            d.description += Literal(src, at);
        }
        out.push_back(std::move(d));
    }
    return out;
}

}

TEST_CASE("docs/settings-reference.md has every setting src/settings.cpp defines, as it describes it") {
    const std::string src = ReadFile(std::string(BAND3_ROOT_DIR) + "/src/settings.cpp");
    const std::string reference = ReadFile(std::string(BAND3_ROOT_DIR) + "/docs/settings-reference.md");
    REQUIRE_FALSE(src.empty());
    REQUIRE_FALSE(reference.empty());
    const std::vector<Defined> defined = DefinedSettings(src);
    CHECK(defined.size() > 50);
    for (const Defined& d : defined) {
        INFO("run python tools/settings_reference.py after changing ", d.name);
        CHECK_FALSE(d.description.empty());
        const std::string row = Row(reference, d.name);
        CHECK_MESSAGE(!row.empty(), d.name, " isn't in the reference");
        if (row.empty()) continue;
        CHECK_MESSAGE(row.find(" | " + ReferenceText(d.description)) != std::string::npos,
                      d.name, "'s description in the reference isn't src/settings.cpp's");
    }
}
