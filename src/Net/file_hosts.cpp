#include "file_hosts.h"

#include <algorithm>
#include <array>
#include "http_request.h"
#include "json.h"

namespace band3::file_hosts {

namespace {

struct Parts {
    std::string host;       // lower-cased, without "www."
    std::string_view path;  // from its '/', without the query
    std::string_view query; // after its '?', without a fragment
};

std::string Lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

// an http(s) URL's parts; nullopt for anything else
std::optional<Parts> Split(std::string_view url) {
    const std::string scheme = Lower(url.substr(0, url.find("://")));
    if (scheme != "http" && scheme != "https") return std::nullopt;
    url.remove_prefix(scheme.size() + 3);
    url = url.substr(0, url.find('#'));
    const size_t end = url.find_first_of("/?");
    Parts parts;
    parts.host = Lower(url.substr(0, end));
    if (parts.host.starts_with("www.")) parts.host.erase(0, 4);
    if (parts.host.empty()) return std::nullopt;
    if (end == std::string_view::npos) return parts;
    std::string_view rest = url.substr(end);
    const size_t question = rest.find('?');
    parts.path = rest.substr(0, question);
    if (question != std::string_view::npos) parts.query = rest.substr(question + 1);
    return parts;
}

bool IsArchive(std::string_view path) {
    std::string lower = Lower(path);
    // MediaFire's newer links end in the name then /file
    if (lower.ends_with("/file")) lower.resize(lower.size() - 5);
    for (const std::string_view ext : {".zip", ".rar", ".7z"}) {
        if (lower.ends_with(ext)) return true;
    }
    return false;
}

std::string Param(std::string_view url, std::string_view name) {
    return http::QueryParam(url.substr(0, url.find('#')), name).value_or("");
}

// letters, digits, - and _, as Drive's and dingorb's IDs are
bool IsId(std::string_view id) {
    return !id.empty() && id.size() <= 128 && std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '-' || c == '_';
    });
}

// a page's text as a browser shows it in an attribute: &amp; and the like
std::string Unescape(std::string_view text) {
    static constexpr std::array<std::pair<std::string_view, char>, 5> kEntities{{
        {"&amp;", '&'}, {"&quot;", '"'}, {"&#39;", '\''}, {"&lt;", '<'}, {"&gt;", '>'}}};
    std::string out;
    for (size_t i = 0; i < text.size(); i++) {
        bool done = false;
        if (text[i] == '&') {
            for (const auto& [entity, c] : kEntities) {
                if (text.substr(i).starts_with(entity)) {
                    out += c;
                    i += entity.size() - 1;
                    done = true;
                    break;
                }
            }
        }
        if (!done) out += text[i];
    }
    return out;
}

// the value of attribute `name` in the tag starting at `at`, or nullopt
std::optional<std::string> Attribute(std::string_view page, size_t at, std::string_view name) {
    const size_t end = page.find('>', at);
    const std::string_view tag = page.substr(at, end == std::string_view::npos ? end : end - at);
    for (size_t pos = 0; (pos = tag.find(name, pos)) != std::string_view::npos; pos += name.size()) {
        const bool starts = pos == 0 || tag[pos - 1] == ' ' || tag[pos - 1] == '\t' || tag[pos - 1] == '\n';
        const size_t eq = pos + name.size();
        if (!starts || eq + 1 >= tag.size() || tag[eq] != '=') continue;
        const char quote = tag[eq + 1];
        if (quote != '"' && quote != '\'') continue;
        const size_t close = tag.find(quote, eq + 2);
        if (close == std::string_view::npos) return std::nullopt;
        return Unescape(tag.substr(eq + 2, close - eq - 2));
    }
    return std::nullopt;
}

std::string Base64Decode(std::string_view text) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;
        if (c == '/' || c == '_') return 63;
        return -1;
    };
    std::string out;
    uint32_t bits = 0;
    int count = 0;
    for (const char c : text) {
        const int v = value(c);
        if (v < 0) continue;
        bits = (bits << 6) | static_cast<uint32_t>(v);
        count += 6;
        if (count >= 8) {
            count -= 8;
            out += static_cast<char>((bits >> count) & 0xFF);
        }
    }
    return out;
}

// a MediaFire download server's link: https://download<n>.mediafire.com/...
bool IsMediaFireDownload(std::string_view url) {
    const auto parts = Split(url);
    if (!parts || !parts->host.starts_with("download") || !parts->host.ends_with(".mediafire.com")) {
        return false;
    }
    const std::string_view digits =
        std::string_view(parts->host).substr(8, parts->host.size() - 8 - std::string_view(".mediafire.com").size());
    return !digits.empty() && std::ranges::all_of(digits, [](char c) { return c >= '0' && c <= '9'; }) &&
           parts->path.size() > 1;
}

}  // namespace

Host HostOf(std::string_view url) {
    const auto parts = Split(url);
    if (!parts || IsArchive(parts->path)) return Host::kNone;
    const std::string& host = parts->host;
    const std::string_view path = parts->path;
    if (host == "rhythmverse.co") return path.size() > 1 ? Host::kRhythmVerse : Host::kNone;
    if (host == "mediafire.com") {
        if (path.starts_with("/file/") || path.starts_with("/download/") || path.starts_with("/file_premium/")) {
            return Host::kMediaFire;
        }
        // the old links: mediafire.com/?<key>
        return (path.empty() || path == "/") && !parts->query.empty() && parts->query.find('=') == std::string_view::npos
                   ? Host::kMediaFire
                   : Host::kNone;
    }
    if (host == "drive.google.com" || host == "docs.google.com" || host == "drive.usercontent.google.com") {
        return DriveFileId(url).empty() ? Host::kNone : Host::kDrive;
    }
    if (host == "dropbox.com" || host == "dl.dropboxusercontent.com" || host == "dl.dropbox.com") {
        return path.starts_with("/s/") || path.starts_with("/scl/fi/") ? Host::kDropbox : Host::kNone;
    }
    if (host == "dingorb.com") return IsId(Param(url, "id")) ? Host::kDingo : Host::kNone;
    if (host == "bit.ly" || host == "tinyurl.com") return path.size() > 1 ? Host::kShortLink : Host::kNone;
    return Host::kNone;
}

std::string DriveFileId(std::string_view url) {
    const auto parts = Split(url);
    if (!parts) return {};
    // /file/d/<id>/view, /uc?id=<id>, /open?id=<id>, /download?id=<id>; never a folder
    if (parts->path.starts_with("/drive/folders/") || parts->path.find("/folders/") != std::string_view::npos) {
        return {};
    }
    constexpr std::string_view kFile = "/file/d/";
    if (const size_t at = parts->path.find(kFile); at != std::string_view::npos) {
        std::string_view id = parts->path.substr(at + kFile.size());
        id = id.substr(0, id.find('/'));
        return IsId(id) ? std::string(id) : std::string();
    }
    const std::string id = Param(url, "id");
    return IsId(id) ? id : std::string();
}

std::string DriveDownloadUrl(std::string_view id) {
    return "https://drive.usercontent.google.com/download?id=" + std::string(id) + "&export=download&confirm=t";
}

std::optional<std::string> DriveFormUrl(std::string_view page) {
    const size_t form = page.find("<form");
    if (form == std::string_view::npos) return std::nullopt;
    const auto action = Attribute(page, form, "action");
    if (!action || !Split(*action) || Split(*action)->host.find("google.com") == std::string::npos) {
        return std::nullopt;
    }
    const size_t end = page.find("</form>", form);
    std::string url = *action;
    bool has_id = false;
    for (size_t at = page.find("<input", form); at != std::string_view::npos && at < end;
         at = page.find("<input", at + 1)) {
        const auto name = Attribute(page, at, "name");
        const auto value = Attribute(page, at, "value");
        if (!name || !value) continue;
        if (*name == "id") has_id = true;
        url += url.find('?') == std::string::npos ? '?' : '&';
        url += *name + "=" + *value;
    }
    if (!has_id) return std::nullopt;
    return url;
}

std::string DropboxDirect(std::string_view url) {
    std::string out(url.substr(0, url.find('#')));
    if (const size_t at = out.find("dl=0"); at != std::string::npos && (out[at - 1] == '?' || out[at - 1] == '&')) {
        out[at + 3] = '1';
        return out;
    }
    if (out.find("dl=1") != std::string::npos) return out;
    return out + (out.find('?') == std::string::npos ? "?dl=1" : "&dl=1");
}

std::optional<std::string> MediaFireLink(std::string_view page) {
    // the button's own link
    for (size_t at = page.find("://download"); at != std::string_view::npos; at = page.find("://download", at + 1)) {
        size_t start = at;
        while (start > 0 && page[start - 1] != '"' && page[start - 1] != '\'' && page[start - 1] != ' ' &&
               page[start - 1] != '=' && at - start < 8) {
            start--;
        }
        const size_t end = page.find_first_of("\"' <>\n", at);
        const std::string url = Unescape(page.substr(start, end - start));
        if (IsMediaFireDownload(url)) return url;
    }
    // newer pages keep it scrambled, as base64, for their script to undo
    constexpr std::string_view kScrambled = "data-scrambled-url=\"";
    if (const size_t at = page.find(kScrambled); at != std::string_view::npos) {
        const size_t start = at + kScrambled.size();
        const size_t end = page.find('"', start);
        const std::string url = Base64Decode(page.substr(start, end - start));
        if (IsMediaFireDownload(url)) return url;
    }
    return std::nullopt;
}

std::optional<std::string> DingoDriveUrl(std::string_view songs_json, std::string_view id) {
    const auto songs = json::Parse(songs_json);
    if (!songs || !songs->IsArray()) return std::nullopt;
    for (const json::Value& song : songs->Items()) {
        if (song["_id"].Text() != id) continue;
        for (const json::Value& url : song["drive_urls"].Items()) {
            if (HostOf(url.Text()) == Host::kDrive) return url.Text();
        }
        return std::nullopt;
    }
    return std::nullopt;
}

bool LooksLikePackage(std::string_view first_bytes) {
    return first_bytes.starts_with("CON ") || first_bytes.starts_with("LIVE") || first_bytes.starts_with("PIRS");
}

}
