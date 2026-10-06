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
// backslashes, quotes and control characters escaped. Comments aren't kept,
// so a file with comments of the player's is copied to band3.toml.bak first.

namespace band3::launcher {

inline constexpr const char* kConfigHeader =
    "# Written by band3's settings (the launcher, F4). All settings > Save to config rewrites "
    "this file.";
// the header earlier builds wrote, which isn't a comment of the player's either
inline constexpr const char* kOldConfigHeader =
    "# Written by the band3 launcher. F4 > Save to config rewrites this file.";

// the first line of a file F4's "Save to config" wrote (the SDK's SaveConfig)
inline constexpr const char* kSdkConfigHeader = "# Auto-generated cvar configuration";

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

// whether the text has a comment the launcher's rewrite would drop: a `#`
// line, or a `#` after a value, other than the launcher's or F4's header line
// (a `#` inside a string isn't one)
bool HasOwnComments(std::string_view text);

// why the file can't be read, for the banner: nullopt when it reads or isn't there
std::optional<std::string> ConfigFileProblem(const std::filesystem::path& file);

struct SaveResult {
    bool ok = false;
    // what went wrong, when !ok
    std::string error;
    // the old file was copied to <file>.bak before it was replaced: it didn't
    // parse (parse_error says why), or it had comments (HasOwnComments), which
    // the rewrite drops
    bool backed_up = false;
    std::string parse_error;
    bool had_comments = false;
};

// the old file kept when it doesn't parse or has comments: band3.toml.bak
std::filesystem::path BackupPath(const std::filesystem::path& file);

// reads the file as it is now, applies the edits and writes it through a
// temporary file and a rename. A file that doesn't parse, or has comments, is
// copied to BackupPath first, over any older copy: the file being replaced is
// the one worth keeping, and once rewritten it has no comments to back up again.
SaveResult SaveConfigFile(const std::filesystem::path& file, std::span<const ConfigEdit> edits);

}
