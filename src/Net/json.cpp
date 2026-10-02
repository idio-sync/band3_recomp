#include "json.h"

#include <charconv>
#include <cmath>
#include <cstdint>

namespace band3::json {

namespace {

// nesting deeper than this isn't a reply anyone meant to send
constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    std::optional<Value> Document() {
        auto value = Read(0);
        Space();
        if (!value || at_ != text_.size()) return std::nullopt;
        return value;
    }

private:
    void Space() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' ||
                                      text_[at_] == '\n' || text_[at_] == '\r')) {
            at_++;
        }
    }

    bool Literal(std::string_view word) {
        if (text_.substr(at_, word.size()) != word) return false;
        at_ += word.size();
        return true;
    }

    std::optional<Value> Read(int depth) {
        if (depth > kMaxDepth) return std::nullopt;
        Space();
        if (at_ >= text_.size()) return std::nullopt;
        switch (text_[at_]) {
            case '{': return ReadObject(depth);
            case '[': return ReadArray(depth);
            case '"': {
                auto s = ReadString();
                if (!s) return std::nullopt;
                return Value(std::move(*s));
            }
            case 't': if (Literal("true")) return Value(true); return std::nullopt;
            case 'f': if (Literal("false")) return Value(false); return std::nullopt;
            case 'n': if (Literal("null")) return Value(); return std::nullopt;
            default: return ReadNumber();
        }
    }

    std::optional<Value> ReadObject(int depth) {
        at_++;  // {
        Value::Object object;
        Space();
        if (at_ < text_.size() && text_[at_] == '}') {
            at_++;
            return Value(std::move(object));
        }
        while (true) {
            Space();
            if (at_ >= text_.size() || text_[at_] != '"') return std::nullopt;
            auto key = ReadString();
            if (!key) return std::nullopt;
            Space();
            if (at_ >= text_.size() || text_[at_] != ':') return std::nullopt;
            at_++;
            auto value = Read(depth + 1);
            if (!value) return std::nullopt;
            object.insert_or_assign(std::move(*key), std::move(*value));
            Space();
            if (at_ >= text_.size()) return std::nullopt;
            if (text_[at_] == '}') {
                at_++;
                return Value(std::move(object));
            }
            if (text_[at_] != ',') return std::nullopt;
            at_++;
        }
    }

    std::optional<Value> ReadArray(int depth) {
        at_++;  // [
        Value::Array array;
        Space();
        if (at_ < text_.size() && text_[at_] == ']') {
            at_++;
            return Value(std::move(array));
        }
        while (true) {
            auto value = Read(depth + 1);
            if (!value) return std::nullopt;
            array.push_back(std::move(*value));
            Space();
            if (at_ >= text_.size()) return std::nullopt;
            if (text_[at_] == ']') {
                at_++;
                return Value(std::move(array));
            }
            if (text_[at_] != ',') return std::nullopt;
            at_++;
        }
    }

    std::optional<uint32_t> Hex4() {
        if (at_ + 4 > text_.size()) return std::nullopt;
        uint32_t out = 0;
        const auto [ptr, ec] = std::from_chars(text_.data() + at_, text_.data() + at_ + 4, out, 16);
        if (ec != std::errc() || ptr != text_.data() + at_ + 4) return std::nullopt;
        at_ += 4;
        return out;
    }

    static void AppendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::optional<std::string> ReadString() {
        at_++;  // "
        std::string out;
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"') return out;
            if (static_cast<unsigned char>(c) < 0x20) return std::nullopt;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= text_.size()) return std::nullopt;
            const char e = text_[at_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    auto cp = Hex4();
                    if (!cp) return std::nullopt;
                    // a surrogate pair is one character; half of one is U+FFFD
                    if (*cp >= 0xD800 && *cp < 0xDC00 && text_.substr(at_, 2) == "\\u") {
                        const size_t back = at_;
                        at_ += 2;
                        auto low = Hex4();
                        if (low && *low >= 0xDC00 && *low < 0xE000) {
                            *cp = 0x10000 + ((*cp - 0xD800) << 10) + (*low - 0xDC00);
                        } else {
                            at_ = back;
                            *cp = 0xFFFD;
                        }
                    } else if (*cp >= 0xD800 && *cp < 0xE000) {
                        *cp = 0xFFFD;
                    }
                    AppendUtf8(out, *cp);
                    break;
                }
                default: return std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::optional<Value> ReadNumber() {
        const size_t start = at_;
        if (at_ < text_.size() && text_[at_] == '-') at_++;
        while (at_ < text_.size() &&
               ((text_[at_] >= '0' && text_[at_] <= '9') || text_[at_] == '.' ||
                text_[at_] == 'e' || text_[at_] == 'E' || text_[at_] == '+' || text_[at_] == '-')) {
            at_++;
        }
        double d = 0;
        const auto [ptr, ec] = std::from_chars(text_.data() + start, text_.data() + at_, d);
        if (start == at_ || ec != std::errc() || ptr != text_.data() + at_) return std::nullopt;
        return Value(d);
    }

    std::string_view text_;
    size_t at_ = 0;
};

const Value kNull;
const Value::Array kNoItems;
const Value::Object kNoMembers;

}

const Value& Value::operator[](std::string_view key) const {
    if (!IsObject()) return kNull;
    const auto& object = *std::get<std::shared_ptr<Object>>(v_);
    const auto it = object.find(key);
    return it == object.end() ? kNull : it->second;
}

const Value::Array& Value::Items() const {
    if (!IsArray()) return kNoItems;
    return *std::get<std::shared_ptr<Array>>(v_);
}

const Value::Object& Value::Members() const {
    if (!IsObject()) return kNoMembers;
    return *std::get<std::shared_ptr<Object>>(v_);
}

std::string Value::Text() const {
    if (const auto* s = std::get_if<std::string>(&v_)) return *s;
    if (const auto* b = std::get_if<bool>(&v_)) return *b ? "true" : "false";
    if (const auto* d = std::get_if<double>(&v_)) {
        char buf[32];
        const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), *d);
        return ec == std::errc() ? std::string(buf, ptr) : std::string();
    }
    return {};
}

double Value::Number(double fallback) const {
    if (const auto* d = std::get_if<double>(&v_)) return *d;
    if (const auto* s = std::get_if<std::string>(&v_)) {
        double d = 0;
        const auto [ptr, ec] = std::from_chars(s->data(), s->data() + s->size(), d);
        if (!s->empty() && ec == std::errc() && ptr == s->data() + s->size()) return d;
    }
    return fallback;
}

bool Value::Bool(bool fallback) const {
    if (const auto* b = std::get_if<bool>(&v_)) return *b;
    if (const auto* d = std::get_if<double>(&v_)) return *d != 0;
    return fallback;
}

std::optional<Value> Parse(std::string_view text) { return Parser(text).Document(); }

}
