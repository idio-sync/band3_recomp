#include "lyrics.h"
#include <algorithm>
#include <utility>

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

}
