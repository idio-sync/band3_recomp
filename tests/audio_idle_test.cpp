// Checks audio_idle_bypass's decisions (src/Hooks/audio_idle.h): which guest
// samples are digital silence and which are quiet, how long an effect has to
// have stayed quiet on silent input before its blocks are skipped, and that a
// block is skipped only when every condition holds.

#include <doctest/doctest.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <span>
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

// CompressionEffect's state words on silent input, as a model: its gain
// envelope halves each block until it reaches its fixed point, 0, as the gated
// mic compressor's decays and sticks; Reset puts it back to 1.0.
struct ModelCompressor {
    static constexpr uint32_t kHalve = 0x00800000;  // one off a normal float's exponent
    std::array<uint32_t, 3> words = {0, 0, kOne};    // gate floor, DC block, envelope

    void ProcessSilence() { words[2] = words[2] >= kHalve ? words[2] - kHalve : 0; }
    void Reset() { words[2] = kOne; }
};

// ... and its hook: steady only in the state its last processed block left
struct HookedCompressor {
    ModelCompressor compressor;
    Settle settle;
    std::array<uint32_t, 3> left{};
    int skipped = 0;

    // a silent block, the bypass on, no microphone
    void Call() {
        const auto now = compressor.words;
        if (Skip(true, false, true, StateAsLeft(now, left), settle)) {
            skipped++;
            return;
        }
        compressor.ProcessSilence();
        left = compressor.words;
        settle.Observe(left == now, kBlock);
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

TEST_CASE("the reverb's kernel runs while it's enabled, and on the pass that fades it out") {
    CHECK(ReverbKernelRuns(true, 1, 1));
    CHECK(ReverbKernelRuns(true, 0, 1));          // fading in
    CHECK(ReverbKernelRuns(true, 0, 0));          // its first pass
    CHECK(ReverbKernelRuns(false, 1, 1));         // fading out
    CHECK_FALSE(ReverbKernelRuns(false, 0, 1));   // disabled: copied through, or flagged SILENT
    CHECK_FALSE(ReverbKernelRuns(false, 1, 0));   // never processed
    CHECK_FALSE(ReverbKernelRuns(false, 0, 0));
    // the guest compares the words with 0
    CHECK(ReverbKernelRuns(false, 0x100, 0xFFFFFFFF));
}

TEST_CASE("the reverb's channel counts are ones its kernels take") {
    CHECK(ReverbChannelsOk(2, 2));
    CHECK(ReverbChannelsOk(1, 6));
    CHECK(ReverbChannelsOk(6, 1));
    CHECK_FALSE(ReverbChannelsOk(0, 2));
    CHECK_FALSE(ReverbChannelsOk(2, 0));
    CHECK_FALSE(ReverbChannelsOk(7, 2));
    CHECK_FALSE(ReverbChannelsOk(2, 7));
    // more than the 16-bit counts LockForProcess stores
    CHECK_FALSE(ReverbChannelsOk(0x00010002, 2));
    CHECK_FALSE(ReverbChannelsOk(2, 0xFFFFFFFF));
}

TEST_CASE("the game's microphone list reads as empty, as up to four entries, or as unreadable") {
    // not made yet (before Synth360::Init), or none
    CHECK(MicListShape(0, 0) == MicList::kEmpty);
    CHECK(MicListShape(0x40001000, 0x40001000) == MicList::kEmpty);
    CHECK(MicListShape(0x40001000, 0x40001004) == MicList::kEntries);
    CHECK(MicListShape(0x40001000, 0x40001010) == MicList::kEntries);
    CHECK(MicListShape(0x40001000, 0x40001014) == MicList::kUnreadable);  // five
    CHECK(MicListShape(0, 0x10) == MicList::kUnreadable);                  // no storage
    CHECK(MicListShape(0x40001010, 0x40001000) == MicList::kUnreadable);  // backwards
    CHECK(MicListShape(0x40001000, 0x40001006) == MicList::kUnreadable);  // not whole pointers
}

TEST_CASE("a compressor is steady only in the state its last processed block left") {
    const std::array<uint32_t, 3> left = {0x2E000000, 0x3F800000, 0x00000000};
    CHECK(StateAsLeft(left, left));
    for (size_t i = 0; i < left.size(); i++) {
        auto changed = left;
        changed[i] ^= 1;
        CHECK_FALSE(StateAsLeft(changed, left));
    }
    // bitwise: -0 isn't the +0 it left
    auto minus_zero = left;
    minus_zero[2] = kMinusZero;
    CHECK_FALSE(StateAsLeft(minus_zero, left));
    // effects with no such words
    CHECK(StateAsLeft(std::span<const uint32_t>{}, std::span<const uint32_t>{}));
}

TEST_CASE("a compressor reset, or made anew at its address, is processed until it settles again") {
    HookedCompressor h;
    // 127 blocks bring its envelope from 1.0 to 0, then 938 leave it there
    for (int i = 0; i < 127 + 938; i++) h.Call();
    CHECK(h.skipped == 0);
    CHECK(h.settle.Settled());
    h.Call();
    CHECK(h.skipped == 1);

    // its send's bypass turned on (StandardEffect::DoProcess calls Reset) and
    // off again: its envelope is 1.0 again, so it isn't skipped from where it
    // was settled, and processing moves its state, which starts the count over
    h.compressor.Reset();
    h.Call();
    CHECK(h.skipped == 1);
    CHECK(h.settle.quiet_frames == 0);
    for (int i = 0; i < 126 + 938; i++) h.Call();
    CHECK(h.skipped == 1);
    h.Call();
    CHECK(h.skipped == 2);
}
