// audio_idle_bypass: the game's audio effects skipped while all they do is
// process silence (the rule and its constants are in audio_idle.h). Five guest
// functions are hooked. Four are DSP inside the parameter hand-over their
// effects do around processing (xapo_params.h), so only the DSP is skipped; the
// reverb's is its XAPO's whole Process, which is never skipped while new
// parameters wait for it (its own hand-over):
// - DSP::Synapse::ProcessInPlace, the mic chain's pitch correction;
// - LEAPFX::CAudioReverb::Process, XAudio2's reverb on the game's reverb submix;
// - EQEffect::Process, CompressionEffect::Process and DelayEffect::Process: the
//   mic chain's EQ, compressor and echo, the master limiter, and the same
//   effects on the game's other effect sends.
// Each has one caller: its effect's DoProcess, or for the reverb its vtable.
// Structures are from rb3-xenon's src/system/synth and synth_xbox and the
// recompiled code.

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <unordered_map>

#include "generated/band3_init.h"
#include "src/Hooks/audio_idle.h"
#include "src/settings.h"

namespace {

using namespace band3::audio_idle;
using Clock = std::chrono::steady_clock;

// ExternalMic's static std::vector<ExternalMic*> (begin, end), which
// ExternalMic::NumConnectedMics (0x82B66D18) counts; ExternalMic+0x9 is set
// while the game holds that mic's connection (Hooks/usb_mic.cpp sets it for
// USB mics)
constexpr uint32_t kExternalMics = 0x82E1218C;
constexpr uint32_t kExternalMic_Connected = 0x9;

// XAPO_PROCESS_BUFFER_PARAMETERS
constexpr uint32_t kBuffer_Samples = 0x0;
constexpr uint32_t kBuffer_Flags = 0x4;
constexpr uint32_t kBuffer_ValidFrameCount = 0x8;
constexpr uint32_t kBufferSilent = 0;  // XAPO_BUFFER_SILENT
constexpr uint32_t kBufferValid = 1;   // XAPO_BUFFER_VALID

// LEAPFX::CAudioReverb
constexpr uint32_t kReverb_NewParameters = 204;  // left by SetParameters for Process
constexpr uint32_t kReverb_InputChannels = 212;  // from LockForProcess
constexpr uint32_t kReverb_OutputChannels = 216;
constexpr uint32_t kReverb_WasEnabled = 228;  // IsEnabled at its last Process
constexpr uint32_t kReverb_Processed = 232;   // set by its first Process
// the float Process fills a SILENT input with: 1.0e-18, not 0, which keeps the
// reverb's state out of the subnormals
constexpr uint32_t kReverbSilentFill = 0x821A6B5C;

// CompressionEffect: every word its Process reads besides the samples (named
// as in Audio/mic_fx.cpp's copy of it). It writes only the last three, its
// state; the rest are its parameters, which SetParameters writes from the
// game's thread.
constexpr std::array<uint32_t, 12> kCompression_Words = {
    0,   // threshold
    8,   // make-up gain
    12,  // ratio
    16,  // output gain
    20,  // attack coefficient
    24,  // release coefficient
    32,  // the gate's attack coefficient
    36,  // the gate's release coefficient
    48,  // the gate's ceiling
    44,  // mGateMin, the gate's floor (SetParameters sets it too)
    52,  // mDCBlock (mono only)
    56,  // mEnvelope, the gain it applies
};

constexpr auto kReportInterval = std::chrono::seconds(30);
// instances are freed when the game rebuilds its effect sends
constexpr auto kForgetAfter = std::chrono::seconds(60);

enum class Kind { kSynapse, kReverb, kEq, kCompressor, kDelay, kCount };
constexpr std::array<const char*, size_t(Kind::kCount)> kKindNames = {
    "synapse", "reverb", "eq", "compressor", "delay"};

enum class Mics { kUnknown, kNone, kConnected, kUnreadable };

// what the microphone gate read
struct MicGate {
    Mics mics;
    uint32_t begin;
    uint32_t end;
};

// one effect instance
struct Entry {
    Settle settle;
    bool skipping = false;  // its last call was skipped
    Clock::time_point last_seen;
    // the words of its own its Process reads as its last processed block left
    // them (the compressor's: kCompression_Words)
    std::array<uint32_t, kCompression_Words.size()> left{};
};

// calls since the last report, of one kind
struct Counts {
    uint64_t calls = 0;
    uint64_t skipped = 0;
    uint64_t silent = 0;          // with silent input
    uint64_t flagged_silent = 0;  // with input flagged SILENT (the reverb's)
};

std::mutex g_mutex;
// by (kind << 32) | the instance's guest address
std::unordered_map<uint64_t, Entry> g_entries;
std::array<Counts, size_t(Kind::kCount)> g_counts{};
std::array<bool, size_t(Kind::kCount)> g_skip_logged{};
int g_enabled_logged = -1;
Mics g_mics_logged = Mics::kUnknown;
bool g_unreadable_logged = false;
bool g_reverb_channels_logged = false;
Clock::time_point g_next_report{};

// Whether the game counts a microphone as connected, read the way
// ExternalMic::NumConnectedMics (a leaf) counts them rather than by calling it.
MicGate ReadMics(uint8_t* base) {
    const uint32_t begin = REX_LOAD_U32(kExternalMics);
    const uint32_t end = REX_LOAD_U32(kExternalMics + 4);
    switch (MicListShape(begin, end)) {
    case MicList::kEmpty:
        return {Mics::kNone, begin, end};
    case MicList::kUnreadable:
        return {Mics::kUnreadable, begin, end};
    case MicList::kEntries:
        break;
    }
    for (uint32_t p = begin; p < end; p += 4) {
        const uint32_t mic = REX_LOAD_U32(p);
        if (mic && REX_LOAD_U8(mic + kExternalMic_Connected)) return {Mics::kConnected, begin, end};
    }
    return {Mics::kNone, begin, end};
}

// the setting and the microphone gate, at the first call and when they change
void LogChanges(bool enabled, const MicGate& gate) {
    if (int(enabled) != g_enabled_logged) {
        g_enabled_logged = enabled;
        REXLOG_INFO("Audio idle bypass: {}", enabled ? "on" : "off");
    }
    if (gate.mics == g_mics_logged) return;
    g_mics_logged = gate.mics;
    switch (gate.mics) {
    case Mics::kNone:
        REXLOG_INFO("Audio idle bypass: no microphone is connected");
        break;
    case Mics::kConnected:
        REXLOG_INFO("Audio idle bypass: a microphone is connected; every audio effect runs");
        break;
    case Mics::kUnreadable:
        if (!g_unreadable_logged) {
            g_unreadable_logged = true;
            REXLOG_WARN("Audio idle bypass: the game's microphone list runs {:08X} to {:08X}, "
                        "not a list of up to {}; every audio effect runs",
                        gate.begin, gate.end, kMaxExternalMics);
        }
        break;
    case Mics::kUnknown:
        break;
    }
}

// what was skipped over the last 30 s; forgets instances not seen for 60 s
void Report(Clock::time_point now) {
    size_t instances = 0;
    size_t skipping = 0;
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        const auto unseen = now - it->second.last_seen;
        if (unseen > kForgetAfter) {
            it = g_entries.erase(it);
            continue;
        }
        if (unseen < kReportInterval) {
            instances++;
            skipping += it->second.skipping;
        }
        ++it;
    }
    const auto& c = g_counts;
    const auto at = [&](Kind kind) -> const Counts& { return c[size_t(kind)]; };
    REXLOG_INFO("Audio idle bypass: last 30 s skipped synapse {}/{}, reverb {}/{}, eq {}/{}, "
                "compressor {}/{}, delay {}/{} calls ({} of {} instances skipping now); silent "
                "input: synapse {}, reverb {} ({} flagged SILENT), eq {}, compressor {}, delay {}",
                at(Kind::kSynapse).skipped, at(Kind::kSynapse).calls,
                at(Kind::kReverb).skipped, at(Kind::kReverb).calls, at(Kind::kEq).skipped,
                at(Kind::kEq).calls, at(Kind::kCompressor).skipped, at(Kind::kCompressor).calls,
                at(Kind::kDelay).skipped, at(Kind::kDelay).calls, skipping, instances,
                at(Kind::kSynapse).silent, at(Kind::kReverb).silent,
                at(Kind::kReverb).flagged_silent, at(Kind::kEq).silent,
                at(Kind::kCompressor).silent, at(Kind::kDelay).silent);
    g_counts = {};
}

// Whether one call of a hooked effect instance skips its block, with the logs
// and counts that go with it. `words` are the words of its own its Process
// reads, as they are now (the compressor's): if anything but its processing
// changed them, its count starts over. Returns the instance's entry when the
// original is to run, null when the block is skipped.
Entry* Decide(uint8_t* base, Kind kind, uint32_t self, uint32_t channels, bool silent,
              bool steady, bool flagged_silent = false, std::span<const uint32_t> words = {}) {
    const bool enabled = REXCVAR_GET(audio_idle_bypass);
    const MicGate gate = ReadMics(base);
    const auto now = Clock::now();
    std::lock_guard lock(g_mutex);
    LogChanges(enabled, gate);
    const char* name = kKindNames[size_t(kind)];
    auto [it, added] = g_entries.try_emplace((uint64_t(kind) << 32) | self);
    Entry& e = it->second;
    if (added) REXLOG_INFO("Audio idle bypass: {} {:08X}: {} channels", name, self, channels);
    e.last_seen = now;
    e.settle.Recheck(words, std::span<const uint32_t>(e.left).first(words.size()));
    e.skipping = Skip(enabled, gate.mics != Mics::kNone, silent, steady, e.settle);

    Counts& c = g_counts[size_t(kind)];
    c.calls++;
    c.skipped += e.skipping;
    c.silent += silent;
    c.flagged_silent += flagged_silent;
    if (e.skipping && !g_skip_logged[size_t(kind)]) {
        g_skip_logged[size_t(kind)] = true;
        REXLOG_INFO("Audio idle bypass: skipping the {} at {:08X} ({} channels) while nothing "
                    "reaches it",
                    name, self, channels);
    }
    if (g_next_report == Clock::time_point{}) {
        g_next_report = now + kReportInterval;
    } else if (now >= g_next_report) {
        Report(now);
        g_next_report = now + kReportInterval;
    }
    return e.skipping ? nullptr : &e;
}

// after the original ran: whether this block counts toward skipping the next
// (its input was silent and what the effect left was quiet), and the words of
// its own it left
void Observe(Entry* e, bool quiet, uint32_t frames, std::span<const uint32_t> left = {}) {
    std::lock_guard lock(g_mutex);
    e->settle.Observe(quiet, frames);
    std::copy(left.begin(), left.end(), e->left.begin());
}

using GuestFunction = void (*)(PPCContext&, uint8_t*);

// T::Process(T* effect, float* samples, int frames, int channels): an ATG
// effect's DSP, in place on interleaved samples, called by
// StandardEffect<T>::DoProcess. `own` are the offsets of the words of its own
// it reads besides the samples, where it has parameters, and state that a
// silent block can change without showing in the output.
template <size_t N = 0>
void ProcessInPlace(PPCContext& ctx, uint8_t* base, Kind kind, GuestFunction original,
                    const std::array<uint32_t, N>& own = {}) {
    static_assert(N <= kCompression_Words.size(), "Entry::left holds its words");
    const uint32_t self = ctx.r3.u32;
    const uint32_t samples = ctx.r4.u32;
    const int32_t frames = ctx.r5.s32;
    const int32_t channels = ctx.r6.s32;
    // it processes nothing unless both are positive
    const size_t count = frames > 0 && channels > 0 ? size_t(frames) * size_t(channels) : 0;
    const bool silent = AllZero(REX_RAW_ADDR(samples), count);
    const auto load_own = [&] {
        std::array<uint32_t, N> words{};
        for (size_t i = 0; i < N; i++) words[i] = REX_LOAD_U32(self + own[i]);
        return words;
    };
    const auto before = load_own();
    Entry* e = Decide(base, kind, self, uint32_t(channels), silent, true, false, before);
    // skipped: the buffer already holds the zeros
    if (!e) return;

    original(ctx, base);
    const auto after = load_own();
    Observe(e, silent && AllQuiet(REX_RAW_ADDR(samples), count) && after == before,
            frames > 0 ? uint32_t(frames) : 0, after);
}

}

// DSP::Synapse::ProcessInPlace(Synapse*, numSamples, float* samples): the mic
// chain's pitch correction, in place, tail-called by DSP::SynapseAPO::DoProcess
// with the block's frames as numSamples and its channel count left in r7. It
// resynthesizes its whole output from its last 400 ms of input, so once its
// input has been silent that long it writes zeros.
extern "C" void __imp__DSP__Synapse__Synapse__ProcessInPlace(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(DSP__Synapse__Synapse__ProcessInPlace) {
    const uint32_t self = ctx.r3.u32;
    const uint32_t frames = ctx.r4.u32;
    const uint32_t samples = ctx.r5.u32;
    const bool silent = AllZero(REX_RAW_ADDR(samples), frames);
    Entry* e = Decide(base, Kind::kSynapse, self, ctx.r7.u32, silent, true);
    // skipped: the buffer already holds the zeros it would write
    if (!e) return;
    __imp__DSP__Synapse__Synapse__ProcessInPlace(ctx, base);
    Observe(e, silent && AllQuiet(REX_RAW_ADDR(samples), frames), frames);
}

// LEAPFX::CAudioReverb::Process(this, InputCount, pInput, OutputCount, pOutput,
// IsEnabled): XAudio2's reverb, the effect on the game's global reverb submix,
// reached only through its vtable. Enabled, it processes every pass and flags
// its output VALID, filling a SILENT input with 1.0e-18 first, so a skipped
// block writes what that path writes less the reverb: the same fill, zeros for
// its output (which was within 2^-24 of them), VALID and the frame count. Its
// other words already hold what that path would store, and it never skips with
// new parameters waiting, so those are applied on the next pass it processes.
extern "C" void __imp__rex_sub_82BC7518(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(rex_sub_82BC7518) {
    const uint32_t self = ctx.r3.u32;
    const uint32_t in = ctx.r5.u32;
    const uint32_t out = ctx.r7.u32;
    const bool is_enabled = ctx.r8.u32 != 0;
    // LockForProcess stores the formats' 16-bit channel counts in these words
    const uint32_t in_channels = REX_LOAD_U32(self + kReverb_InputChannels);
    const uint32_t out_channels = REX_LOAD_U32(self + kReverb_OutputChannels);
    if (!ReverbChannelsOk(in_channels, out_channels)) {
        // not a layout its kernels take: leave it alone
        {
            std::lock_guard lock(g_mutex);
            if (!g_reverb_channels_logged) {
                g_reverb_channels_logged = true;
                REXLOG_WARN("Audio idle bypass: the reverb at {:08X} has {} input and {} output "
                            "channels (enabled {}); it always runs",
                            self, in_channels, out_channels, is_enabled);
            }
        }
        __imp__rex_sub_82BC7518(ctx, base);
        return;
    }
    const uint32_t frames = REX_LOAD_U32(in + kBuffer_ValidFrameCount);
    const uint32_t in_samples = REX_LOAD_U32(in + kBuffer_Samples);
    const uint32_t out_samples = REX_LOAD_U32(out + kBuffer_Samples);
    const bool flagged_silent = REX_LOAD_U32(in + kBuffer_Flags) == kBufferSilent;
    const bool silent =
        flagged_silent || AllZero(REX_RAW_ADDR(in_samples), size_t(frames) * in_channels);
    const uint32_t was_enabled = REX_LOAD_U32(self + kReverb_WasEnabled);
    const uint32_t processed = REX_LOAD_U32(self + kReverb_Processed);
    const bool steady = ReverbSteady(is_enabled, REX_LOAD_U32(self + kReverb_NewParameters),
                                     was_enabled, processed);
    const bool kernel_runs = ReverbKernelRuns(is_enabled, was_enabled, processed);
    Entry* e = Decide(base, Kind::kReverb, self, out_channels, silent, steady, flagged_silent);
    if (!e) {
        if (flagged_silent) {
            // the reverb's anti-denormal fill, as the original leaves it
            const uint32_t fill = REX_LOAD_U32(kReverbSilentFill);
            for (size_t i = 0; i < size_t(frames) * in_channels; i++) {
                REX_STORE_U32(in_samples + 4 * uint32_t(i), fill);
            }
        }
        std::memset(REX_RAW_ADDR(out_samples), 0, size_t(frames) * out_channels * 4);
        REX_STORE_U32(out + kBuffer_Flags, kBufferValid);
        REX_STORE_U32(out + kBuffer_ValidFrameCount, frames);
        return;
    }
    __imp__rex_sub_82BC7518(ctx, base);
    // Only a pass that ran the kernel counts: disabled, its state holds still
    // and its output is its input copied through, or isn't written (flagged
    // SILENT). Fading passes count too: a fade shows any state that wasn't
    // quiet.
    if (!kernel_runs) return;
    const uint32_t out_frames = REX_LOAD_U32(out + kBuffer_ValidFrameCount);
    const bool quiet =
        out_samples && AllQuiet(REX_RAW_ADDR(out_samples), size_t(out_frames) * out_channels);
    Observe(e, silent && quiet, frames);
}

// The mic chain's EQ (and other sends'): linear filters, whose output shows
// what's left in them.
extern "C" void __imp__EQEffect__Process(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(EQEffect__Process) {
    ProcessInPlace(ctx, base, Kind::kEq, __imp__EQEffect__Process);
}

// The mic chain's compressor and the master limiter. Its gain keeps moving
// after its input goes silent without showing in a silent output, so it's
// skipped only once silent blocks leave its state where it was, and its count
// goes on only while its parameters and state are what its last processed
// block left: then the blocks it skips are what processing them would have
// done. New parameters from the game, a Reset (its send's bypass turning on,
// which puts its gain back to 1.0) or a new instance at its address start the
// count over: it's processed until it settles again.
extern "C" void __imp__CompressionEffect__Process(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(CompressionEffect__Process) {
    ProcessInPlace(ctx, base, Kind::kCompressor, __imp__CompressionEffect__Process,
                   kCompression_Words);
}

// The mic chain's overdrive echo. On silent input it writes what it outputs
// back into its 2 s ring, so 2 s of quiet output leave the whole ring quiet;
// its write position stays put while it's skipped.
extern "C" void __imp__DelayEffect__Process(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(DelayEffect__Process) {
    ProcessInPlace(ctx, base, Kind::kDelay, __imp__DelayEffect__Process);
}
