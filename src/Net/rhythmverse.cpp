#include "rhythmverse.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include "src/Game/song_id.h"
#include "http_request.h"
#include "json.h"

namespace band3::rhythmverse {

namespace {

using http::JsonString;

// RhythmVerse's difficulty fields, by the game's names for the parts. Its
// values are the game's tiers plus one: 1 Warmup to 7 Impossible; 0 and -1
// are a part the song doesn't have.
constexpr std::pair<std::string_view, std::string_view> kParts[] = {
    {"diff_band", "band"},           {"diff_guitar", "guitar"},
    {"diff_bass", "bass"},           {"diff_drums", "drum"},
    {"diff_vocals", "vocals"},       {"diff_keys", "keys"},
    {"diff_proguitar", "real_guitar"}, {"diff_probass", "real_bass"},
    {"diff_prokeys", "real_keys"},
};

std::string Absolute(std::string_view url) {
    if (url.starts_with("/")) return std::string(kSite) + std::string(url);
    return std::string(url);
}

// "https://www.mediafire.com/x" -> "www.mediafire.com"; "" for a relative URL
std::string HostOf(std::string_view url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string_view::npos) return {};
    url.remove_prefix(scheme + 3);
    return std::string(url.substr(0, url.find_first_of("/?#")));
}

// the upload's own field, or failing that the song's
const json::Value& Field(const json::Value& file, const json::Value& data, std::string_view file_key,
                         std::string_view data_key) {
    const json::Value& own = file[file_key];
    return own.IsNull() || own.Text().empty() ? data[data_key] : own;
}

int64_t Whole(const json::Value& value) {
    const double d = value.Number(0);
    return std::isfinite(d) ? static_cast<int64_t>(d) : 0;
}

std::optional<Song> ParseSong(const json::Value& entry) {
    const json::Value& data = entry["data"];
    const json::Value& file = entry["file"];
    Song song;
    song.file_id = file["file_id"].Text();
    if (!ValidFileId(song.file_id)) return std::nullopt;
    song.title = Field(file, data, "file_title", "title").Text();
    song.artist = Field(file, data, "file_artist", "artist").Text();
    song.album = Field(file, data, "file_album", "album").Text();
    song.genre = Field(file, data, "file_genre", "genre").Text();
    song.year = static_cast<int32_t>(Whole(Field(file, data, "file_year", "year")));
    song.length_s = static_cast<int32_t>(Whole(Field(file, data, "song_length", "song_length")));
    song.vocal_parts =
        static_cast<int32_t>(Whole(Field(file, data, "vocal_parts_authored", "vocal_parts")));
    song.size = Whole(file["size"]);
    song.downloads = Whole(file["downloads"]);
    song.author = file["author"]["name"].Text();
    if (song.author.empty()) song.author = file["user"].Text();

    for (const auto& [key, part] : kParts) {
        const int64_t value = Whole(Field(file, data, key, key));
        if (value >= 1) song.tiers.emplace_back(part, static_cast<int32_t>(std::min<int64_t>(value - 1, 6)));
    }

    const std::string art = Field(file, data, "album_art", "album_art").Text();
    if (!art.empty()) song.art_url = Absolute(art);
    song.page_url = Absolute(file["file_url"].Text());
    song.file_name = file["file_name"].Text();
    song.song_id = SongIdOf(file["custom_id"].Text());
    const std::string hashes = file["gamefile_hash"].Text() + file["gameplay_file_hash"].Text() +
                               file["configuration_file_hash"].Text();
    if (!hashes.empty()) {
        song.hash = file["gamefile_hash"].Text() + "/" + file["gameplay_file_hash"].Text() + "/" +
                    file["configuration_file_hash"].Text();
    }

    const std::string external = file["external_url"].Text();
    const std::string download = file["download_url"].Text();
    const std::string url = !external.empty() ? external : download;
    const std::string host = HostOf(url);
    const bool hosted = !url.empty() && (host.empty() || host == "rhythmverse.co");
    // a bool, a number, or a string of one as PHP sends them
    const json::Value& zip = file["zippata"];
    const bool zipped = zip.Bool(false) || zip.Number(0) != 0;
    if (hosted && !zipped) {
        song.download_url = Absolute(url);
    } else {
        song.host = hosted ? "rhythmverse.co" : host;
    }
    return song;
}

// "in_library" as FormatSearch and FormatDownloads give it
std::string InLibrary(int32_t song_id, const std::optional<std::set<int32_t>>& game_ids) {
    if (!game_ids) return "null";
    return song_id && game_ids->contains(song_id) ? "true" : "false";
}

// the page's sorts, and RhythmVerse's field|order for each
constexpr std::pair<std::string_view, std::string_view> kSorts[] = {
    {"newest", "release_date|DESC"}, {"updated", "update_date|DESC"},
    {"downloads", "downloads|DESC"}, {"title", "title|ASC"},
    {"artist", "artist|ASC"},        {"length", "length|ASC"},
};

// RhythmVerse's name for one of the game's parts
std::optional<std::string_view> RvInstrument(std::string_view part) {
    static constexpr std::pair<std::string_view, std::string_view> kInstruments[] = {
        {"guitar", "guitar"},     {"bass", "bass"},           {"drum", "drums"},
        {"vocals", "vocals"},     {"keys", "keys"},           {"real_guitar", "proguitar"},
        {"real_bass", "probass"}, {"real_keys", "prokeys"},
    };
    for (const auto& [game, rv] : kInstruments) {
        if (game == part) return rv;
    }
    return std::nullopt;
}

// "a,,b" -> {"a", "b"}
std::vector<std::string> SplitCommas(std::string_view text) {
    std::vector<std::string> out;
    while (!text.empty()) {
        const size_t comma = text.find(',');
        if (comma) out.emplace_back(text.substr(0, comma));
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    return out;
}

// all of text as a whole number
std::optional<int32_t> WholeOf(std::string_view text) {
    int32_t n = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
    if (text.empty() || ec != std::errc() || ptr != text.data() + text.size()) return std::nullopt;
    return n;
}

void AppendField(std::string& out, std::string_view key, std::string_view json) {
    if (out.back() != '{') out += ',';
    out += '"';
    out += key;
    out += "\":";
    out += json;
}

}

std::string FormEncode(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<uint8_t>(c);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~') {
            out += c;
        } else {
            out += '%';
            out += kHex[byte >> 4];
            out += kHex[byte & 15];
        }
    }
    return out;
}

SearchOptions ParseSearchOptions(std::string_view target) {
    SearchOptions options;
    auto param = [&](std::string_view name) {
        return http::QueryParam(target, name).value_or("");
    };
    options.text = param("text");
    options.page = std::max<int32_t>(WholeOf(param("page")).value_or(1), 1);
    const std::string sort = param("sort");
    if (std::ranges::any_of(kSorts, [&](const auto& s) { return s.first == sort; })) {
        options.sort = sort;
    }
    options.downloadable_only = param("downloadable") == "1";
    options.harmonies = param("harmonies") == "1";
    for (const std::string& part : SplitCommas(param("has"))) {
        if (RvInstrument(part)) options.has.push_back(part);
    }
    for (const std::string& genre : SplitCommas(param("genre"))) {
        const bool slug = genre.size() <= 32 && std::ranges::all_of(genre, [](char c) {
            return (c >= 'a' && c <= 'z') || c == '_';
        });
        if (slug) options.genres.push_back(genre);
    }
    for (const std::string& decade : SplitCommas(param("decade"))) {
        const auto year = WholeOf(decade);
        if (year && *year >= 1900 && *year <= 2100 && *year % 10 == 0) {
            options.decades.push_back(*year);
        }
    }
    const std::string cap = param("cap");
    const size_t colon = cap.find(':');
    if (colon != std::string::npos && RvInstrument(cap.substr(0, colon))) {
        const auto tier = WholeOf(std::string_view(cap).substr(colon + 1));
        if (tier && *tier >= 0 && *tier <= 6) {
            options.cap_part = cap.substr(0, colon);
            options.cap_tier = *tier;
        }
    }
    return options;
}

SearchRequest Search(const SearchOptions& options) {
    SearchRequest request;
    request.page_size = options.downloadable_only ? kDownloadablePageSize : kPageSize;
    request.form = "records=" + std::to_string(request.page_size) +
                   "&page=" + std::to_string(std::max(options.page, 1)) + "&data_type=full";
    auto add = [&](std::string_view key, std::string_view value) {
        request.form += "&" + FormEncode(key) + "=" + FormEncode(value);
    };
    if (options.text.empty()) {
        request.url = std::string(kSite) + "/api/rb3xbox/songfiles/list";
    } else {
        request.url = std::string(kSite) + "/api/rb3xbox/songfiles/search/live";
        add("text", options.text);
    }
    // a search keeps its own order unless asked; the list, the newest first
    std::string_view sort = options.sort;
    if (sort.empty() && options.text.empty()) sort = "newest";
    for (const auto& [name, field] : kSorts) {
        if (name != sort) continue;
        const size_t bar = field.find('|');
        add("sort[0][sort_by]", field.substr(0, bar));
        add("sort[0][sort_order]", field.substr(bar + 1));
    }
    for (const std::string& part : options.has) {
        if (const auto instrument = RvInstrument(part)) add("instrument[]", *instrument);
    }
    if (options.harmonies) {
        add("vocal_parts[]", "2");
        add("vocal_parts[]", "3");
    }
    for (const std::string& genre : options.genres) add("genre[]", genre);
    for (const int32_t decade : options.decades) add("decade[]", std::to_string(decade));
    if (const auto instrument = RvInstrument(options.cap_part);
        instrument && options.cap_tier >= 0 && options.cap_tier <= 6) {
        add("tierinstrument[]", *instrument);
        // RhythmVerse's tiers are the game's plus one, and match exactly
        for (int32_t tier = 0; tier <= options.cap_tier; tier++) {
            add("tier[]", std::to_string(tier + 1));
        }
    }
    return request;
}

std::optional<SearchResult> ParseSearch(std::string_view text) {
    const auto reply = json::Parse(text);
    if (!reply || (*reply)["status"].Text() != "success") return std::nullopt;
    const json::Value& data = (*reply)["data"];
    // a search that found nothing says "songs": false
    if (!data["songs"].IsArray() && data["songs"].Bool(true)) return std::nullopt;
    SearchResult result;
    result.total = Whole(data["records"]["total_filtered"]);
    result.page = static_cast<int32_t>(std::max<int64_t>(Whole(data["pagination"]["page"]), 1));
    for (const json::Value& entry : data["songs"].Items()) {
        if (auto song = ParseSong(entry)) result.songs.push_back(std::move(*song));
    }
    return result;
}

int32_t SongIdOf(std::string_view custom_id) {
    if (custom_id.empty() || custom_id == "0") return 0;
    int64_t id = 0;
    const auto [ptr, ec] = std::from_chars(custom_id.data(), custom_id.data() + custom_id.size(), id);
    if (ptr != custom_id.data() + custom_id.size() || ec != std::errc()) {
        // text, as some songs.dta have it; a number with more after it is too
        return CorrectedSongId(custom_id);
    }
    return id > 0 && id <= INT32_MAX ? static_cast<int32_t>(id) : 0;
}

std::string LowerAscii(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool IsDownloaded(const Song& song, const LocalSongs& local) {
    // band3's own download, whatever its size: it was checked as it came
    const auto has_file = [&](const std::string& name) {
        const std::string lower = LowerAscii(name);
        const auto it = local.files.lower_bound({lower, INT64_MIN});
        return it != local.files.end() && it->first == lower;
    };
    if (has_file(DownloadFileName(song))) return true;
    const auto record = local.records.find(song.file_id);
    if (record != local.records.end() && has_file(record->second.file_name)) return true;
    // RhythmVerse's download, as its page saves it; the size tells it from
    // another upload by the same name (and from an older version of this one)
    return !song.file_name.empty() && song.size > 0 &&
           local.files.contains({LowerAscii(song.file_name), song.size});
}

DownloadRecords ParseRecords(std::string_view text) {
    DownloadRecords records;
    const auto json = json::Parse(text);
    if (!json || !json->IsObject()) return records;
    for (const auto& [file_id, value] : json->Members()) {
        DownloadRecord record{value["file"].Text(), value["hash"].Text(), value["pending"].Text()};
        // a file in the download folder, not a path that leaves it
        const std::string& name = record.file_name;
        const bool bare = !name.empty() && name != "." && name != ".." &&
                          name.find_first_of("/\\:") == std::string::npos;
        if (ValidFileId(file_id) && bare) records.emplace(file_id, std::move(record));
    }
    return records;
}

std::string FormatRecords(const DownloadRecords& records) {
    std::string out = "{";
    for (const auto& [file_id, record] : records) {
        if (out.size() > 1) out += ",";
        out += "\n  " + JsonString(file_id) + ": {\"file\": " + JsonString(record.file_name) +
               ", \"hash\": " + JsonString(record.hash) +
               ", \"pending\": " + JsonString(record.pending_hash) + "}";
    }
    return out + "\n}\n";
}

UpdateState UpdateOf(const Song& song, const DownloadRecords& records) {
    const auto it = records.find(song.file_id);
    if (it == records.end() || song.hash.empty() || song.download_url.empty()) return UpdateState::kNone;
    const DownloadRecord& record = it->second;
    if (!record.pending_hash.empty()) {
        return record.pending_hash == song.hash ? UpdateState::kPending : UpdateState::kAvailable;
    }
    // a record from before band3 kept hashes says nothing
    if (record.hash.empty() || record.hash == song.hash) return UpdateState::kNone;
    return UpdateState::kAvailable;
}

bool ValidFileId(std::string_view file_id) {
    if (file_id.empty() || file_id.size() > 64) return false;
    return std::all_of(file_id.begin(), file_id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '.';
    });
}

std::string DownloadFileName(const Song& song) {
    // long enough to tell uploads apart, short enough for Windows' paths
    constexpr size_t kMaxName = 60;
    std::string name;
    for (const char c : song.file_name.substr(0, kMaxName)) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_' ||
                          c == '.' || c == '(' || c == ')';
        name += keep ? c : '_';
    }
    // Windows drops a trailing dot or space from a name
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.empty()) name = "song";
    return name + "_" + song.file_id;
}

std::string FormatSearch(const SearchResult& result, const LocalSongs& local) {
    std::string out = "{\"total\":" + std::to_string(result.total) +
                      ",\"page\":" + std::to_string(result.page) +
                      ",\"page_size\":" + std::to_string(result.page_size) + ",\"songs\":[";
    for (size_t i = 0; i < result.songs.size(); i++) {
        const Song& s = result.songs[i];
        if (i) out += ',';
        out += '{';
        AppendField(out, "file_id", JsonString(s.file_id));
        AppendField(out, "title", JsonString(s.title));
        AppendField(out, "artist", JsonString(s.artist));
        AppendField(out, "album", JsonString(s.album));
        AppendField(out, "genre", JsonString(s.genre));
        AppendField(out, "author", JsonString(s.author));
        AppendField(out, "year", std::to_string(s.year));
        AppendField(out, "length_ms", std::to_string(int64_t{s.length_s} * 1000));
        AppendField(out, "vocal_parts", std::to_string(s.vocal_parts));
        AppendField(out, "size", std::to_string(s.size));
        AppendField(out, "downloads", std::to_string(s.downloads));
        std::string tiers = "{";
        for (const auto& [part, tier] : s.tiers) {
            if (tiers.size() > 1) tiers += ',';
            tiers += JsonString(part) + ":" + std::to_string(tier);
        }
        AppendField(out, "tiers", tiers + "}");
        AppendField(out, "art", JsonString(s.art_url));
        AppendField(out, "page", JsonString(s.page_url));
        AppendField(out, "host", JsonString(s.host));
        AppendField(out, "download", s.download_url.empty() ? "false" : "true");
        AppendField(out, "downloaded", IsDownloaded(s, local) ? "true" : "false");
        AppendField(out, "song_id", std::to_string(s.song_id));
        AppendField(out, "in_library", InLibrary(s.song_id, local.game_ids));
        const UpdateState update = UpdateOf(s, local.records);
        AppendField(out, "update", update == UpdateState::kAvailable ? "\"available\""
                                   : update == UpdateState::kPending  ? "\"pending\""
                                                                      : "\"\"");
        out += '}';
    }
    return out + "]}";
}

std::string FormatDownloads(const std::vector<Download>& downloads, std::string_view folder,
                            const std::optional<std::set<int32_t>>& game_ids) {
    std::string out = "{\"folder\":" + JsonString(folder) + ",\"downloads\":[";
    for (size_t i = 0; i < downloads.size(); i++) {
        const Download& d = downloads[i];
        if (i) out += ',';
        std::string_view state = "queued";
        switch (d.state) {
            case Download::State::kQueued: state = "queued"; break;
            case Download::State::kDownloading: state = "downloading"; break;
            case Download::State::kDone: state = "done"; break;
            case Download::State::kFailed: state = "failed"; break;
        }
        out += '{';
        AppendField(out, "file_id", JsonString(d.file_id));
        AppendField(out, "title", JsonString(d.title));
        AppendField(out, "artist", JsonString(d.artist));
        AppendField(out, "state", JsonString(state));
        AppendField(out, "received", std::to_string(d.received));
        AppendField(out, "total", std::to_string(d.total));
        AppendField(out, "error", JsonString(d.error));
        AppendField(out, "song_id", std::to_string(d.song_id));
        AppendField(out, "update", d.update ? "true" : "false");
        AppendField(out, "in_library", InLibrary(d.song_id, game_ids));
        out += '}';
    }
    return out + "]}";
}

}
