#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A song's lyrics for the web server's /karaoke page (http_server.h), read
// from its MIDI file as Rock Band songs are authored: PART VOCALS and HARM1
// to HARM3, each lyric on the note it's sung on, lines between phrase
// markers. Nothing here needs the game.

namespace band3::lyrics {

// One event of a MIDI track that the lyrics need: a note starting or ending
// (a note-on of velocity 0 ends it), a text or lyric meta event, or a tempo
// change
struct MidiEvent {
    enum class Kind { kNoteOn, kNoteOff, kText, kLyric, kTempo };
    uint32_t tick = 0;
    Kind kind = Kind::kText;
    uint8_t note = 0;              // kNoteOn, kNoteOff
    std::string text;              // kText (FF 01), kLyric (FF 05), as written
    uint32_t us_per_quarter = 0;   // kTempo (FF 51)
};

struct MidiTrack {
    std::string name;  // its first track name (FF 03); empty if none
    std::vector<MidiEvent> events;  // in the track's order, so by tick
};

struct MidiFile {
    uint16_t ticks_per_quarter = 0;
    std::vector<MidiTrack> tracks;
};

// nullopt when it isn't a standard MIDI file timed in ticks per quarter note
// (not SMPTE), or a chunk or event runs past its end
std::optional<MidiFile> ReadMidi(std::string_view bytes);

// Ticks to milliseconds through the file's tempo changes, from every track
// (Rock Band's are in the first): 120 bpm until the first one.
class TempoMap {
public:
    explicit TempoMap(const MidiFile& midi);
    double Ms(uint32_t tick) const;

private:
    struct Segment {
        uint32_t tick;
        double ms;
        double ms_per_tick;
    };
    std::vector<Segment> segments_;  // by tick, the first at 0
};

// One sung syllable, in the song's milliseconds
struct Syllable {
    int32_t start_ms = 0;
    int32_t end_ms = 0;
    std::string text;     // UTF-8, the authoring markers gone
    bool join = false;    // runs into the next syllable with no space
    bool spoken = false;  // spoken or talky (# or ^): no pitch
};

// one phrase: what the page shows as a line
struct Line {
    int32_t start_ms = 0;
    int32_t end_ms = 0;
    std::vector<Syllable> syllables;
};

struct Part {
    std::string part;  // "lead" (PART VOCALS), "harm1", "harm2", "harm3"
    std::vector<Line> lines;
};

// A lyric's text with Rock Band's markers read off it: a trailing - joins the
// next syllable, = joins it showing a hyphen, # and ^ mark it spoken, $
// hides it (a harmony's copy of another part's word), % is a range marker, +
// alone carries the last syllable onto another note (a slide), and §
// joins two syllables sung on one note (shown as a tie, ‿). Latin-1 text
// becomes UTF-8.
struct CleanText {
    std::string text;
    bool join = false;
    bool spoken = false;
    bool hidden = false;
    bool slide = false;
};
CleanText Clean(std::string_view raw);

// The song's vocal parts that have lines: lead, then harm1 to harm3. A
// syllable is a pitched note (36 to 84) with a lyric at its start; lines are
// phrase markers (notes 105 and 106, overlapping ones merged). HARM2's
// markers are HARM3's too, and HARM1's stand in where HARM2 has none.
// Syllables outside every phrase aren't shown.
std::vector<Part> FromMidi(const MidiFile& midi);

// what /lyrics?shortname= replies:
// {"shortname":..., "parts":[{"part":..., "lines":[{"start_ms":..., "end_ms":...,
//   "syllables":[{"start_ms":..., "end_ms":..., "text":..., "join":..., "spoken":...}]}]}]}
std::string FormatJson(std::string_view shortname, const std::vector<Part>& parts);

}
