#include "src/Video/video_search.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <set>

namespace band3::video {

namespace {

// a Latin letter with an accent (Latin-1 and Latin Extended-A), as plain
// letters; nullptr for anything else
const char* Plain(char32_t c) {
    static constexpr struct {
        char32_t from, to;
        const char* plain;
    } kRanges[] = {
        {0xC0, 0xC5, "a"}, {0xC6, 0xC6, "ae"}, {0xC7, 0xC7, "c"}, {0xC8, 0xCB, "e"},
        {0xCC, 0xCF, "i"}, {0xD0, 0xD0, "d"},  {0xD1, 0xD1, "n"}, {0xD2, 0xD6, "o"},
        {0xD8, 0xD8, "o"}, {0xD9, 0xDC, "u"},  {0xDD, 0xDD, "y"}, {0xDF, 0xDF, "ss"},
        {0xE0, 0xE5, "a"}, {0xE6, 0xE6, "ae"}, {0xE7, 0xE7, "c"}, {0xE8, 0xEB, "e"},
        {0xEC, 0xEF, "i"}, {0xF0, 0xF0, "d"},  {0xF1, 0xF1, "n"}, {0xF2, 0xF6, "o"},
        {0xF8, 0xF8, "o"}, {0xF9, 0xFC, "u"},  {0xFD, 0xFD, "y"}, {0xFF, 0xFF, "y"},
        {0x100, 0x105, "a"}, {0x106, 0x10D, "c"}, {0x10E, 0x111, "d"}, {0x112, 0x11B, "e"},
        {0x11C, 0x123, "g"}, {0x124, 0x127, "h"}, {0x128, 0x131, "i"}, {0x134, 0x135, "j"},
        {0x136, 0x138, "k"}, {0x139, 0x142, "l"}, {0x143, 0x14B, "n"}, {0x14C, 0x151, "o"},
        {0x152, 0x153, "oe"}, {0x154, 0x159, "r"}, {0x15A, 0x161, "s"}, {0x162, 0x167, "t"},
        {0x168, 0x173, "u"}, {0x174, 0x175, "w"}, {0x176, 0x178, "y"}, {0x179, 0x17E, "z"},
    };
    for (const auto& r : kRanges)
        if (c >= r.from && c <= r.to) return r.plain;
    return nullptr;
}

// the next code point of UTF-8 text, from `at`; a stray byte as itself
char32_t Next(std::string_view s, size_t& at, size_t& length) {
    const auto b = static_cast<unsigned char>(s[at]);
    length = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : (b >> 3) == 30 ? 4 : 1;
    if (at + length > s.size()) length = 1;
    char32_t c = length == 1 ? b : b & (0xFF >> (length + 1));
    for (size_t k = 1; k < length; k++) c = (c << 6) | (static_cast<unsigned char>(s[at + k]) & 0x3F);
    const char32_t out = c;
    at += length;
    return out;
}

std::vector<std::string> Words(std::string_view normalized) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at < normalized.size()) {
        const size_t space = normalized.find(' ', at);
        const size_t end = space == std::string_view::npos ? normalized.size() : space;
        if (end > at) out.emplace_back(normalized.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

// the words that matter for a match
std::vector<std::string> KeyWords(std::string_view text) {
    std::vector<std::string> out;
    for (std::string& w : Words(Normalized(text)))
        if (w != "the" && w != "a" && w != "an") out.push_back(std::move(w));
    return out;
}

std::string Compact(std::string_view text) {
    std::string out;
    for (const char c : Normalized(text))
        if (c != ' ') out += c;
    return out;
}

// whether `phrase` is a whole word, or whole words, of normalized text
bool HasPhrase(std::string_view normalized, std::string_view phrase) {
    const std::string padded = " " + std::string(normalized) + " ";
    return padded.find(" " + std::string(phrase) + " ") != std::string::npos;
}

// in a bracketed part of a chart's title: what charts add
constexpr std::string_view kChartWords[] = {
    "2x",   "bass pedal", "rerecord", "re record", "rb3",       "rb2",      "rb4",
    "rbn",  "version",    "ver",      "feat",      "ft",        "featuring", "remaster",
    "remastered", "album", "radio edit", "single", "original", "explicit", "clean",
};
// ... unless it says what kind of take it is
constexpr std::string_view kTakeWords[] = {"live", "acoustic", "unplugged", "remix", "demo",
                                           "instrumental", "cover"};

// words in a video's title that say it's another kind of upload
constexpr std::string_view kOtherUploads[] = {
    "lyric",      "lyrics",       "visualizer", "visualiser", "live",       "cover",
    "karaoke",    "remix",        "sped up",    "slowed",     "8d",         "nightcore",
    "reaction",   "reacts",       "playthrough", "rock band", "full album", "tutorial",
    "lesson",     "instrumental", "backing track", "audio",   "how to play", "isolated",
    "drum cam",   "guitar cam",
};

}

std::string Normalized(std::string_view text) {
    std::string out;
    bool space = true;  // nothing yet, or a space last
    const auto add = [&](std::string_view s) {
        out += s;
        space = false;
    };
    const auto gap = [&] {
        if (!space) out += ' ';
        space = true;
    };
    for (size_t at = 0; at < text.size();) {
        size_t length = 0;
        const char32_t c = Next(text, at, length);
        if (c < 0x80) {
            const char a = char(c);
            if (a >= 'A' && a <= 'Z') add(std::string(1, char(a - 'A' + 'a')));
            else if ((a >= 'a' && a <= 'z') || (a >= '0' && a <= '9')) add(std::string(1, a));
            else if (a == '&') {
                gap();
                add("and");
                gap();
            } else if (a != '\'') gap();
        } else if (c == 0x2019 || c == 0x2018) {
            // a curly apostrophe: dropped as a straight one is
        } else if (const char* p = Plain(c)) {
            add(p);
        } else if ((c >= 0x2000 && c <= 0x206F) || (c >= 0xA0 && c <= 0xBF) || c == 0xD7 ||
                   c == 0xF7 || (c >= 0x3000 && c <= 0x303F)) {
            // punctuation and spaces
            gap();
        } else {
            // other scripts' letters, as they are
            add(text.substr(at - length, length));
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string ChartTitle(std::string_view title) {
    std::string out;
    size_t at = 0;
    while (at < title.size()) {
        const char open = title[at];
        const char close = open == '(' ? ')' : open == '[' ? ']' : 0;
        const size_t end = close ? title.find(close, at + 1) : std::string_view::npos;
        if (end == std::string_view::npos) {
            out += open;
            at++;
            continue;
        }
        const std::string inner = Normalized(title.substr(at + 1, end - at - 1));
        const bool take = std::any_of(std::begin(kTakeWords), std::end(kTakeWords),
                                      [&](std::string_view w) { return HasPhrase(inner, w); });
        const bool chart = std::any_of(std::begin(kChartWords), std::end(kChartWords),
                                       [&](std::string_view w) { return HasPhrase(inner, w); });
        if (!chart || take) out += title.substr(at, end - at + 1);
        at = end + 1;
    }
    // "Song feat. Someone" outside brackets
    for (std::string_view feat : {" feat. ", " ft. ", " featuring "}) {
        const size_t f = out.find(feat);
        if (f != std::string::npos) out.erase(f);
    }
    // the spaces left where brackets were
    std::string tidy;
    for (const char c : out)
        if (c != ' ' || (!tidy.empty() && tidy.back() != ' ')) tidy += c;
    while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
    return tidy;
}

std::string PlainQuery(const SongInfo& song) { return song.artist + " " + ChartTitle(song.title); }

std::string SearchQuery(const SongInfo& song) { return PlainQuery(song) + " official music video"; }

ScoredCandidate Score(const SongInfo& song, const YtCandidate& video) {
    ScoredCandidate s;
    s.video = video;
    const std::string title = Normalized(video.title);
    const std::string channel = Normalized(video.channel);
    std::set<std::string> in_title, in_title_or_channel;
    for (std::string& w : Words(title)) in_title.insert(std::move(w));
    in_title_or_channel = in_title;
    for (std::string& w : Words(channel)) in_title_or_channel.insert(std::move(w));
    const auto share = [](const std::vector<std::string>& want, const std::set<std::string>& have) {
        if (want.empty()) return 0.0;
        size_t n = 0;
        for (const std::string& w : want) n += have.count(w);
        return double(n) / double(want.size());
    };

    // the song's title, and its artist, in the video's
    s.score += 0.4 * share(KeyWords(ChartTitle(song.title)), in_title);
    s.score += 0.2 * share(KeyWords(song.artist), in_title_or_channel);
    // the artist's own channel, or their VEVO
    const std::string artist = Compact(song.artist), from = Compact(video.channel);
    if (!artist.empty() && (from == artist || from == artist + "vevo" ||
                            from == artist + "official" || from == "official" + artist))
        s.score += 0.15;
    if (HasPhrase(title, "official video") || HasPhrase(title, "official music video") ||
        HasPhrase(title, "music video"))
        s.score += 0.1;
    // a video often runs longer than its song (an intro, an ending), seldom shorter
    if (song.length_ms > 0 && video.duration > 0) {
        const double d = video.duration - song.length_ms / 1000.0;
        if (d >= -10 && d <= 90) s.score += 0.1;
        else if (d < -10) s.score -= std::min(0.3, (-d - 10) / 60 * 0.3);
        else s.score -= std::min(0.2, (d - 90) / 300 * 0.2);
        // a radio edit, most often: a single offset can't follow its cuts
        if (d < -30) s.note = std::format("{:.0f} s shorter than the song: likely another edit", -d);
    }
    if (video.views > 0) s.score += std::min(0.05, std::log10(double(video.views)) / 8 * 0.05);

    const std::string own = Normalized(song.title) + " " + Normalized(song.artist);
    for (std::string_view word : kOtherUploads) {
        if (!HasPhrase(title, word) || HasPhrase(own, word)) continue;
        s.penalized = true;
        s.score -= 0.3;
        s.note = "\"" + std::string(word) + "\" in its title";
        break;
    }
    const std::string about = Normalized(video.description);
    if (channel.ends_with(" topic") || channel == "topic" ||
        about.find("provided to youtube by") != std::string::npos ||
        about.find("auto generated by youtube") != std::string::npos) {
        s.rejected = true;
        s.note = "an art track (a still picture)";
    }
    return s;
}

std::vector<ScoredCandidate> Rank(const SongInfo& song, const std::vector<YtCandidate>& videos) {
    std::vector<ScoredCandidate> out;
    for (const YtCandidate& v : videos) out.push_back(Score(song, v));
    std::stable_sort(out.begin(), out.end(),
                     [](const ScoredCandidate& a, const ScoredCandidate& b) { return a.score > b.score; });
    return out;
}

bool CanAutoAccept(const ScoredCandidate& c) {
    return c.score >= kAutoAcceptScore && !c.penalized && !c.rejected;
}

}
