// Checks the HID capture format (src/Input/hid_capture.cpp), and replays every
// capture in tests/captures through its instrument's translator (the Xbox One
// ones' too, which the Lab saves the same way). Captures saved from the
// Instrument Lab can be dropped into that folder to keep them working.

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "rb3_reference.h"
#include "src/Input/hid_capture.h"
#include "src/Input/hid_instrument_types.h"

using namespace band3::input;
using namespace rb3;

namespace {

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

}

TEST_CASE("a capture survives formatting and parsing") {
    HidCapture capture;
    capture.device = "PS3 Rock Band guitar";
    capture.vendor = 0x12BA;
    capture.product = 0x0200;
    capture.release = 0x0313;
    capture.reports = {{0, {0x00, 0x01, 0x08}}, {17, {0xff, 0x7f}}, {40, {}}};

    const auto parsed = ParseHidCapture(FormatHidCapture(capture));
    REQUIRE(parsed.has_value());
    CHECK(parsed->device == capture.device);
    CHECK(parsed->vendor == 0x12BA);
    CHECK(parsed->product == 0x0200);
    CHECK(parsed->release == 0x0313);
    REQUIRE(parsed->reports.size() == 3);
    CHECK(parsed->reports[1].ms == 17);
    CHECK(parsed->reports[1].bytes == std::vector<uint8_t>{0xff, 0x7f});
    CHECK(parsed->reports[2].bytes.empty());
}

TEST_CASE("captures allow comments, CRLF and extra header lines") {
    const auto parsed = ParseHidCapture(
        "# a note\r\ndevice: x\r\nusb: 1bad:3110\r\nnotes: hit the red pad\r\n---\r\n"
        "# between reports\r\n5 01 02\r\n");
    REQUIRE(parsed.has_value());
    CHECK(parsed->vendor == 0x1BAD);
    CHECK(parsed->product == 0x3110);
    REQUIRE(parsed->reports.size() == 1);
    CHECK(parsed->reports[0].bytes == std::vector<uint8_t>{0x01, 0x02});
}

TEST_CASE("text that isn't a capture is rejected") {
    CHECK_FALSE(ParseHidCapture("").has_value());
    CHECK_FALSE(ParseHidCapture("usb: 12BA:0210\n0 00\n").has_value());      // no ---
    CHECK_FALSE(ParseHidCapture("device: x\n---\n0 00\n").has_value());      // no usb
    CHECK_FALSE(ParseHidCapture("usb: 12BA:0210\n---\n0 zz\n").has_value());  // bad byte
    CHECK_FALSE(ParseHidCapture("usb: 12BA:0210\n---\nx 00\n").has_value());  // bad time
}

TEST_CASE("every saved capture replays through the translator") {
    int replayed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(BAND3_CAPTURE_DIR)) {
        if (entry.path().extension() != ".txt") continue;
        CAPTURE(entry.path().filename().string());
        const auto capture = ParseHidCapture(ReadFile(entry.path()));
        REQUIRE(capture.has_value());
        const auto instrument =
            IdentifyInstrument(capture->vendor, capture->product, capture->release);
        REQUIRE(instrument.has_value());

        HidInstrumentTranslator translator(*instrument);
        for (const auto& report : capture->reports) {
            CAPTURE(report.ms);
            CHECK(translator.Translate(report.bytes).has_value());
        }
        replayed++;
    }
    CHECK(replayed > 0);
}

TEST_CASE("the synthetic drum capture hits the red pad, then the yellow cymbal") {
    const auto capture = ParseHidCapture(
        ReadFile(std::filesystem::path(BAND3_CAPTURE_DIR) / "synthetic-ps3-drums.txt"));
    REQUIRE(capture.has_value());
    REQUIRE(capture->reports.size() == 5);

    HidInstrumentTranslator translator(HidInstrumentType::kPs3Drums);
    std::vector<Gamepad360> states;
    for (const auto& report : capture->reports) states.push_back(*translator.Translate(report.bytes));

    CHECK(states[0].buttons == 0);
    CHECK(states[1].buttons == (kB | kRightThumb));
    CHECK(DrumSlot(JoypadButtonFor(SlotButtons(states[1].buttons))) == 1);  // red
    CHECK(u16(states[1].thumb_lx) == 0x20 * 0x7FFF / 0xFF);
    CHECK(states[2].buttons == 0);
    CHECK(states[3].buttons == (kY | kRightShoulder | kDpadUp));
    CHECK(u16(states[3].thumb_ly) == (0x8000 | 0x10 * 0x7FFF / 0xFF));
    CHECK(states[4].buttons == 0);
}

TEST_CASE("the synthetic Xbox One drum capture hits the red pad, then the yellow cymbal") {
    const auto capture = ParseHidCapture(
        ReadFile(std::filesystem::path(BAND3_CAPTURE_DIR) / "synthetic-xbox-one-drums.txt"));
    REQUIRE(capture.has_value());
    REQUIRE(capture->reports.size() == 6);
    REQUIRE(IdentifyInstrument(capture->vendor, capture->product, capture->release) ==
            HidInstrumentType::kXboxOneDrums);

    HidInstrumentTranslator translator(HidInstrumentType::kXboxOneDrums);
    std::vector<Gamepad360> states;
    for (const auto& report : capture->reports) states.push_back(*translator.Translate(report.bytes));

    CHECK(states[0].buttons == 0);
    CHECK(states[1].buttons == (kB | kRightThumb));
    CHECK(DrumSlot(JoypadButtonFor(SlotButtons(states[1].buttons))) == 1);  // red
    CHECK(states[2].buttons == states[1].buttons);
    CHECK(states[3].buttons == 0);
    CHECK(states[4].buttons == (kY | kRightShoulder | kDpadUp));
    CHECK(states[5].buttons == 0);
}
