#include "config_file.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>
#include <toml++/toml.hpp>

namespace band3::launcher {

namespace {

bool IsBareKey(std::string_view key) {
    if (key.empty()) return false;
    return std::ranges::all_of(key, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

// the key with tables (and tables in arrays) written inline, so it fits on
// one line
void MakeInline(toml::node& node) {
    if (auto* table = node.as_table()) {
        table->is_inline(true);
        for (auto&& [key, child] : *table) MakeInline(child);
    } else if (auto* array = node.as_array()) {
        for (auto& child : *array) MakeInline(child);
    }
}

// a kept key's line, its value keeping its TOML type
std::string LineFor(std::string_view key, const toml::node& node) {
    const std::string name = TomlKey(key);
    if (auto* v = node.as_string()) return name + " = " + TomlString(v->get());
    if (auto* v = node.as_integer()) return name + " = " + std::to_string(v->get());
    if (auto* v = node.as_floating_point()) return name + " = " + FormatFloat(v->get());
    if (auto* v = node.as_boolean()) return name + " = " + (v->get() ? "true" : "false");
    // tables, arrays, dates and times: toml++ writes those, inline
    toml::table root;
    root.insert(std::string(key), node);
    MakeInline(*root.get(key));
    std::ostringstream text;
    text << toml::toml_formatter{root, toml::format_flags::none};
    std::string line = text.str();
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    return line;
}

std::string Describe(const toml::parse_error& error) {
    std::string text(error.description());
    if (error.source().begin.line > 0) {
        text += " (line " + std::to_string(error.source().begin.line) + ")";
    }
    return text;
}

ParsedConfig FromTable(const toml::table& table) {
    struct Keyed {
        toml::source_position at;
        ConfigLine line;
    };
    std::vector<Keyed> keyed;
    for (auto&& [key, node] : table) {
        keyed.push_back({node.source().begin, {std::string(key.str()), LineFor(key.str(), node)}});
    }
    // toml++ keeps a table sorted by key; the file's order reads better
    std::ranges::stable_sort(keyed, [](const Keyed& a, const Keyed& b) {
        return a.at.line != b.at.line ? a.at.line < b.at.line : a.at.column < b.at.column;
    });
    ParsedConfig parsed;
    for (auto& k : keyed) parsed.lines.push_back(std::move(k.line));
    return parsed;
}

// the file's text; nullopt when it isn't there
std::optional<std::string> ReadFile(const std::filesystem::path& file, std::string& error) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "it can't be opened";
        return std::nullopt;
    }
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

}

ParsedConfig ParseConfig(std::string_view text) {
#if TOML_EXCEPTIONS
    try {
        return FromTable(toml::parse(text));
    } catch (const toml::parse_error& error) {
        return {.ok = false, .error = Describe(error), .lines = {}};
    }
#else
    auto result = toml::parse(text);
    if (!result) return {.ok = false, .error = Describe(result.error()), .lines = {}};
    return FromTable(result.table());
#endif
}

std::string TomlString(std::string_view value) {
    std::string out = "\"";
    for (char c : value) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\f': out += "\\f"; break;
        case '\r': out += "\\r"; break;
        default:
            if (u < 0x20 || u == 0x7f) {
                static constexpr char kHex[] = "0123456789ABCDEF";
                out += "\\u00";
                out += kHex[u >> 4];
                out += kHex[u & 0xf];
            } else {
                out += c;
            }
        }
    }
    out += '"';
    return out;
}

std::string TomlKey(std::string_view key) {
    return IsBareKey(key) ? std::string(key) : TomlString(key);
}

std::string FormatFloat(double value) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
    char buffer[512];
    // the shortest digits that read back as the same double
    const auto [end, ec] =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed);
    if (ec != std::errc()) return std::to_string(value);
    std::string text(buffer, end);
    // TOML reads 1 as an integer
    if (text.find('.') == std::string::npos) text += ".0";
    return text;
}

std::string FormatValue(const ConfigValue& value) {
    if (auto* b = std::get_if<bool>(&value)) return *b ? "true" : "false";
    if (auto* i = std::get_if<int64_t>(&value)) return std::to_string(*i);
    if (auto* d = std::get_if<double>(&value)) return FormatFloat(*d);
    return TomlString(std::get<std::string>(value));
}

std::string MergeConfig(std::span<const ConfigLine> existing, std::span<const ConfigEdit> edits) {
    std::vector<ConfigLine> lines(existing.begin(), existing.end());
    for (const auto& edit : edits) {
        auto it = std::ranges::find(lines, edit.key, &ConfigLine::key);
        if (!edit.value) {
            if (it != lines.end()) lines.erase(it);
            continue;
        }
        std::string text = TomlKey(edit.key) + " = " + FormatValue(*edit.value);
        if (it != lines.end()) {
            it->text = std::move(text);
        } else {
            lines.push_back({edit.key, std::move(text)});
        }
    }
    std::string out = kConfigHeader;
    out += '\n';
    for (const auto& line : lines) {
        out += line.text;
        out += '\n';
    }
    return out;
}

bool HasOwnComments(std::string_view text) {
    enum class In { kNothing, kBasic, kLiteral, kMultiBasic, kMultiLiteral };
    In in = In::kNothing;
    auto at = [&](size_t i, std::string_view s) { return text.substr(i, s.size()) == s; };
    for (size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        switch (in) {
        case In::kNothing:
            if (c == '#') {
                // the whole line: a header line on its own doesn't count
                const size_t newline = text.rfind('\n', i);
                const size_t start = newline == std::string_view::npos ? 0 : newline + 1;
                size_t end = text.find('\n', i);
                if (end == std::string_view::npos) end = text.size();
                std::string_view line = text.substr(start, end - start);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' ||
                                         line.back() == '\t')) {
                    line.remove_suffix(1);
                }
                if (line != kConfigHeader && line != kOldConfigHeader &&
                    line != kSdkConfigHeader) {
                    return true;
                }
                i = end;
            } else if (at(i, "\"\"\"")) {
                in = In::kMultiBasic;
                i += 2;
            } else if (at(i, "'''")) {
                in = In::kMultiLiteral;
                i += 2;
            } else if (c == '"') {
                in = In::kBasic;
            } else if (c == '\'') {
                in = In::kLiteral;
            }
            break;
        case In::kBasic:
            if (c == '\\') {
                i++;
            } else if (c == '"' || c == '\n') {
                in = In::kNothing;
            }
            break;
        case In::kLiteral:
            if (c == '\'' || c == '\n') in = In::kNothing;
            break;
        case In::kMultiBasic:
            if (c == '\\') {
                i++;
            } else if (at(i, "\"\"\"")) {
                in = In::kNothing;
                i += 2;
            }
            break;
        case In::kMultiLiteral:
            if (at(i, "'''")) {
                in = In::kNothing;
                i += 2;
            }
            break;
        }
    }
    return false;
}

std::optional<std::string> ConfigFileProblem(const std::filesystem::path& file) {
    std::string error;
    const auto text = ReadFile(file, error);
    if (!text) {
        if (error.empty()) return std::nullopt;
        return error;
    }
    ParsedConfig parsed = ParseConfig(*text);
    if (parsed.ok) return std::nullopt;
    return parsed.error;
}

std::filesystem::path BackupPath(const std::filesystem::path& file) {
    std::filesystem::path backup = file;
    backup += ".bak";
    return backup;
}

SaveResult SaveConfigFile(const std::filesystem::path& file, std::span<const ConfigEdit> edits) {
    SaveResult result;
    std::error_code ec;
    std::string read_error;
    const auto text = ReadFile(file, read_error);
    if (!read_error.empty()) {
        result.error = "band3.toml " + read_error;
        return result;
    }

    ParsedConfig parsed;
    if (text) {
        parsed = ParseConfig(*text);
        if (!parsed.ok) {
            // keep the file that couldn't be read, and start again from nothing
            std::filesystem::copy_file(file, BackupPath(file),
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                result.error = "the old band3.toml couldn't be kept as band3.toml.bak: " +
                               ec.message();
                return result;
            }
            result.backed_up = true;
            result.parse_error = parsed.error;
            parsed = {};
        } else if (HasOwnComments(*text)) {
            // the rewrite drops comments; keep the file the player wrote them in
            std::filesystem::copy_file(file, BackupPath(file),
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                result.error = "the old band3.toml's comments couldn't be kept in "
                               "band3.toml.bak: " + ec.message();
                return result;
            }
            result.backed_up = true;
            result.had_comments = true;
        }
    }

    const std::string merged = MergeConfig(parsed.lines, edits);
    std::filesystem::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(merged.data(), static_cast<std::streamsize>(merged.size()));
        out.close();
        if (!out) {
            std::filesystem::remove(temp, ec);
            result.error = "band3.toml couldn't be written";
            return result;
        }
    }
    std::filesystem::rename(temp, file, ec);
    if (ec) {
        result.error = "band3.toml couldn't be replaced: " + ec.message();
        std::filesystem::remove(temp, ec);
        return result;
    }
    result.ok = true;
    return result;
}

}
