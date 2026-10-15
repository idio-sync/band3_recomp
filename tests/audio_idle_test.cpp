// Checks audio_idle_bypass's decisions (src/Hooks/audio_idle.h): which guest
// samples are digital silence and which are quiet, how long an effect has to
// have stayed quiet on silent input before its blocks are skipped, and that a
// block is skipped only when every condition holds.

#include <doctest/doctest.h>
#include <bit>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>
#include "src/Hooks/audio_idle.h"

using namespace band3::audio_idle;

namespace {

// floats as the guest stores them: big-endian
std::vector<uint8_t> Guest(std::initializer_list<uint32_t> bits) {
    std::vector<uint8_t> out;
    for (uint32_t b : bits) {
        out.push_back(uint8_t(b >> 24));
        out.push_back(uint8_t(b >> 16));
        out.push_back(uint8_t(b >> 8));
        out.push_back(uint8_t(b));
    }
    return out;
}

constexpr uint32_t kPlusZero = 0x00000000;
constexpr uint32_t kMinusZero = 0x80000000;
constexpr uint32_t kSmallestSubnormal = 0x00000001;
constexpr uint32_t kOne = 0x3F800000;
constexpr uint32_t kPlusInfinity = 0x7F800000;
constexpr uint32_t kMinusInfinity = 0xFF800000;
constexpr uint32_t kNaN = 0x7FC00000;

constexpr uint32_t kBlock = 256;  // frames in an XAudio2 pass

// One hooked effect instance, as the hooks drive it: a skipped call leaves its
// count alone; a processed one counts when its input was silent and what it
// output was quiet.
struct Hooked {
    Settle settle;
    int skipped = 0;
    int processed = 0;

    void Call(bool enabled, bool mic_connected, bool silent, bool output_quiet,
              bool steady = true) {
        if (Skip(enabled, mic_connected, silent, steady, settle)) {
            skipped++;
            return;
        }
        processed++;
        settle.Observe(silent && output_quiet, kBlock);
    }
};

}

TEST_CASE("only +0 and -0 are digital silence") {
    CHECK(AllZero(Guest({kPlusZero, kMinusZero, kPlusZero}).data(), 3));
    CHECK_FALSE(AllZero(Guest({kPlusZero, kSmallestSubnormal}).data(), 2));
    CHECK_FALSE(AllZero(Guest({kSmallestSubnormal | kMinusZero}).data(), 1));
    CHECK_FALSE(AllZero(Guest({kOne}).data(), 1));
    // read in the guest's byte order: these bytes, read little-endian, are -0
    CHECK_FALSE(AllZero(Guest({0x00000080}).data(), 1));
}

TEST_CASE("quiet is at most 2^-24 either way; NaN and infinities aren't") {
    CHECK(std::bit_cast<float>(kQuietBits) == std::ldexp(1.0f, -24));
    CHECK(AllQuiet(Guest({kPlusZero, kMinusZero}).data(), 2));
    // the smallest subnormal isn't silence, but it's quiet
    CHECK(AllQuiet(Guest({kSmallestSubnormal, kSmallestSubnormal | kMinusZero}).data(), 2));
    CHECK(AllQuiet(Guest({0x33800000, 0xB3800000}).data(), 2));
    CHECK_FALSE(AllQuiet(Guest({0x33800001}).data(), 1));
    CHECK_FALSE(AllQuiet(Guest({0xB3800001}).data(), 1));
    CHECK_FALSE(AllQuiet(Guest({kOne}).data(), 1));
    CHECK_FALSE(AllQuiet(Guest({kPlusInfinity}).data(), 1));
    CHECK_FALSE(AllQuiet(Guest({kMinusInfinity}).data(), 1));
    CHECK_FALSE(AllQuiet(Guest({kNaN}).data(), 1));
    CHECK_FALSE(AllQuiet(Guest({kNaN | kMinusZero | 1}).data(), 1));
    // read in the guest's byte order: these bytes, read little-endian, are 2^-23
    CHECK(AllQuiet(Guest({0x00000034}).data(), 1));
}

TEST_CASE("the count is the number of floats, odd counts included") {
    const auto loud_fifth = Guest({kPlusZero, kMinusZero, kPlusZero, kPlusZero, kOne});
    CHECK_FALSE(AllZero(loud_fifth.data(), 5));
    CHECK_FALSE(AllQuiet(loud_fifth.data(), 5));
    CHECK(AllZero(loud_fifth.data(), 4));
    CHECK(AllQuiet(loud_fifth.data(), 4));
    const auto three = Guest({kPlusZero, kPlusZero, kOne});
    CHECK_FALSE(AllZero(three.data(), 3));
    CHECK(AllZero(three.data(), 2));
    // nothing to read
    CHECK(AllZero(three.data(), 0));
    CHECK(AllQuiet(three.data(), 0));
}

TEST_CASE("an effect settles after exactly 5 s of quiet frames") {
    CHECK(kSettleFrames == 240000);
    Settle s;
    CHECK_FALSE(s.Settled());
    s.Observe(true, kSettleFrames - 1);
    CHECK_FALSE(s.Settled());
    s.Observe(true, 1);
    CHECK(s.Settled());

    // in 256-frame blocks, the 938th
    Settle blocks;
    for (int i = 0; i < 937; i++) blocks.Observe(true, kBlock);
    CHECK_FALSE(blocks.Settled());
    blocks.Observe(true, kBlock);
    CHECK(blocks.Settled());
}

TEST_CASE("the quiet count saturates") {
    Settle s;
    s.Observe(true, kSettleFrames);
    s.Observe(true, kSettleFrames);
    CHECK(s.quiet_frames == kSettleFrames);
    s.Observe(true, UINT32_MAX);
    CHECK(s.quiet_frames == kSettleFrames);
    CHECK(s.Settled());
}

TEST_CASE("one block that isn't quiet starts the count over") {
    Settle s;
    s.Observe(true, kSettleFrames);
    s.Observe(false, kBlock);
    CHECK(s.quiet_frames == 0);
    CHECK_FALSE(s.Settled());
    s.Observe(true, kSettleFrames - kBlock);
    CHECK_FALSE(s.Settled());
}

TEST_CASE("a block is skipped only when every condition holds") {
    Settle settled;
    settled.Observe(true, kSettleFrames);
    const Settle unsettled;
    CHECK(Skip(true, false, true, true, settled));
    CHECK_FALSE(Skip(false, false, true, true, settled));  // the bypass is off
    CHECK_FALSE(Skip(true, true, true, true, settled));    // a microphone is connected
    CHECK_FALSE(Skip(true, false, false, true, settled));  // the input isn't silent
    CHECK_FALSE(Skip(true, false, true, false, settled));  // the effect isn't steady
    CHECK_FALSE(Skip(true, false, true, true, unsettled)); // not quiet for 5 s yet

    int skips = 0;
    for (int bits = 0; bits < 32; bits++) {
        skips += Skip(bits & 1, bits & 2, bits & 4, bits & 8, bits & 16 ? settled : unsettled);
    }
    CHECK(skips == 1);
}

TEST_CASE("skipped blocks leave the count where it was; the first sound starts it over") {
    Hooked h;
    for (int i = 0; i < 938; i++) h.Call(true, false, true, true);
    CHECK(h.skipped == 0);
    CHECK(h.settle.Settled());
    for (int i = 0; i < 1000; i++) h.Call(true, false, true, true);
    CHECK(h.skipped == 1000);
    CHECK(h.settle.quiet_frames == kSettleFrames);

    // a sound reaches it: it runs, and must settle again before the next skip
    h.Call(true, false, false, false);
    CHECK(h.skipped == 1000);
    CHECK(h.settle.quiet_frames == 0);
    for (int i = 0; i < 937; i++) h.Call(true, false, true, true);
    CHECK(h.skipped == 1000);
    h.Call(true, false, true, true);
    h.Call(true, false, true, true);
    CHECK(h.skipped == 1001);
}

TEST_CASE("a tail louder than 2^-24 keeps the effect running, then 5 s more") {
    Hooked h;
    // silent input, but the effect still rings for 3 s
    for (int i = 0; i < 3 * 48000 / int(kBlock); i++) h.Call(true, false, true, false);
    CHECK(h.settle.quiet_frames == 0);
    for (int i = 0; i < 938; i++) h.Call(true, false, true, true);
    CHECK(h.skipped == 0);
    h.Call(true, false, true, true);
    CHECK(h.skipped == 1);
}

TEST_CASE("with the bypass off or a microphone connected the count goes on") {
    Hooked h;
    for (int i = 0; i < 938; i++) h.Call(false, false, true, true);
    CHECK(h.skipped == 0);
    CHECK(h.settle.Settled());
    // turned on: it skips at once, the effect having stayed quiet all along
    h.Call(true, false, true, true);
    CHECK(h.skipped == 1);
    for (int i = 0; i < 10; i++) h.Call(true, true, true, true);
    CHECK(h.skipped == 1);
    CHECK(h.settle.Settled());
    h.Call(true, false, true, true);
    CHECK(h.skipped == 2);
}

TEST_CASE("an effect that isn't steady runs, and its quiet blocks count") {
    Hooked h;
    for (int i = 0; i < 1000; i++) h.Call(true, false, true, true, false);
    CHECK(h.skipped == 0);
    CHECK(h.settle.Settled());
    h.Call(true, false, true, true, true);
    CHECK(h.skipped == 1);
}

TEST_CASE("the reverb is steady enabled, with no new parameters, past its fade-in and first pass") {
    CHECK(ReverbSteady(true, 0, 1, 1));
    CHECK_FALSE(ReverbSteady(false, 0, 1, 1));       // disabled, or fading out
    CHECK_FALSE(ReverbSteady(true, 0x40001000, 1, 1)); // new parameters to apply
    CHECK_FALSE(ReverbSteady(true, 0, 0, 1));        // fading in
    CHECK_FALSE(ReverbSteady(true, 0, 1, 0));        // never processed
    // the guest compares the words with 0
    CHECK(ReverbSteady(true, 0, 0x100, 0xFFFFFFFF));
}
