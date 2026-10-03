// Checks the launcher's device lists' pure parts (src/Launcher/device_names.cpp
// and mic_level.cpp): which entry a saved setting selects, the names saved for
// MIDI ports and mic slots, the order monitors are numbered in, and the mic
// meter's levels.

#include <doctest/doctest.h>
#include <chrono>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>
#include "src/Audio/usb_mic.h"
#include "src/Launcher/device_names.h"
#include "src/Launcher/launcher_settings.h"
#include "src/Launcher/mic_level.h"

using namespace band3::launcher;
using namespace std::chrono_literals;

TEST_CASE("a saved device selects the first entry containing it, ignoring case") {
    const std::vector<std::string> mics = {"Microphone (USB PnP Sound Device)",
                                           "Microphone (Yeti Stereo Microphone)",
                                           "Headset Microphone (USB PnP Sound Device)"};
    CHECK(FindSavedDevice(mics, "usb pnp") == 0u);
    CHECK(FindSavedDevice(mics, "YETI") == 1u);
    CHECK(FindSavedDevice(mics, "Headset") == 2u);
    CHECK(FindSavedDevice(mics, mics[2]) == 2u);
    CHECK(FindSavedDevice(mics, " Yeti ") == 1u);
    // nothing to select: shown as "<saved> (not connected)", or the default
    CHECK_FALSE(FindSavedDevice(mics, "SingStar").has_value());
    CHECK_FALSE(FindSavedDevice(mics, "").has_value());
    CHECK_FALSE(FindSavedDevice({}, "Yeti").has_value());
}

TEST_CASE("the MIDI port picked is the one the driver opens") {
    const std::vector<std::string> linux_ports = {"Midi Through:Midi Through Port-0 14:0",
                                                  "TD-17:TD-17 MIDI 1 20:0"};
    // no port named: the first that isn't the loopback
    CHECK(FindMidiPort(linux_ports, "") == 1u);
    CHECK(FindMidiPort(linux_ports, "td-17") == 1u);
    CHECK(FindMidiPort(linux_ports, "through") == 0u);
    CHECK_FALSE(FindMidiPort(linux_ports, "Alesis").has_value());
    CHECK_FALSE(FindMidiPort({}, "").has_value());
}

TEST_CASE("WinMM's port index comes off MIDI port names") {
    // MidiInWinMM::getPortName: the device's name, a space and the port number
    CHECK(StripMidiPortIndex("TD-17 0", 0) == "TD-17");
    CHECK(StripMidiPortIndex("USB MIDI Interface 3", 3) == "USB MIDI Interface");
    CHECK(StripMidiPortIndex("Alesis Nitro 12", 12) == "Alesis Nitro");
    // only its own index, so a model number stays
    CHECK(StripMidiPortIndex("Roland TD 1", 0) == "Roland TD 1");
    CHECK(StripMidiPortIndex("Kit 21", 1) == "Kit 21");
    CHECK(StripMidiPortIndex("Kit 21 2", 2) == "Kit 21");
    CHECK(StripMidiPortIndex(" 0", 0) == " 0");

    // the saved name still finds the port: the driver matches part of a name
    const std::vector<std::string> ports = {"Microsoft GS Wavetable Synth 0", "TD-17 1"};
    const std::string saved = StripMidiPortIndex(ports[1], 1);
    CHECK(FindMidiPort(ports, saved) == 1u);
    CHECK(band3::audio::usb_mic::NameMatches(ports[1], saved));
}

TEST_CASE("a mic slot saves the device name, or its longest part without a comma") {
    CHECK(MicSlotValue("Microphone (Yeti Stereo Microphone)") ==
          "Microphone (Yeti Stereo Microphone)");
    CHECK(MicSlotValue("Mic, USB (Audio-Technica AT2020USB+)") == "USB (Audio-Technica AT2020USB+)");
    const std::string value = MicSlotValue("Mic, USB (Audio-Technica AT2020USB+)");
    CHECK(band3::audio::usb_mic::NameMatches("Mic, USB (Audio-Technica AT2020USB+)", value));
}

TEST_CASE("devices chosen for mic slots join into usb_mic_devices and split back") {
    using band3::audio::usb_mic::ParseDeviceList;
    const std::vector<std::string> devices = {"Microphone (USB PnP Sound Device)", "",
                                              "Mic, USB (Audio-Technica AT2020USB+)", ""};
    std::vector<std::string> slots;
    for (const auto& device : devices) slots.push_back(MicSlotValue(device));
    const auto parsed = ParseDeviceList(JoinMicSlots(slots));
    REQUIRE(parsed.size() == 3);
    CHECK(parsed[0] == devices[0]);
    CHECK(parsed[1] == "");
    CHECK(band3::audio::usb_mic::NameMatches(devices[2], parsed[2]));
}

TEST_CASE("monitors number from the primary one, then in enumeration order") {
    CHECK(SdlDisplayOrder({}).empty());
    CHECK(SdlDisplayOrder({true}) == std::vector<size_t>{0});
    CHECK(SdlDisplayOrder({false, true}) == std::vector<size_t>{1, 0});
    CHECK(SdlDisplayOrder({false, false, true}) == std::vector<size_t>{2, 0, 1});
    CHECK(SdlDisplayOrder({true, false, false}) == std::vector<size_t>{0, 1, 2});
    // no primary (it can't happen, but the rest keep their order)
    CHECK(SdlDisplayOrder({false, false}) == std::vector<size_t>{0, 1});
}

TEST_CASE("display modes are listed once each, largest and fastest first") {
    const std::vector<DisplayMode> modes = SortDisplayModes({
        {1280, 720, 60}, {1920, 1080, 60}, {1920, 1080, 144}, {1280, 720, 60},
        {1920, 1200, 60}, {800, 600, 0}, {0, 0, 60},
    });
    const std::vector<DisplayMode> want = {
        {1920, 1200, 60}, {1920, 1080, 144}, {1920, 1080, 60}, {1280, 720, 60}, {800, 600, 0},
    };
    CHECK(modes == want);
}

namespace {

const MicLevel::Clock::time_point t0 = MicLevel::Clock::time_point{} + 100s;

// `seconds` of a sine at `amplitude`, in 10 ms blocks, from `start`
MicLevel::Clock::time_point FeedSine(MicLevel& level, float amplitude, double seconds,
                                     MicLevel::Clock::time_point start) {
    constexpr int kRate = 16000;
    constexpr int kBlock = kRate / 100;
    std::vector<float> block(kBlock);
    auto now = start;
    size_t n = 0;
    for (int b = 0; b < static_cast<int>(seconds * 100); b++) {
        for (auto& s : block) {
            s = amplitude * static_cast<float>(std::sin(2 * std::numbers::pi * 440 * n++ / kRate));
        }
        now += 10ms;
        level.Feed(block, kRate, now);
    }
    return now;
}

}

TEST_CASE("a mic level reads a tone's peak and RMS") {
    MicLevel level;
    CHECK(level.Read(t0).peak == 0.0f);
    CHECK(level.Read(t0).rms == 0.0f);

    const auto end = FeedSine(level, 0.5f, 2.0, t0);
    const MicLevel::Reading r = level.Read(end);
    CHECK(r.peak == doctest::Approx(0.5).epsilon(0.01));
    CHECK(r.rms == doctest::Approx(0.5 / std::sqrt(2.0)).epsilon(0.02));
}

TEST_CASE("a mic level holds its peak, then falls away in silence") {
    MicLevel level;
    const auto end = FeedSine(level, 0.8f, 1.0, t0);
    // held
    CHECK(level.Read(end + 400ms).peak == doctest::Approx(0.8).epsilon(0.01));
    // then falling, to about a third a time constant after the hold
    CHECK(level.Read(end + 800ms).peak == doctest::Approx(0.8 * std::exp(-1.0)).epsilon(0.05));
    CHECK(level.Read(end + 5s).peak < 0.001f);
    // the RMS keeps averaging in silence
    CHECK(level.Read(end + 300ms).rms < level.Read(end).rms * 0.7f);
    CHECK(level.Read(end + 5s).rms < 0.001f);

    // silence fed in falls the same way, and a louder sound takes over at once
    MicLevel fed;
    auto now = FeedSine(fed, 0.8f, 1.0, t0);
    now = FeedSine(fed, 0.0f, 0.4, now);
    CHECK(fed.Read(now).peak == doctest::Approx(0.8).epsilon(0.01));
    now = FeedSine(fed, 0.0f, 1.0, now);
    CHECK(fed.Read(now).peak < 0.4f);
    now = FeedSine(fed, 0.9f, 0.1, now);
    CHECK(fed.Read(now).peak == doctest::Approx(0.9).epsilon(0.01));
}

TEST_CASE("mic levels in dB below full scale") {
    CHECK(MicLevel::ToDecibels(1.0f) == doctest::Approx(0.0));
    CHECK(MicLevel::ToDecibels(0.5f) == doctest::Approx(-6.02).epsilon(0.01));
    CHECK(MicLevel::ToDecibels(0.0f) == -60.0f);
    CHECK(MicLevel::ToDecibels(0.00001f) == -60.0f);
    CHECK(MicLevel::ToDecibels(0.001f, -90.0f) == doctest::Approx(-60.0));
    CHECK(MicLevel::ToDecibels(2.0f) == doctest::Approx(0.0));
}
