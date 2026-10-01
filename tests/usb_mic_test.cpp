// Checks USB microphones' host side (src/Audio/usb_mic.cpp): the device list
// setting, the test tone's format and rate, and when a slot connects, feeds and
// disconnects.

#include <doctest/doctest.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>
#include "src/Audio/usb_mic.h"

using namespace band3::audio::usb_mic;
using namespace std::chrono_literals;

namespace {

const Clock::time_point t0{};

int16_t SampleAt(const std::vector<uint8_t>& pcm, size_t i) {
    return static_cast<int16_t>(static_cast<uint16_t>(pcm[i * 2] << 8 | pcm[i * 2 + 1]));
}

// upward zero crossings per second
double MeasurePitch(const std::vector<uint8_t>& pcm, size_t samples) {
    int crossings = 0;
    for (size_t i = 1; i < samples; i++) {
        if (SampleAt(pcm, i - 1) < 0 && SampleAt(pcm, i) >= 0) crossings++;
    }
    return crossings * static_cast<double>(kSampleRate) / samples;
}

}

TEST_CASE("the device list keeps slot order and blank slots") {
    CHECK(ParseDeviceList("").empty());
    CHECK(ParseDeviceList("   ").empty());
    CHECK(ParseDeviceList("Yeti") == std::vector<std::string>{"Yeti"});
    CHECK(ParseDeviceList(" Yeti , USB Audio ") == std::vector<std::string>{"Yeti", "USB Audio"});
    CHECK(ParseDeviceList(",Yeti") == std::vector<std::string>{"", "Yeti"});
}

TEST_CASE("device names match case-insensitively on part of the name") {
    CHECK(NameMatches("Microphone (Yeti Stereo Microphone)", "yeti"));
    CHECK(NameMatches("USB Audio Device", "USB AUDIO"));
    CHECK_FALSE(NameMatches("Realtek Line In", "yeti"));
    CHECK_FALSE(NameMatches("anything", ""));
}

TEST_CASE("the test tone is 16 kHz big-endian PCM at its pitch") {
    ToneSource tone(440.0, t0, 8000);
    std::vector<uint8_t> pcm(kSampleRate * 2);

    SUBCASE("produces exactly the samples that have elapsed") {
        std::array<uint8_t, kMaxChunk> buf{};
        CHECK(tone.Read(buf, t0) == 0);
        CHECK(tone.Read(buf, t0 + 10ms) == 160 * 2);
        CHECK(tone.Read(buf, t0 + 10ms) == 0);
        CHECK(tone.Read(buf, t0 + 20ms) == 160 * 2);
    }

    SUBCASE("big-endian, starting at zero and rising") {
        std::array<uint8_t, 8> buf{};
        REQUIRE(tone.Read(buf, t0 + 1ms) == 8);
        CHECK(buf[0] == 0);
        CHECK(buf[1] == 0);
        const int16_t second = static_cast<int16_t>(buf[2] << 8 | buf[3]);
        CHECK(second == static_cast<int16_t>(std::lround(8000 * std::sin(2 * 3.141592653589793 * 440 / kSampleRate))));
    }

    SUBCASE("one second holds the pitch") {
        size_t filled = 0;
        for (auto t = t0 + 10ms; filled < pcm.size(); t += 10ms) {
            filled += tone.Read(std::span(pcm).subspan(filled), t);
        }
        CHECK(MeasurePitch(pcm, kSampleRate) == doctest::Approx(440.0).epsilon(0.01));
    }

    SUBCASE("stays within its amplitude") {
        size_t filled = 0;
        for (auto t = t0 + 10ms; filled < pcm.size(); t += 10ms) {
            filled += tone.Read(std::span(pcm).subspan(filled), t);
        }
        for (size_t i = 0; i < kSampleRate; i++) {
            CHECK(std::abs(SampleAt(pcm, i)) <= 8000);
        }
    }
}

TEST_CASE("a small buffer takes what fits and the rest follows") {
    ToneSource tone(440.0, t0);
    std::array<uint8_t, 100> small{};
    CHECK(tone.Read(small, t0 + 10ms) == 100);
    std::array<uint8_t, kMaxChunk> big{};
    CHECK(tone.Read(big, t0 + 10ms) == 160 * 2 - 100);
}

TEST_CASE("after a stall the tone skips ahead instead of building a backlog") {
    ToneSource tone(440.0, t0);
    std::vector<uint8_t> buf(kSampleRate * 2 * 2);
    // two seconds without reading
    CHECK(tone.Read(buf, t0 + 2s) == kMaxBacklogBytes);
    CHECK(tone.Read(buf, t0 + 2s + 10ms) == 160 * 2);
}

TEST_CASE("a slot connects once something feeds it") {
    Slot slot;
    CHECK(slot.Next(false, t0) == Action::kWait);
    CHECK(slot.Next(true, t0) == Action::kConnect);
    slot.Connected(true, t0);
    CHECK(slot.connected());
    CHECK(slot.Next(true, t0) == Action::kFeed);
}

TEST_CASE("a refused connection is retried after a pause") {
    Slot slot;
    slot.Connected(false, t0);
    CHECK_FALSE(slot.connected());
    CHECK(slot.Next(true, t0 + Slot::kConnectRetry - 1ms) == Action::kWait);
    CHECK(slot.Next(true, t0 + Slot::kConnectRetry) == Action::kConnect);
}

TEST_CASE("a slot disconnects when its microphone goes, and reconnects when it's back") {
    Slot slot;
    slot.Connected(true, t0);
    CHECK(slot.Next(false, t0 + 1s) == Action::kDisconnect);
    slot.Disconnected();
    CHECK(slot.Next(false, t0 + 1s) == Action::kWait);
    CHECK(slot.Next(true, t0 + 1s) == Action::kConnect);
}

TEST_CASE("the gain a generic USB mic gets in play leaves its level alone") {
    CHECK(GainRatio(kNormalGain) == 1.0f);
}

TEST_CASE("gain follows the game's 0-1 setting across an RB microphone's range") {
    // tambourine sections turn the gain to 0: quieter, never silent
    CHECK(GainRatio(0.0f) == doctest::Approx(kMinGainRatio));
    CHECK(GainRatio(1.0f) > 2.0f);
    CHECK(GainRatio(0.1f) < GainRatio(0.2f));
    CHECK(GainRatio(0.5f) < GainRatio(0.6f));
    // MicXbox::SetGain clamps to 0-1
    CHECK(GainRatio(-1.0f) == GainRatio(0.0f));
    CHECK(GainRatio(2.0f) == GainRatio(1.0f));
}

TEST_CASE("gain scales big-endian samples and clips at full scale") {
    const auto pcm = [](std::initializer_list<int16_t> samples) {
        std::vector<uint8_t> out;
        for (int16_t s : samples) {
            const auto bits = static_cast<uint16_t>(s);
            out.push_back(static_cast<uint8_t>(bits >> 8));
            out.push_back(static_cast<uint8_t>(bits));
        }
        return out;
    };

    SUBCASE("unity leaves the bytes as they are") {
        auto buf = pcm({1234, -1234, 32767, -32768});
        const auto before = buf;
        ApplyGain(buf, 1.0f);
        CHECK(buf == before);
    }

    SUBCASE("halving") {
        auto buf = pcm({1000, -1000, 3});
        ApplyGain(buf, 0.5f);
        CHECK(SampleAt(buf, 0) == 500);
        CHECK(SampleAt(buf, 1) == -500);
        CHECK(SampleAt(buf, 2) == 2);
    }

    SUBCASE("boosting clips instead of wrapping") {
        auto buf = pcm({20000, -20000, 100});
        ApplyGain(buf, 2.0f);
        CHECK(SampleAt(buf, 0) == 32767);
        CHECK(SampleAt(buf, 1) == -32768);
        CHECK(SampleAt(buf, 2) == 200);
    }
}
