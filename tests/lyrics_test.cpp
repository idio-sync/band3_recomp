// Checks the web server's lyrics (src/Net/lyrics.cpp): standard MIDI files
// built here byte by byte, so no song's data is in the repository.

#include <doctest/doctest.h>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include "src/Net/lyrics.h"

using namespace band3::lyrics;

namespace {

std::string Vlq(uint32_t v) {
    std::string out(1, static_cast<char>(v & 0x7F));
    while (v >>= 7) out.insert(out.begin(), static_cast<char>(0x80 | (v & 0x7F)));
    return out;
}

std::string Be(uint32_t v, int bytes) {
    std::string out;
    for (int i = bytes - 1; i >= 0; i--) out += static_cast<char>((v >> (8 * i)) & 0xFF);
    return out;
}

// A track's events, given at any tick in any order; Chunk() sorts them by
// tick (stably, so events at one tick keep the order they were added in)
class Track {
public:
    Track& Raw(uint32_t tick, std::string bytes) {
        events_.emplace_back(tick, std::move(bytes));
        return *this;
    }
    Track& Meta(uint32_t tick, uint8_t type, std::string_view body) {
        return Raw(tick, std::string("\xFF", 1) + static_cast<char>(type) +
                             Vlq(static_cast<uint32_t>(body.size())) + std::string(body));
    }
    Track& Name(std::string_view name) { return Meta(0, 0x03, name); }
    Track& Lyric(uint32_t tick, std::string_view text) { return Meta(tick, 0x05, text); }
    Track& Text(uint32_t tick, std::string_view text) { return Meta(tick, 0x01, text); }
    Track& Tempo(uint32_t tick, uint32_t us_per_quarter) {
        return Meta(tick, 0x51, Be(us_per_quarter, 3));
    }
    Track& Note(uint32_t tick, uint32_t length, uint8_t note) {
        Raw(tick, {'\x90', static_cast<char>(note), '\x64'});
        return Raw(tick + length, {'\x80', static_cast<char>(note), '\x00'});
    }
    std::string Chunk() const {
        auto events = events_;
        std::stable_sort(events.begin(), events.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        std::string body;
        uint32_t last = 0;
        for (const auto& [tick, bytes] : events) {
            body += Vlq(tick - last) + bytes;
            last = tick;
        }
        body += Vlq(0) + std::string("\xFF\x2F\x00", 3);
        return "MTrk" + Be(static_cast<uint32_t>(body.size()), 4) + body;
    }

private:
    std::vector<std::pair<uint32_t, std::string>> events_;
};

std::string Smf(uint16_t ticks_per_quarter, const std::vector<std::string>& chunks) {
    std::string out = "MThd" + Be(6, 4) + Be(1, 2) + Be(static_cast<uint32_t>(chunks.size()), 2) +
                      Be(ticks_per_quarter, 2);
    for (const auto& chunk : chunks) out += chunk;
    return out;
}

}

TEST_CASE("a MIDI file's tracks give their names, notes, lyrics and tempo") {
    const std::string bytes =
        Smf(480, {Track().Name("tempo").Tempo(0, 600000).Chunk(),
                  Track().Name("PART VOCALS").Note(480, 240, 60).Lyric(480, "Hel-").Chunk()});
    const auto midi = ReadMidi(bytes);
    REQUIRE(midi);
    CHECK(midi->ticks_per_quarter == 480);
    REQUIRE(midi->tracks.size() == 2);
    CHECK(midi->tracks[0].name == "tempo");
    REQUIRE(midi->tracks[0].events.size() == 1);
    CHECK(midi->tracks[0].events[0].kind == MidiEvent::Kind::kTempo);
    CHECK(midi->tracks[0].events[0].us_per_quarter == 600000);

    const auto& vocals = midi->tracks[1];
    CHECK(vocals.name == "PART VOCALS");
    REQUIRE(vocals.events.size() == 3);
    CHECK(vocals.events[0].kind == MidiEvent::Kind::kNoteOn);
    CHECK(vocals.events[0].tick == 480);
    CHECK(vocals.events[0].note == 60);
    CHECK(vocals.events[1].kind == MidiEvent::Kind::kLyric);
    CHECK(vocals.events[1].text == "Hel-");
    CHECK(vocals.events[2].kind == MidiEvent::Kind::kNoteOff);
    CHECK(vocals.events[2].tick == 720);
}

TEST_CASE("running status and note-ons of velocity 0 read as the notes they are") {
    // note on 60, then running status: note on 62, note "on" 60 at velocity 0
    const std::string body = Vlq(0) + std::string("\x90\x3C\x64", 3) + Vlq(10) +
                             std::string("\x3E\x64", 2) + Vlq(10) + std::string("\x3C\x00", 2) +
                             Vlq(0) + std::string("\xFF\x2F\x00", 3);
    const std::string bytes = Smf(480, {"MTrk" + Be(static_cast<uint32_t>(body.size()), 4) + body});
    const auto midi = ReadMidi(bytes);
    REQUIRE(midi);
    const auto& events = midi->tracks.at(0).events;
    REQUIRE(events.size() == 3);
    CHECK(events[1].kind == MidiEvent::Kind::kNoteOn);
    CHECK(events[1].note == 62);
    CHECK(events[1].tick == 10);
    CHECK(events[2].kind == MidiEvent::Kind::kNoteOff);
    CHECK(events[2].note == 60);
    CHECK(events[2].tick == 20);
}

TEST_CASE("sysex, other channel messages and unknown chunks are passed over") {
    const std::string body = Vlq(0) + std::string("\xF0\x03\x01\x02\xF7", 5) +  // sysex
                             Vlq(0) + std::string("\xC0\x05", 2) +              // program change
                             Vlq(0) + std::string("\xB0\x07\x64", 3) +          // controller
                             Vlq(5) + std::string("\x90\x40\x64", 3) +
                             Vlq(0) + std::string("\xFF\x2F\x00", 3);
    const std::string track = "MTrk" + Be(static_cast<uint32_t>(body.size()), 4) + body;
    const std::string other = "XFIH" + Be(2, 4) + "ab";
    const auto midi = ReadMidi(Smf(480, {other, track}));
    REQUIRE(midi);
    REQUIRE(midi->tracks.size() == 1);
    REQUIRE(midi->tracks[0].events.size() == 1);
    CHECK(midi->tracks[0].events[0].note == 0x40);
    CHECK(midi->tracks[0].events[0].tick == 5);
}

TEST_CASE("what isn't a MIDI file band3 can time is refused") {
    CHECK(!ReadMidi(""));
    CHECK(!ReadMidi("RIFF0000"));
    // SMPTE timing (the division's top bit)
    CHECK(!ReadMidi("MThd" + Be(6, 4) + Be(1, 2) + Be(0, 2) + Be(0xE728, 2)));
    // a track longer than the file
    const std::string whole = Smf(480, {Track().Note(0, 10, 60).Chunk()});
    CHECK(!ReadMidi(whole.substr(0, whole.size() - 3)));
    // an event cut off inside its track
    const std::string body = Vlq(0) + std::string("\x90\x3C", 2);
    CHECK(!ReadMidi(Smf(480, {"MTrk" + Be(static_cast<uint32_t>(body.size()), 4) + body})));
}

TEST_CASE("ticks become milliseconds through the tempo changes") {
    // 120 bpm until tick 960, then 60 bpm
    const auto midi = ReadMidi(Smf(480, {Track().Tempo(960, 1000000).Chunk()}));
    REQUIRE(midi);
    const TempoMap tempo(*midi);
    CHECK(tempo.Ms(0) == doctest::Approx(0));
    CHECK(tempo.Ms(480) == doctest::Approx(500));
    CHECK(tempo.Ms(960) == doctest::Approx(1000));
    CHECK(tempo.Ms(1440) == doctest::Approx(2000));
}

TEST_CASE("of two tempos at one tick the later one counts") {
    const auto midi =
        ReadMidi(Smf(480, {Track().Tempo(0, 1000000).Tempo(0, 250000).Chunk()}));
    REQUIRE(midi);
    CHECK(TempoMap(*midi).Ms(480) == doctest::Approx(250));
}

TEST_CASE("a lyric's markers are read off it") {
    CHECK(Clean("Hel-").text == "Hel");
    CHECK(Clean("Hel-").join);
    CHECK(!Clean("lo").join);
    CHECK(Clean("world#").text == "world");
    CHECK(Clean("world#").spoken);
    CHECK(Clean("talk^").spoken);
    CHECK(Clean("mon-#").join);
    CHECK(Clean("mon-#").spoken);
    CHECK(Clean("$hid").hidden);
    CHECK(Clean("hid$").hidden);
    CHECK(Clean("+").slide);
    CHECK(Clean("+-").slide);
    CHECK(Clean("+").text.empty());
    CHECK(Clean("re=").text == "re-");
    CHECK(Clean("re=").join);
    CHECK(Clean("word%").text == "word");
    CHECK(Clean("  spaced ").text == "spaced");
    // § in UTF-8 and in Latin-1
    CHECK(Clean("a\xC2\xA7" "b").text == "a\xE2\x80\xBF" "b");
    CHECK(Clean("a\xA7" "b").text == "a\xE2\x80\xBF" "b");
    // Latin-1 é becomes UTF-8
    CHECK(Clean("caf\xE9").text == "caf\xC3\xA9");
}

TEST_CASE("a phrase's notes and lyrics make a line of syllables in milliseconds") {
    // 120 bpm, 480 ticks a quarter: 480 ticks are 500 ms
    const auto midi = ReadMidi(Smf(480, {Track().Name("tempo").Chunk(),
                                         Track()
                                             .Name("PART VOCALS")
                                             .Note(480, 1440, 105)
                                             .Note(480, 240, 60).Lyric(480, "Hel-")
                                             .Note(720, 240, 62).Lyric(720, "lo")
                                             .Note(960, 480, 64).Lyric(960, "world")
                                             .Chunk()}));
    REQUIRE(midi);
    const auto parts = FromMidi(*midi);
    REQUIRE(parts.size() == 1);
    CHECK(parts[0].part == "lead");
    REQUIRE(parts[0].lines.size() == 1);
    const Line& line = parts[0].lines[0];
    CHECK(line.start_ms == 500);
    CHECK(line.end_ms == 2000);
    REQUIRE(line.syllables.size() == 3);
    CHECK(line.syllables[0].text == "Hel");
    CHECK(line.syllables[0].join);
    CHECK(line.syllables[0].start_ms == 500);
    CHECK(line.syllables[0].end_ms == 750);
    CHECK(line.syllables[1].text == "lo");
    CHECK(!line.syllables[1].join);
    CHECK(line.syllables[2].start_ms == 1000);
    CHECK(line.syllables[2].end_ms == 1500);
}

TEST_CASE("a slide carries a syllable on, and hidden or stray syllables don't show") {
    const auto midi = ReadMidi(Smf(480, {Track()
                                             .Name("PART VOCALS")
                                             .Note(0, 1920, 105)
                                             .Note(0, 240, 60).Lyric(0, "Oh")
                                             .Note(240, 240, 64).Lyric(240, "+")
                                             .Note(480, 240, 60).Lyric(480, "$gone")
                                             .Note(720, 240, 60).Lyric(720, "+")
                                             .Note(960, 240, 60).Lyric(960, "yeah-")
                                             // after the phrase: not shown
                                             .Note(2400, 240, 60).Lyric(2400, "stray")
                                             .Chunk()}));
    REQUIRE(midi);
    const auto parts = FromMidi(*midi);
    REQUIRE(parts.size() == 1);
    REQUIRE(parts[0].lines.size() == 1);
    const auto& syllables = parts[0].lines[0].syllables;
    REQUIRE(syllables.size() == 2);
    CHECK(syllables[0].text == "Oh");
    CHECK(syllables[0].end_ms == 500);  // to the slide's end
    CHECK(syllables[1].text == "yeah");
    // a line's last syllable joins nothing
    CHECK(!syllables[1].join);
}

TEST_CASE("each phrase is a line, and phrases without syllables are left out") {
    const auto midi = ReadMidi(Smf(480, {Track()
                                             .Name("PART VOCALS")
                                             .Note(0, 480, 105)
                                             .Note(0, 240, 60).Lyric(0, "one")
                                             .Note(960, 480, 105)  // nothing sung
                                             .Note(1920, 480, 105)
                                             .Note(1920, 240, 60).Lyric(1920, "two")
                                             // 106 over the same span merges with it
                                             .Note(1920, 480, 106)
                                             .Chunk()}));
    REQUIRE(midi);
    const auto parts = FromMidi(*midi);
    REQUIRE(parts.size() == 1);
    REQUIRE(parts[0].lines.size() == 2);
    CHECK(parts[0].lines[0].syllables.at(0).text == "one");
    CHECK(parts[0].lines[1].syllables.at(0).text == "two");
}

TEST_CASE("text events stand in for lyrics where a track has none, minus its [markers]") {
    const auto midi = ReadMidi(Smf(480, {Track()
                                             .Name("PART VOCALS")
                                             .Text(0, "[idle]")
                                             .Note(0, 960, 105)
                                             .Note(0, 240, 60).Text(0, "old")
                                             .Note(240, 240, 60).Text(240, "style")
                                             .Chunk()}));
    REQUIRE(midi);
    const auto parts = FromMidi(*midi);
    REQUIRE(parts.size() == 1);
    const auto& syllables = parts[0].lines.at(0).syllables;
    REQUIRE(syllables.size() == 2);
    CHECK(syllables[0].text == "old");
}

TEST_CASE("harmony parts take their phrase markers as Rock Band 3 does") {
    // HARM1 has its markers; HARM2 has none, so HARM1's stand in, and HARM3
    // follows HARM2
    const auto midi = ReadMidi(Smf(480, {Track()
                                             .Name("HARM1")
                                             .Note(0, 960, 105)
                                             .Note(0, 240, 60).Lyric(0, "high")
                                             .Chunk(),
                                         Track()
                                             .Name("HARM2")
                                             .Note(0, 240, 55).Lyric(0, "mid")
                                             .Chunk(),
                                         Track()
                                             .Name("HARM3")
                                             .Note(240, 240, 50).Lyric(240, "low")
                                             .Chunk()}));
    REQUIRE(midi);
    const auto parts = FromMidi(*midi);
    REQUIRE(parts.size() == 3);
    CHECK(parts[0].part == "harm1");
    CHECK(parts[1].part == "harm2");
    CHECK(parts[1].lines.at(0).syllables.at(0).text == "mid");
    CHECK(parts[2].part == "harm3");
    CHECK(parts[2].lines.at(0).syllables.at(0).text == "low");
}

TEST_CASE("a song without vocal tracks has no parts") {
    const auto midi = ReadMidi(Smf(480, {Track().Name("PART GUITAR").Note(0, 240, 96).Chunk()}));
    REQUIRE(midi);
    CHECK(FromMidi(*midi).empty());
}

TEST_CASE("the lyrics' JSON lists parts, lines and syllables") {
    Part part{"lead", {Line{500, 2000, {Syllable{500, 750, "Hel", true, false},
                                        Syllable{750, 1000, "\"lo\"", false, true}}}}};
    CHECK(FormatJson("song&co", {part}) ==
          "{\"shortname\":\"song&co\",\"parts\":[{\"part\":\"lead\",\"lines\":[{\"start_ms\":500,"
          "\"end_ms\":2000,\"syllables\":[{\"start_ms\":500,\"end_ms\":750,\"text\":\"Hel\","
          "\"join\":true,\"spoken\":false},{\"start_ms\":750,\"end_ms\":1000,"
          "\"text\":\"\\\"lo\\\"\",\"join\":false,\"spoken\":true}]}]}]}");
    CHECK(FormatJson("x", {}) == "{\"shortname\":\"x\",\"parts\":[]}");
}
