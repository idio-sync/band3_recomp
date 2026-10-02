#include "http_request.h"

#include <charconv>

namespace band3::http {

namespace {

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string_view StatusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 503: return "Service Unavailable";
        default: return "Error";
    }
}

// a value on a line of its own: line breaks in it would start another key
void AppendValue(std::string& out, std::string_view key, std::string_view value) {
    out += key;
    out += '=';
    for (char c : ToUtf8(value)) out += (c == '\r' || c == '\n') ? ' ' : c;
    out += "\r\n";
}

// the length of the UTF-8 sequence at `text`, or 0 when it isn't one
size_t Utf8Length(std::string_view text) {
    const auto lead = static_cast<uint8_t>(text[0]);
    size_t length;
    if (lead < 0x80) return 1;
    if (lead >= 0xC2 && lead <= 0xDF) length = 2;
    else if (lead >= 0xE0 && lead <= 0xEF) length = 3;
    else if (lead >= 0xF0 && lead <= 0xF4) length = 4;
    else return 0;
    if (text.size() < length) return 0;
    for (size_t i = 1; i < length; i++) {
        if ((static_cast<uint8_t>(text[i]) & 0xC0) != 0x80) return 0;
    }
    return length;
}

bool IsUtf8(std::string_view text) {
    while (!text.empty()) {
        const size_t length = Utf8Length(text);
        if (!length) return false;
        text.remove_prefix(length);
    }
    return true;
}

}

std::optional<Request> ParseRequest(std::string_view head) {
    const size_t line_end = head.find("\r\n");
    const std::string_view line = head.substr(0, line_end);
    const size_t first = line.find(' ');
    if (first == std::string_view::npos || first == 0) return std::nullopt;
    const size_t second = line.find(' ', first + 1);
    if (second == std::string_view::npos || second == first + 1) return std::nullopt;
    if (!line.substr(second + 1).starts_with("HTTP/")) return std::nullopt;
    return Request{std::string(line.substr(0, first)),
                   std::string(line.substr(first + 1, second - first - 1))};
}

std::string UrlDecode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int high = HexValue(text[i + 1]);
            const int low = HexValue(text[i + 2]);
            if (high >= 0 && low >= 0) {
                out += static_cast<char>(high * 16 + low);
                i += 2;
                continue;
            }
        }
        out += text[i];
    }
    return out;
}

Route MatchRoute(std::string_view target) {
    const std::string path = UrlDecode(target);
    Route route;
    if (path == "/") {
        route.endpoint = Endpoint::kIndex;
    } else if (path == "/list_songs") {
        route.endpoint = Endpoint::kListSongs;
    } else if (path == "/jsonrpc") {
        route.endpoint = Endpoint::kJsonRpc;
    } else if (path == "/status") {
        route.endpoint = Endpoint::kStatus;
    } else if (path == "/song_details") {
        route.endpoint = Endpoint::kSongDetails;
    } else if (path.starts_with("/song_")) {
        const char* begin = path.data() + 6;
        const char* end = path.data() + path.size();
        int32_t id = 0;
        const auto [ptr, ec] = std::from_chars(begin, end, id);
        if (ec == std::errc() && ptr == end) {
            route.endpoint = Endpoint::kSong;
            route.song_id = id;
        }
    } else if (path.starts_with("/jump?shortname=")) {
        route.endpoint = Endpoint::kJump;
        route.argument = path.substr(16);
    } else if (path.starts_with("/execute?script=")) {
        route.endpoint = Endpoint::kExecute;
        route.argument = path.substr(16);
    } else if (path.starts_with("/album_art?shortname=")) {
        route.endpoint = Endpoint::kAlbumArt;
        route.argument = path.substr(21);
    }
    return route;
}

std::string FormatSong(const SongInfo& song, bool section) {
    std::string out;
    if (section) out += "[" + ToUtf8(song.shortname) + "]\r\n";
    AppendValue(out, "shortname", song.shortname);
    AppendValue(out, "title", song.title);
    AppendValue(out, "artist", song.artist);
    AppendValue(out, "album", song.album);
    AppendValue(out, "origin", song.origin);
    out += "\r\n";
    return out;
}

std::string ToUtf8(std::string_view text) {
    if (IsUtf8(text)) return std::string(text);
    std::string out;
    out.reserve(text.size() * 2);
    for (char c : text) {
        const auto byte = static_cast<uint8_t>(c);
        if (byte < 0x80) {
            out += c;
        } else {
            out += static_cast<char>(0xC0 | (byte >> 6));
            out += static_cast<char>(0x80 | (byte & 0x3F));
        }
    }
    return out;
}

std::string JsonString(std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out = "\"";
    for (char c : ToUtf8(text)) {
        const auto byte = static_cast<uint8_t>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (byte < 0x20) {
            out += "\\u00";
            out += kHex[byte >> 4];
            out += kHex[byte & 15];
        } else {
            out += c;
        }
    }
    return out + "\"";
}

std::string FormatStatus(const Status& status) {
    std::string out = "{\"screen\":" + JsonString(status.screen) +
                      ",\"in_library\":" + (status.in_library ? "true" : "false") +
                      ",\"playing\":";
    if (!status.playing) return out + "null}";
    const Status::Playing& p = *status.playing;
    out += "{\"shortname\":" + JsonString(p.shortname) + ",\"title\":" + JsonString(p.title) +
           ",\"artist\":" + JsonString(p.artist) + ",\"score\":" + std::to_string(p.score) +
           ",\"position_ms\":" + (p.position_ms < 0 ? "null" : std::to_string(p.position_ms)) +
           ",\"length_ms\":" + std::to_string(p.length_ms) + "}}";
    return out;
}

std::string FormatSongDetails(const std::vector<SongDetails>& songs) {
    std::string out = "{";
    for (const SongDetails& s : songs) {
        if (out.size() > 1) out += ',';
        out += JsonString(s.shortname) + ":{\"genre\":" + JsonString(s.genre) +
               ",\"year\":" + std::to_string(s.year) +
               ",\"length_ms\":" + std::to_string(s.length_ms) +
               ",\"vocal_parts\":" + std::to_string(s.vocal_parts) + ",\"tiers\":{";
        for (size_t i = 0; i < s.tiers.size(); i++) {
            if (i) out += ',';
            out += JsonString(s.tiers[i].first) + ":" + std::to_string(s.tiers[i].second);
        }
        out += "}}";
    }
    return out + "}";
}

std::string Response(int status, std::string_view content_type, std::string_view body,
                     bool cors, int max_age) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " +
                      std::string(StatusText(status)) + "\r\n";
    out += "Server: band3\r\n";
    out += "Content-Type: ";
    out += content_type;
    out += "\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    out += max_age > 0 ? "Cache-Control: max-age=" + std::to_string(max_age) + "\r\n"
                       : std::string("Cache-Control: no-store\r\n");
    if (cors) out += "Access-Control-Allow-Origin: *\r\n";
    out += "Connection: close\r\n\r\n";
    out += body;
    return out;
}

}
