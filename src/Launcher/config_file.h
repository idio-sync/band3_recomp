#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// band3.toml as the launcher writes it. Everything happens in SaveConfigFile:
// the file is read as it is then (F4's "Save to config" may have rewritten it
// while the launcher was open), the launcher's settings are set or removed,
// every other key is kept, and the file is written again.
//
// The file is written by hand rather than with toml++'s serializer, one flat
// `name = value` line per key, the form the SDK's LoadConfig is known to read:
// bools and ints bare, floats in plain decimal, strings as basic strings with
// backslashes, quotes and control characters escaped. Comments aren't kept.

namespace band3::launcher {

inline constexpr const char* kConfigHeader =
    "# Written by the band3 launcher. F4 > Save to config rewrites this file.";

using ConfigValue = std::variant<bool, int64_t, double, std::string>;

// sets `key` to `value`, or removes it when there's none
struct ConfigEdit {
    std::string key;
    std::optional<ConfigValue> value;
};

// one key of the file, as written back
struct ConfigLine {
    std::string key;
    // the whole `key = value` line, without the newline
    std::string text;
};

struct ParsedConfig {
    bool ok = true;
    // why it didn't parse, for the launcher's banner
    std::string error;
    // the keys in file order, each keeping its TOML type
    std::vector<ConfigLine> lines;
};

ParsedConfig ParseConfig(std::string_view text);

// a TOML basic string: "..." with \\, \" and control characters escaped
std::string TomlString(std::string_view value);
// the key bare when it can be, quoted otherwise
std::string TomlKey(std::string_view key);
// plain decimal, always with a point: 1.0, 0.15; nan and inf as TOML spells them
std::string FormatFloat(double value);
std::string FormatValue(const ConfigValue& value);

// the file's text after the edits: the header line, then the kept and set
// keys (a set key stays where it was, a new one goes at the end)
std::string MergeConfig(std::span<const ConfigLine> existing, std::span<const ConfigEdit> edits);

// why the file can't be read, for the banner: nullopt when it reads or isn't there
std::optional<std::string> ConfigFileProblem(const std::filesystem::path& file);

struct SaveResult {
    bool ok = false;
    // what went wrong, when !ok
    std::string error;
    // the old file didn't parse: it was copied to <file>.bak and replaced
    bool backed_up = false;
    std::string parse_error;
};

// the old file kept when it doesn't parse: band3.toml.bak
std::filesystem::path BackupPath(const std::filesystem::path& file);

// reads the file as it is now, applies the edits and writes it through a
// temporary file and a rename
SaveResult SaveConfigFile(const std::filesystem::path& file, std::span<const ConfigEdit> edits);

}
