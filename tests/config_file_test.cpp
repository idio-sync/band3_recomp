// Checks band3.toml's reader and writer (src/Launcher/config_file.h): escaping,
// typed values, merging, and a file that doesn't parse.

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <toml++/toml.hpp>
#include "src/Launcher/config_file.h"

namespace fs = std::filesystem;
using namespace band3::launcher;

namespace {

toml::table Parse(const std::string& text) {
    return toml::parse(text);
}

std::string ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

void WriteAll(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

// the line for `key`, without the newline
std::string LineOf(const std::string& text, std::string_view key) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(std::string(key) + " = ", 0) == 0) return line;
    }
    return {};
}

}

TEST_CASE("strings are basic strings with backslashes, quotes and control characters escaped") {
    CHECK(TomlString("plain") == "\"plain\"");
    CHECK(TomlString("C:\\Games\\RB3") == "\"C:\\\\Games\\\\RB3\"");
    CHECK(TomlString("say \"hi\"") == "\"say \\\"hi\\\"\"");
    CHECK(TomlString("a\tb\nc\rd") == "\"a\\tb\\nc\\rd\"");
    CHECK(TomlString(std::string("x\x01y\x7f", 4)) == "\"x\\u0001y\\u007F\"");
    // UTF-8 passes through
    CHECK(TomlString("Beyonc\xc3\xa9") == "\"Beyonc\xc3\xa9\"");

    // and toml++ reads each back as it was
    for (const std::string& value :
         {std::string("C:\\Games\\RB3 \"deluxe\""), std::string("tab\there\nnewline"),
          std::string("\\\\server\\share\\"), std::string("ctl\x02\x1f end"), std::string()}) {
        INFO(value);
        const auto table = Parse("k = " + TomlString(value));
        CHECK(table["k"].value<std::string>() == value);
    }
}

TEST_CASE("keys are bare when they can be") {
    CHECK(TomlKey("audio_maxqframes") == "audio_maxqframes");
    CHECK(TomlKey("a-b_9") == "a-b_9");
    CHECK(TomlKey("UI/Window") == "\"UI/Window\"");
    CHECK(TomlKey("") == "\"\"");
}

TEST_CASE("floats are plain decimals that read back as floats") {
    CHECK(FormatFloat(1.0) == "1.0");
    CHECK(FormatFloat(0.15) == "0.15");
    CHECK(FormatFloat(-2.5) == "-2.5");
    CHECK(FormatFloat(144.0) == "144.0");
    CHECK(FormatFloat(1e-7) == "0.0000001");
    CHECK(FormatFloat(std::numeric_limits<double>::infinity()) == "inf");
    CHECK(FormatFloat(-std::numeric_limits<double>::infinity()) == "-inf");
    CHECK(FormatFloat(std::numeric_limits<double>::quiet_NaN()) == "nan");
    for (double v : {1.0, 0.15, 1.25, 0.05, 123456.789, -0.5}) {
        const auto table = Parse("k = " + FormatFloat(v));
        CHECK(table["k"].is_floating_point());
        CHECK(table["k"].value<double>() == v);
    }
}

TEST_CASE("values are written by type") {
    CHECK(FormatValue(true) == "true");
    CHECK(FormatValue(false) == "false");
    CHECK(FormatValue(int64_t{-1}) == "-1");
    CHECK(FormatValue(0.75) == "0.75");
    CHECK(FormatValue(std::string("fxaa")) == "\"fxaa\"");
}

TEST_CASE("the merge keeps other keys, sets in place, appends and removes") {
    const ParsedConfig parsed = ParseConfig(
        "# a comment that goes\n"
        "window_width = 1600\n"
        "lang = \"fre\"\n"
        "log_level = 'debug'\n"
        "rnd_sync = 1\n");
    REQUIRE(parsed.ok);
    REQUIRE(parsed.lines.size() == 4);
    // file order, not toml++'s sorted order
    CHECK(parsed.lines[0].key == "window_width");
    CHECK(parsed.lines[3].key == "rnd_sync");

    const ConfigEdit edits[] = {
        {"lang", std::string("deu")},
        {"rnd_sync", std::nullopt},
        {"song_speed", 1.5},
        {"not_there", std::nullopt},
        {"midi_drums", true},
    };
    const std::string text = MergeConfig(parsed.lines, edits);
    CHECK(text.rfind(std::string(kConfigHeader) + "\n", 0) == 0);
    CHECK(text ==
          std::string(kConfigHeader) +
              "\n"
              "window_width = 1600\n"
              "lang = \"deu\"\n"
              // a literal string kept as a string, written as a basic one
              "log_level = \"debug\"\n"
              "song_speed = 1.5\n"
              "midi_drums = true\n");
}

TEST_CASE("kept keys keep their TOML type") {
    const std::string original =
        "s = \"C:\\\\x \\\"q\\\"\"\n"
        "i = 42\n"
        "neg = -7\n"
        "f = 0.25\n"
        "whole = 3.0\n"
        "b = false\n"
        "arr = [1, 2, 3]\n"
        "strs = [\"a\\\\b\", \"c\"]\n"
        "inline = { x = 1, y = \"z\" }\n"
        "date = 1979-05-27\n"
        "dotted.key = true\n"
        "[GPU]\n"
        "vsync = false\n"
        "[[list]]\n"
        "n = 1\n"
        "[[list]]\n"
        "n = 2\n";
    const ParsedConfig parsed = ParseConfig(original);
    REQUIRE(parsed.ok);
    const std::string text = MergeConfig(parsed.lines, {});
    INFO(text);

    const toml::table before = Parse(original);
    const toml::table after = Parse(text);
    CHECK(after == before);
    CHECK(after["i"].is_integer());
    CHECK(after["whole"].is_floating_point());
    CHECK(after["b"].is_boolean());
    CHECK(after["s"].value<std::string>() == "C:\\x \"q\"");
    // one line per key
    CHECK(LineOf(text, "GPU") == "GPU = { vsync = false }");
    CHECK_FALSE(LineOf(text, "list").empty());
}

TEST_CASE("a file that doesn't parse says why") {
    const ParsedConfig parsed = ParseConfig("window_width = 1600\nthis is not toml\n");
    CHECK_FALSE(parsed.ok);
    CHECK_FALSE(parsed.error.empty());
    CHECK(parsed.error.find("line 2") != std::string::npos);
    // the SDK's F4 save used to write unescaped Windows paths
    CHECK_FALSE(ParseConfig("user_data_root = \"C:\\Users\\me\\rb3\"\n").ok);
}

TEST_CASE("saving writes through a temporary file") {
    const fs::path dir = fs::temp_directory_path() / "band3_config_file_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path file = dir / "band3.toml";

    SUBCASE("a missing file is created") {
        CHECK_FALSE(ConfigFileProblem(file));
        const ConfigEdit edits[] = {{"lang", std::string("eng")}, {"rnd_sync", std::nullopt}};
        const SaveResult result = SaveConfigFile(file, edits);
        CHECK(result.ok);
        CHECK_FALSE(result.backed_up);
        CHECK(ReadAll(file) == std::string(kConfigHeader) + "\nlang = \"eng\"\n");
        CHECK_FALSE(fs::exists(dir / "band3.toml.tmp"));
    }
    SUBCASE("an existing file is merged") {
        WriteAll(file, "window_width = 1600\nlang = \"fre\"\n");
        CHECK_FALSE(ConfigFileProblem(file));
        const ConfigEdit edits[] = {{"lang", std::nullopt}, {"vsync", false}};
        REQUIRE(SaveConfigFile(file, edits).ok);
        CHECK(ReadAll(file) ==
              std::string(kConfigHeader) + "\nwindow_width = 1600\nvsync = false\n");
    }
    SUBCASE("a file that doesn't parse is kept as band3.toml.bak and replaced") {
        const std::string broken = "window_width = 1600\nuser_data_root = \"C:\\Users\\me\"\n";
        WriteAll(file, broken);
        const auto problem = ConfigFileProblem(file);
        REQUIRE(problem);
        CHECK_FALSE(problem->empty());

        const ConfigEdit edits[] = {{"lang", std::string("ita")}};
        const SaveResult result = SaveConfigFile(file, edits);
        CHECK(result.ok);
        CHECK(result.backed_up);
        CHECK(result.parse_error == *problem);
        CHECK(BackupPath(file) == dir / "band3.toml.bak");
        CHECK(ReadAll(BackupPath(file)) == broken);
        CHECK(ReadAll(file) == std::string(kConfigHeader) + "\nlang = \"ita\"\n");
        CHECK_FALSE(ConfigFileProblem(file));
    }
    fs::remove_all(dir);
}

TEST_CASE("everything the writer writes reads back with its type") {
    const ConfigEdit edits[] = {
        {"game_data_root", std::string("D:/Games/RB3")},
        {"user_data_root", std::string("user data")},
        {"content_folders", std::string("songs|D:/More Songs")},
        {"username", std::string("The \"Band\" \\o/")},
        {"fullscreen", false},
        {"monitor", int64_t{2}},
        {"anisotropic_override", int64_t{-1}},
        {"left_stick_deadzone_percentage", 0.2},
        {"video_mode_refresh_rate", 144.0},
    };
    const toml::table table = Parse(MergeConfig({}, edits));
    CHECK(table["game_data_root"].value<std::string>() == "D:/Games/RB3");
    CHECK(table["user_data_root"].value<std::string>() == "user data");
    CHECK(table["content_folders"].value<std::string>() == "songs|D:/More Songs");
    CHECK(table["username"].value<std::string>() == "The \"Band\" \\o/");
    CHECK(table["fullscreen"].value<bool>() == false);
    CHECK(table["monitor"].value<int64_t>() == 2);
    CHECK(table["anisotropic_override"].value<int64_t>() == -1);
    CHECK(table["left_stick_deadzone_percentage"].value<double>() == 0.2);
    CHECK(table["video_mode_refresh_rate"].is_floating_point());
}
