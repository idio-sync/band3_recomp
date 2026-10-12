// Checks band3's copy of RB3's mic effects (src/Audio/mic_fx.cpp): the EQ's
// shape, the compressor's levelling and gate, and the chain they make.

#include <doctest/doctest.h>
#include <cmath>
#include <numbers>
#include <vector>
#include "src/Audio/mic_fx.h"

using namespace band3::audio::mic_fx;

namespace {

constexpr int kRate = 48000;

std::vector<float> Sine(double hz, float amplitude, int samples) {
    std::vector<float> out(samples);
    for (int i = 0; i < samples; i++) {
        out[i] = amplitude * static_cast<float>(std::sin(2 * std::numbers::pi * hz * i / kRate));
    }
    return out;
}

// RMS of the last quarter, once filters and envelopes have settled
float SettledRms(const std::vector<float>& x) {
    double sum = 0;
    const size_t from = x.size() * 3 / 4;
    for (size_t i = from; i < x.size(); i++) sum += double(x[i]) * x[i];
    return static_cast<float>(std::sqrt(sum / (x.size() - from)));
}

float GainDb(double hz) {
    Eq eq;
    auto x = Sine(hz, 0.1f, kRate / 2);
    const float in = SettledRms(x);
    for (float& s : x) s = eq.Process(s);
    return 20.0f * std::log10(SettledRms(x) / in);
}

}

TEST_CASE("the mic EQ cuts the lows and lifts the highs as mic_fx.milo sets it") {
    // the low shelf: -10 dB well under 240 Hz
    CHECK(GainDb(50) == doctest::Approx(-10.0f).epsilon(0.1));
    // the middle is left about alone
    CHECK(std::fabs(GainDb(1500)) < 1.5f);
    // the bell lifts 5.5 kHz most; the shelf holds the top about 5 dB up
    CHECK(GainDb(5500) > 6.0f);
    CHECK(GainDb(5500) > GainDb(3000));
    CHECK(GainDb(12000) == doctest::Approx(5.0f).epsilon(0.15));
}

TEST_CASE("the mic compressor levels loud and quiet singing alike") {
    const auto level = [](float amplitude) {
        Compressor comp;
        auto x = Sine(300, amplitude, kRate * 3);
        comp.Process(x);
        return SettledRms(x);
    };
    // 26 dB apart going in, within a few dB coming out (a loud low sine's
    // flat peaks read as clipping, which turns it down a little more)
    const float loud = level(0.5f), quiet = level(0.025f);
    CHECK(loud > 0.0f);
    CHECK(20.0f * std::log10(loud / quiet) < 6.0f);
}

TEST_CASE("the mic compressor lifts quiet singing with its make-up gain") {
    Compressor comp;
    auto x = Sine(300, 0.005f, kRate * 2);  // -46 dB
    const float in = SettledRms(x);
    comp.Process(x);
    CHECK(SettledRms(x) > 5.0f * in);
}

TEST_CASE("the mic compressor comes up slowly after a gap, as the game's does") {
    Compressor comp;
    std::vector<float> silence(kRate / 2, 0.0f);
    comp.Process(silence);
    auto x = Sine(300, 0.2f, kRate);
    comp.Process(x);
    const auto rms = [&](int from_ms, int to_ms) {
        double sum = 0;
        for (int i = from_ms * 48; i < to_ms * 48; i++) sum += double(x[i]) * x[i];
        return std::sqrt(sum / ((to_ms - from_ms) * 48));
    };
    // its 500 ms recovery: quieter at first than half a second on
    CHECK(rms(0, 50) < 0.5 * rms(600, 700));
}

TEST_CASE("the chain plays silence as silence and stays finite") {
    Chain chain;
    std::vector<float> x(kRate / 10, 0.0f);
    chain.Process(x, 1.0f);
    for (float s : x) CHECK(s == 0.0f);
    auto loud = Sine(440, 1.0f, kRate);
    chain.Process(loud, 4.0f);
    for (float s : loud) REQUIRE(std::isfinite(s));
}
