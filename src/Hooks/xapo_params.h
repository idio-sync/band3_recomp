#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

// audio_xapo_nonblocking (src/Hooks/song_audio.cpp): XAudio2's
// CXAPOParametersBase::GetParameters answered without waiting for the audio
// thread.
//
// The XDK's CXAPOParametersBase (the Windows SDK's xapobase.h has the same
// members) keeps an effect's parameters in three blocks. Every effect RB3 makes
// is a consumer (ATG's CSampleXAPOBase passes fProducer = FALSE), for which:
// SetParameters takes the block m_pCurrentParameters points to, leaving it
// claimed (-1), and copies the parameters into it and into the third block;
// BeginProcess, on the audio thread at the start of the effect's Process, puts
// that block back and sets m_fNewerResultsReady; GetParameters, with newer
// results ready, waits for the block to be put back, swaps it for the other
// one and copies it out.
//
// RB3's EnvelopeGenerator (the effect on every voice, whose parameters say
// whether its release has finished: Voice::IsPlaying reads them) calls
// SetParameters itself at the end of each DoProcess, on the audio thread,
// while its envelope attacks or releases. So between those passes its block is
// claimed and newer results are ready, and the game's GetParameters waits for
// the next pass to reach the voice, a few microseconds after its BeginProcess,
// before SetParameters claims the block again: up to an audio frame (256
// samples, 5.3 ms) on a console, and up to two here, where the SDK's audio
// device asks for frames two at a time. The third block already holds what
// SetParameters last set (here, the last pass's result), so it's copied out
// instead, leaving the blocks' hand-over to a later call that finds the block
// put back.

namespace band3::xapo {

// CXAPOParametersBase's members, from its IXAPOParameters interface (the
// `this` GetParameters gets: the object + 32)
constexpr uint32_t kParameterBlocks = 4;          // BYTE*: three blocks
constexpr uint32_t kCurrentParameters = 8;        // BYTE*, or kClaimed
constexpr uint32_t kParameterBlockByteSize = 20;
constexpr uint32_t kNewerResultsReady = 24;       // BOOL
constexpr uint32_t kProducer = 28;                // BOOL
constexpr uint32_t kClaimed = 0xFFFFFFFFu;

struct State {
    uint32_t current;  // m_pCurrentParameters
    uint32_t newer;    // m_fNewerResultsReady
    uint32_t producer; // m_fProducer
};

// GetParameters would wait for the audio thread: newer results are ready (the
// guest compares with 1) and the block is claimed. A producer's block is
// claimed only inside its Process, and the third block isn't its results, so
// it's left to the guest.
constexpr bool WouldWait(const State& s) {
    return s.producer == 0 && s.newer == 1 && s.current == kClaimed;
}

// the third block, which SetParameters copies the parameters into as well
constexpr uint32_t LatestBlock(uint32_t blocks, uint32_t block_size) {
    return blocks + 2 * block_size;
}

// Copies `size` bytes of `src` to `dst` until a copy matches `src` read again,
// as SetParameters on the audio thread may be writing it; false if none did in
// `tries`. Byte by byte through volatile, so each read is a fresh load: blocks
// are a few dozen bytes.
inline bool CopySettled(uint8_t* dst, const volatile uint8_t* src, size_t size,
                        int tries = 4) {
    for (int t = 0; t < tries; t++) {
        for (size_t i = 0; i < size; i++) dst[i] = src[i];
        std::atomic_thread_fence(std::memory_order_acquire);
        bool same = true;
        for (size_t i = 0; i < size && same; i++) same = dst[i] == src[i];
        if (same) return true;
    }
    return false;
}

}
