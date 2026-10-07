#include "song_dta.h"

#include <charconv>
#include "src/Game/song_id.h"

namespace band3::content {

namespace {

// past this, the text is taken for something other than a songs.dta
constexpr int kMaxDepth = 64;

struct Node {
    enum class Kind { kAtom, kString, kList };
    Kind kind = Kind::kAtom;
    std::string text;         // an atom's, a 'symbol''s or a "string"'s
    std::vector<Node> items;  // a list's
};

class Reader {
public:
    explicit Reader(std::string_view text) : text_(text) {}

    enum class Result { kNode, kClose, kEnd, kBad };

    Result Read(Node& node, int depth) {
        Skip();
        if (at_ >= text_.size()) return Result::kEnd;
        const char c = text_[at_];
        if (c == ')' || c == ']' || c == '}') {
            at_++;
            return Result::kClose;
        }
        if (c == '(' || c == '[' || c == '{') {
            if (depth >= kMaxDepth) return Result::kBad;
            at_++;
            node.kind = Node::Kind::kList;
            while (true) {
                Node item;
                const Result result = Read(item, depth + 1);
                if (result == Result::kClose) return Result::kNode;
                if (result != Result::kNode) return Result::kBad;  // unclosed, or worse
                node.items.push_back(std::move(item));
            }
        }
        if (c == '"' || c == '\'') {
            const size_t end = text_.find(c, at_ + 1);
            if (end == std::string_view::npos) return Result::kBad;
            node.kind = c == '"' ? Node::Kind::kString : Node::Kind::kAtom;
            node.text = text_.substr(at_ + 1, end - at_ - 1);
            at_ = end + 1;
            return Result::kNode;
        }
        const size_t start = at_;
        while (at_ < text_.size() && !IsSpace(text_[at_]) &&
               std::string_view("()[]{}\"';").find(text_[at_]) == std::string_view::npos) {
            at_++;
        }
        node.text = text_.substr(start, at_ - start);
        return Result::kNode;
    }

private:
    static bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

    // whitespace, comments, and # directives (#ifdef, #include...), which
    // don't change what songs there are
    void Skip() {
        while (at_ < text_.size()) {
            const char c = text_[at_];
            if (IsSpace(c)) {
                at_++;
            } else if (c == ';' || c == '#') {
                const size_t end = text_.find('\n', at_);
                at_ = end == std::string_view::npos ? text_.size() : end + 1;
            } else if (text_.substr(at_, 2) == "/*") {
                const size_t end = text_.find("*/", at_ + 2);
                at_ = end == std::string_view::npos ? text_.size() : end + 2;
            } else {
                break;
            }
        }
    }

    std::string_view text_;
    size_t at_ = 0;
};

bool ValidUtf8(std::string_view text) {
    for (size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        const size_t length = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!length || i + length > text.size()) return false;
        for (size_t k = 1; k < length; k++) {
            if ((static_cast<unsigned char>(text[i + k]) >> 6) != 2) return false;
        }
        i += length;
    }
    return true;
}

// as the game takes it: UTF-8 for a song that says so, Latin-1 for the rest
// (and for text that says UTF-8 but isn't)
std::string ToUtf8(std::string_view text, bool utf8) {
    if (utf8 && ValidUtf8(text)) return std::string(text);
    std::string out;
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x80) {
            out += c;
        } else {
            out += static_cast<char>(0xC0 | (byte >> 6));
            out += static_cast<char>(0x80 | (byte & 0x3F));
        }
    }
    return out;
}

int32_t SongIdOf(const Node& value) {
    if (value.kind == Node::Kind::kList || value.text.empty()) return 0;
    const std::string& text = value.text;
    int64_t id = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), id);
    if (ec == std::errc() && end == text.data() + text.size()) {
        return id > 0 && id <= INT32_MAX ? static_cast<int32_t>(id) : 0;
    }
    // a text song_id, as the game turns it into a number
    return CorrectedSongId(text);
}

}

std::vector<DtaSong> ParseSongsDta(std::string_view text) {
    std::vector<DtaSong> songs;
    Reader reader(text);
    while (true) {
        Node entry;
        const Reader::Result result = reader.Read(entry, 0);
        if (result == Reader::Result::kClose) continue;  // a stray one
        if (result != Reader::Result::kNode) break;
        if (entry.kind != Node::Kind::kList || entry.items.empty() ||
            entry.items[0].kind == Node::Kind::kList || entry.items[0].text.empty()) {
            continue;
        }
        DtaSong song;
        song.shortname = entry.items[0].text;
        bool is_song = false, utf8 = false;
        std::string title, artist;
        for (const Node& field : entry.items) {
            if (field.kind != Node::Kind::kList || field.items.size() < 2 ||
                field.items[0].kind != Node::Kind::kAtom) {
                continue;
            }
            const std::string& key = field.items[0].text;
            const Node& value = field.items[1];
            if (key == "song") {
                is_song = true;
            } else if (key == "song_id") {
                song.song_id = SongIdOf(value);
            } else if (value.kind == Node::Kind::kList) {
                continue;
            } else if (key == "name") {
                title = value.text;
            } else if (key == "artist") {
                artist = value.text;
            } else if (key == "encoding") {
                utf8 = value.text == "utf8";
            }
        }
        if (!is_song) continue;
        song.title = ToUtf8(title, utf8);
        song.artist = ToUtf8(artist, utf8);
        songs.push_back(std::move(song));
    }
    return songs;
}

}
