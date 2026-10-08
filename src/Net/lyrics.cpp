#include "lyrics.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include "src/Net/http_request.h"

namespace band3::lyrics {

namespace {

// reads big-endian numbers and variable-length quantities; once anything
// runs past the end, ok stays false and every read gives 0
struct Reader {
    std::string_view data;
    size_t pos = 0;
    bool ok = true;

    bool Has(size_t n) const { return ok && data.size() - pos >= n; }
    uint8_t U8() {
        if (!Has(1)) {
            ok = false;
            return 0;
        }
        return static_cast<uint8_t>(data[pos++]);
    }
    uint32_t Be(int bytes) {
        uint32_t v = 0;
        for (int i = 0; i < bytes; i++) v = (v << 8) | U8();
        return v;
    }
    uint32_t Vlq() {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            const uint8_t b = U8();
            v = (v << 7) | (b & 0x7F);
            if (!(b & 0x80)) return v;
        }
        ok = false;
        return 0;
    }
    std::string_view Take(size_t n) {
        if (!Has(n)) {
            ok = false;
            return {};
        }
        const std::string_view out = data.substr(pos, n);
        pos += n;
        return out;
    }
};

std::optional<MidiTrack> ReadTrack(std::string_view bytes) {
    Reader r{bytes};
    MidiTrack track;
    bool named = false;
    uint32_t tick = 0;
    uint8_t status = 0;  // running status; meta events and sysex end it
    while (r.ok && r.pos < bytes.size()) {
        tick += r.Vlq();
        const uint8_t b = r.U8();
        if (b == 0xFF) {
            const uint8_t type = r.U8();
            const std::string_view body = r.Take(r.Vlq());
            status = 0;
            if (!r.ok) break;
            if (type == 0x2F) break;  // end of track
            if (type == 0x03 && !named) {
                track.name = std::string(body);
                named = true;
            } else if (type == 0x01 || type == 0x05) {
                MidiEvent e;
                e.tick = tick;
                e.kind = type == 0x05 ? MidiEvent::Kind::kLyric : MidiEvent::Kind::kText;
                e.text = std::string(body);
                track.events.push_back(std::move(e));
            } else if (type == 0x51 && body.size() == 3) {
                MidiEvent e;
                e.tick = tick;
                e.kind = MidiEvent::Kind::kTempo;
                e.us_per_quarter = (static_cast<uint32_t>(static_cast<uint8_t>(body[0])) << 16) |
                                   (static_cast<uint32_t>(static_cast<uint8_t>(body[1])) << 8) |
                                   static_cast<uint8_t>(body[2]);
                track.events.push_back(std::move(e));
            }
            continue;
        }
        if (b == 0xF0 || b == 0xF7) {
            r.Take(r.Vlq());
            status = 0;
            continue;
        }
        if (b > 0xF0) return std::nullopt;  // system messages don't belong in a file
        uint8_t first;
        if (b & 0x80) {
            status = b;
            first = r.U8();
        } else {
            if (!status) return std::nullopt;
            first = b;
        }
        const uint8_t kind = status & 0xF0;
        const uint8_t second = (kind == 0xC0 || kind == 0xD0) ? 0 : r.U8();
        if (!r.ok) break;
        if (kind == 0x90 || kind == 0x80) {
            MidiEvent e;
            e.tick = tick;
            e.kind = kind == 0x90 && second > 0 ? MidiEvent::Kind::kNoteOn
                                                : MidiEvent::Kind::kNoteOff;
            e.note = first;
            track.events.push_back(std::move(e));
        }
    }
    if (!r.ok) return std::nullopt;
    return track;
}

}

std::optional<MidiFile> ReadMidi(std::string_view bytes) {
    Reader r{bytes};
    if (r.Take(4) != "MThd") return std::nullopt;
    const uint32_t header_size = r.Be(4);
    if (!r.ok || header_size < 6) return std::nullopt;
    r.Be(2);  // format
    const uint32_t count = r.Be(2);
    const uint32_t division = r.Be(2);
    r.Take(header_size - 6);
    if (!r.ok || division == 0 || (division & 0x8000)) return std::nullopt;
    MidiFile midi;
    midi.ticks_per_quarter = static_cast<uint16_t>(division);
    // the header counts track chunks; chunks of other kinds are passed over
    while (midi.tracks.size() < count && r.pos < bytes.size()) {
        const std::string_view id = r.Take(4);
        const std::string_view body = r.Take(r.Be(4));
        if (!r.ok) return std::nullopt;
        if (id != "MTrk") continue;
        std::optional<MidiTrack> track = ReadTrack(body);
        if (!track) return std::nullopt;
        midi.tracks.push_back(std::move(*track));
    }
    return midi;
}

TempoMap::TempoMap(const MidiFile& midi) {
    std::vector<std::pair<uint32_t, uint32_t>> tempos;
    for (const MidiTrack& track : midi.tracks) {
        for (const MidiEvent& e : track.events) {
            if (e.kind == MidiEvent::Kind::kTempo) tempos.emplace_back(e.tick, e.us_per_quarter);
        }
    }
    std::stable_sort(tempos.begin(), tempos.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    const double ticks = midi.ticks_per_quarter;
    double ms = 0;
    uint32_t tick = 0;
    double per_tick = 500.0 / ticks;  // 120 bpm: 500 ms a quarter
    segments_.push_back({0, 0, per_tick});
    for (const auto& [at, us] : tempos) {
        ms += (at - tick) * per_tick;
        tick = at;
        per_tick = us / 1000.0 / ticks;
        if (segments_.back().tick == at) {
            segments_.back() = {at, ms, per_tick};
        } else {
            segments_.push_back({at, ms, per_tick});
        }
    }
}

double TempoMap::Ms(uint32_t tick) const {
    const auto after = std::upper_bound(
        segments_.begin(), segments_.end(), tick,
        [](uint32_t t, const Segment& s) { return t < s.tick; });
    const Segment& s = *(after - 1);
    return s.ms + (tick - s.tick) * s.ms_per_tick;
}

namespace {

constexpr uint8_t kPitchLow = 36, kPitchHigh = 84;
constexpr uint8_t kPhrase = 105, kPhrase2 = 106;

struct Span {
    uint32_t start = 0, end = 0;
};

// the notes `pick` takes, as start..end, by start
template <typename Pick>
std::vector<Span> NoteSpans(const MidiTrack& track, Pick pick) {
    std::vector<Span> spans;
    std::map<uint8_t, uint32_t> open;
    for (const MidiEvent& e : track.events) {
        if (e.kind != MidiEvent::Kind::kNoteOn && e.kind != MidiEvent::Kind::kNoteOff) continue;
        if (!pick(e.note)) continue;
        const auto it = open.find(e.note);
        if (it != open.end()) {
            // a note on again before its off ends the first there
            spans.push_back({it->second, e.tick});
            open.erase(it);
        }
        if (e.kind == MidiEvent::Kind::kNoteOn) open[e.note] = e.tick;
    }
    std::stable_sort(spans.begin(), spans.end(),
                     [](const Span& a, const Span& b) { return a.start < b.start; });
    return spans;
}

std::vector<Span> Phrases(const MidiTrack& track) {
    const std::vector<Span> spans =
        NoteSpans(track, [](uint8_t n) { return n == kPhrase || n == kPhrase2; });
    std::vector<Span> merged;
    for (const Span& s : spans) {
        if (!merged.empty() && s.start < merged.back().end) {
            merged.back().end = std::max(merged.back().end, s.end);
        } else {
            merged.push_back(s);
        }
    }
    return merged;
}

const MidiTrack* Find(const MidiFile& midi, std::string_view name) {
    for (const MidiTrack& track : midi.tracks) {
        if (track.name == name) return &track;
    }
    return nullptr;
}

int32_t Round(double ms) { return static_cast<int32_t>(std::lround(ms)); }

Part ReadPart(std::string name, const MidiTrack& notes_track, const std::vector<Span>& phrases,
              const TempoMap& tempo) {
    // lyric events, or text events where the track has none (older songs),
    // minus text events' [markers]
    const bool has_lyrics =
        std::ranges::any_of(notes_track.events,
                            [](const MidiEvent& e) { return e.kind == MidiEvent::Kind::kLyric; });
    std::multimap<uint32_t, std::string_view> words;
    for (const MidiEvent& e : notes_track.events) {
        const bool lyric = has_lyrics ? e.kind == MidiEvent::Kind::kLyric
                                      : e.kind == MidiEvent::Kind::kText && !e.text.empty() &&
                                            e.text.front() != '[';
        if (lyric) words.emplace(e.tick, e.text);
    }

    struct Sung {
        Span span;
        CleanText text;
    };
    std::vector<Sung> sung;
    bool last_shown = false;  // so a slide after a hidden syllable stays hidden
    for (const Span& note : NoteSpans(notes_track, [](uint8_t n) {
             return n >= kPitchLow && n <= kPitchHigh;
         })) {
        const auto word = words.find(note.start);
        if (word == words.end()) continue;
        CleanText text = Clean(word->second);
        if (text.slide) {
            if (last_shown) sung.back().span.end = std::max(sung.back().span.end, note.end);
            continue;
        }
        last_shown = !text.hidden && !text.text.empty();
        if (last_shown) sung.push_back({note, std::move(text)});
    }

    Part part{std::move(name), {}};
    size_t i = 0;
    for (const Span& phrase : phrases) {
        while (i < sung.size() && sung[i].span.start < phrase.start) i++;
        Line line;
        for (; i < sung.size() && sung[i].span.start < phrase.end; i++) {
            const Sung& s = sung[i];
            line.syllables.push_back({Round(tempo.Ms(s.span.start)), Round(tempo.Ms(s.span.end)),
                                      s.text.text, s.text.join, s.text.spoken});
        }
        if (line.syllables.empty()) continue;
        line.start_ms = Round(tempo.Ms(phrase.start));
        line.end_ms = Round(tempo.Ms(phrase.end));
        line.syllables.back().join = false;
        part.lines.push_back(std::move(line));
    }
    return part;
}

void ReplaceAll(std::string& s, std::string_view from, std::string_view to) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) {
        s.replace(at, from.size(), to);
    }
}

}

CleanText Clean(std::string_view raw) {
    CleanText out;
    std::string s = http::ToUtf8(raw);
    const size_t first = s.find_first_not_of(' ');
    s = first == std::string::npos ? std::string()
                                   : s.substr(first, s.find_last_not_of(' ') - first + 1);
    if (!s.empty() && s.front() == '$') {
        out.hidden = true;
        s.erase(0, 1);
    }
    for (bool more = true; more && !s.empty();) {
        switch (s.back()) {
            case '-': out.join = true; s.pop_back(); break;
            case '=': out.join = true; s.back() = '-'; more = false; break;
            case '#':
            case '^': out.spoken = true; s.pop_back(); break;
            case '$': out.hidden = true; s.pop_back(); break;
            case '%': s.pop_back(); break;
            default: more = false;
        }
    }
    if (s == "+") {
        out.slide = true;
        s.clear();
    }
    ReplaceAll(s, "\xC2\xA7", "\xE2\x80\xBF");
    out.text = std::move(s);
    return out;
}

std::vector<Part> FromMidi(const MidiFile& midi) {
    const TempoMap tempo(midi);
    std::vector<Part> parts;
    auto add = [&](const char* name, const MidiTrack* notes, const std::vector<Span>& phrases) {
        if (!notes) return;
        Part part = ReadPart(name, *notes, phrases, tempo);
        if (!part.lines.empty()) parts.push_back(std::move(part));
    };
    if (const MidiTrack* lead = Find(midi, "PART VOCALS")) add("lead", lead, Phrases(*lead));
    const MidiTrack* harm1 = Find(midi, "HARM1");
    const MidiTrack* harm2 = Find(midi, "HARM2");
    const std::vector<Span> harm1_phrases = harm1 ? Phrases(*harm1) : std::vector<Span>{};
    std::vector<Span> harm2_phrases = harm2 ? Phrases(*harm2) : std::vector<Span>{};
    if (harm2_phrases.empty()) harm2_phrases = harm1_phrases;
    add("harm1", harm1, harm1_phrases);
    add("harm2", harm2, harm2_phrases);
    add("harm3", Find(midi, "HARM3"), harm2_phrases);
    return parts;
}

std::string FormatJson(std::string_view shortname, const std::vector<Part>& parts) {
    auto flag = [](bool b) { return b ? "true" : "false"; };
    std::string out = "{\"shortname\":" + http::JsonString(shortname) + ",\"parts\":[";
    for (size_t p = 0; p < parts.size(); p++) {
        if (p) out += ',';
        out += "{\"part\":" + http::JsonString(parts[p].part) + ",\"lines\":[";
        for (size_t l = 0; l < parts[p].lines.size(); l++) {
            const Line& line = parts[p].lines[l];
            if (l) out += ',';
            out += "{\"start_ms\":" + std::to_string(line.start_ms) +
                   ",\"end_ms\":" + std::to_string(line.end_ms) + ",\"syllables\":[";
            for (size_t s = 0; s < line.syllables.size(); s++) {
                const Syllable& syl = line.syllables[s];
                if (s) out += ',';
                out += "{\"start_ms\":" + std::to_string(syl.start_ms) +
                       ",\"end_ms\":" + std::to_string(syl.end_ms) +
                       ",\"text\":" + http::JsonString(syl.text) + ",\"join\":" + flag(syl.join) +
                       ",\"spoken\":" + flag(syl.spoken) + "}";
            }
            out += "]}";
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

}
