#pragma once
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

// audio_idle_bypass (src/Hooks/audio_idle.cpp): the game's audio effects
// skipped while all they do is process silence.
//
// Every effect RB3 makes runs every 5.33 ms audio pass whether anything reaches
// it or not: ATG's CSampleXAPOBase zero-fills a silent input and processes it
// anyway, and XAudio2's reverb, while it's enabled, fills one with 1.0e-18 and
// does the same. The mic effects chain (sfx/mic_fx.milo: pitch correction, EQ,
// compressor, echo) loads at boot whether or not a microphone exists, so with
// none it processes silence all session; the reverb and the other effects do
// whenever nothing reaches them, as while a song is paused.
//
// A hooked effect's block is skipped only when the bypass is on, the game
// counts no connected microphone, the block's input is digital silence, the
// effect is in its steady state, and this effect instance has, over its last
// 5 s of silent input, output nothing louder than 2^-24 (-144 dBFS, below
// 24-bit resolution) on any sample. A skipped block's output is exact zeros,
// within 2^-24 of what the effect would have output, and the effect's state is
// left where it was. A sound that reaches the effect runs it again, and the
// count starts over: a tail that stays louder than 2^-24 keeps it running.

namespace band3::audio_idle {

// guest floats are big-endian; the host reading them is little-endian
static_assert(std::endian::native == std::endian::little);

// every voice RB3 gives effects runs at 48 kHz (FxSend360, Synth360::PreInit)
constexpr uint32_t kSampleRate = 48000;
// longer than any of these effects keeps a sound in its output: DelayEffect's
// ring is 2 s, the reverb's pre-delays are under 300 ms and its late lines
// under a second, the Synapse resynthesizes from 400 ms of input
constexpr uint32_t kSettleFrames = 5 * kSampleRate;
// 2^-24: below 24-bit resolution. The most gain RB3's mix adds after an
// effect (sends up to +20 dB, a voice up to x4) leaves it under -112 dBFS.
constexpr uint32_t kQuietBits = 0x33800000;

namespace detail {

// a guest float's bits, less its sign
inline uint32_t Magnitude(const uint8_t* p) {
    uint32_t raw;
    std::memcpy(&raw, p, sizeof raw);
    return std::byteswap(raw) & 0x7FFFFFFFu;
}

}

// `count` big-endian floats at `p` are all +0 or -0
inline bool AllZero(const uint8_t* p, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (detail::Magnitude(p + 4 * i) != 0) return false;
    }
    return true;
}

// ... all have |x| <= 2^-24; NaN and infinities aren't quiet
inline bool AllQuiet(const uint8_t* p, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (detail::Magnitude(p + 4 * i) > kQuietBits) return false;
    }
    return true;
}

// How long one effect instance has been processing silent input and staying
// quiet, in frames. A processed block that isn't both starts it over; skipped
// blocks leave it as it is.
struct Settle {
    uint32_t quiet_frames = 0;

    constexpr bool Settled() const { return quiet_frames >= kSettleFrames; }

    constexpr void Observe(bool quiet, uint32_t frames) {
        quiet_frames = quiet ? uint32_t(std::min<uint64_t>(uint64_t(quiet_frames) + frames,
                                                           kSettleFrames))
                             : 0;
    }
};

// whether a hooked effect skips this block
constexpr bool Skip(bool enabled, bool mic_connected, bool input_silent, bool steady,
                    const Settle& s) {
    return enabled && !mic_connected && input_silent && steady && s.Settled();
}

// An effect whose Process changes words besides its samples (CompressionEffect:
// its gate floor, DC block and gain envelope) is steady only while those words
// are what its last processed block left. Anything else that changed them, a
// Reset when its send's bypass turns on or a new instance at the same address,
// has it processed until it settles again.
inline bool StateAsLeft(std::span<const uint32_t> now, std::span<const uint32_t> left) {
    return std::equal(now.begin(), now.end(), left.begin(), left.end());
}

// LEAPFX::CAudioReverb::Process's steady state: enabled, no new parameters
// waiting for it, past the pass that fades it in after enabling, and past its
// first pass. The guest compares each word with 0.
constexpr bool ReverbSteady(bool is_enabled, uint32_t new_parameters, uint32_t was_enabled,
                            uint32_t processed) {
    return is_enabled && new_parameters == 0 && was_enabled != 0 && processed != 0;
}

// Whether CAudioReverb::Process runs its kernel: while it's enabled, and on the
// pass after it's disabled, which fades it out. Disabled after that, it flags a
// SILENT input's output SILENT or copies a VALID one through, and its state
// holds still.
constexpr bool ReverbKernelRuns(bool is_enabled, uint32_t was_enabled, uint32_t processed) {
    return is_enabled || (was_enabled != 0 && processed != 0);
}

// The reverb's channel counts (LockForProcess stores its formats' 16-bit ones)
// are ones its kernels take: mono, stereo and 5.1, each way.
constexpr uint32_t kMaxReverbChannels = 6;
constexpr bool ReverbChannelsOk(uint32_t in, uint32_t out) {
    return in >= 1 && in <= kMaxReverbChannels && out >= 1 && out <= kMaxReverbChannels;
}

// The game's microphone list, ExternalMic's static std::vector<ExternalMic*>,
// from its begin and end words: ExternalMic::Init makes four.
constexpr uint32_t kMaxExternalMics = 4;
enum class MicList {
    kEmpty,       // not made yet (before Synth360::Init), or none
    kEntries,     // up to four pointers
    kUnreadable,  // not a list: every effect runs
};
constexpr MicList MicListShape(uint32_t begin, uint32_t end) {
    if (begin == end) return MicList::kEmpty;
    if (!begin || end < begin || (end - begin) % 4 || end - begin > 4 * kMaxExternalMics) {
        return MicList::kUnreadable;
    }
    return MicList::kEntries;
}

}
