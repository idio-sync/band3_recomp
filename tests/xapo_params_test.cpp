// Checks audio_xapo_nonblocking's answer (src/Hooks/xapo_params.h) against a
// model of the XDK's CXAPOParametersBase for a consumer effect, as the
// recompiled SetParameters, BeginProcess and GetParameters have it: when
// GetParameters would wait for the audio thread, the third block already holds
// what it would hand back after the wait, or newer.

#include <doctest/doctest.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include "src/Hooks/xapo_params.h"

using namespace band3::xapo;

namespace {

constexpr uint32_t kBlocks = 0x1000;  // the three blocks' guest address
constexpr uint32_t kSize = 16;        // EnvelopeGenerator's parameters

using Params = std::array<uint8_t, kSize>;

Params P(uint8_t v) {
    Params p;
    p.fill(v);
    return p;
}

// one effect's CXAPOParametersBase, a consumer (m_fProducer FALSE)
struct Effect {
    std::array<uint8_t, 3 * kSize> blocks{};
    uint32_t current = kBlocks;  // m_pCurrentParameters
    uint32_t internal = 0;       // m_pCurrentParametersInternal
    uint32_t index = 0;          // m_uCurrentParametersIndex
    uint32_t newer = 0;          // m_fNewerResultsReady

    uint8_t* At(uint32_t addr) { return blocks.data() + (addr - kBlocks); }

    // SetParameters: claims the current block (unless it holds one already) and
    // copies the parameters into it and into the third
    void Set(const Params& p) {
        if (!internal) {
            internal = current;
            current = kClaimed;
        }
        std::memcpy(At(internal), p.data(), kSize);
        std::memcpy(At(LatestBlock(kBlocks, kSize)), p.data(), kSize);
    }

    // BeginProcess, on the audio thread: the claimed block put back
    void Begin() {
        if (internal) {
            current = internal;
            newer = 1;
            internal = 0;
        }
    }

    // GetParameters as the guest has it; nullopt where it would wait
    std::optional<Params> Get() {
        if (newer == 1) {
            if (current == kClaimed) return std::nullopt;
            if (current == kBlocks + index * kSize) current = kBlocks + (index ^ 1) * kSize;
            newer = 0;
            index ^= 1;
        }
        Params out{};
        std::memcpy(out.data(), At(kBlocks + (index ^ 1) * kSize), kSize);
        return out;
    }

    // with audio_xapo_nonblocking
    Params HookedGet() {
        if (WouldWait({current, newer, 0})) {
            Params out{};
            REQUIRE(CopySettled(out.data(), At(LatestBlock(kBlocks, kSize)), kSize));
            return out;
        }
        return *Get();
    }

    // one audio pass over an EnvelopeGenerator: its DoProcess ends by setting
    // its own result
    void EnvelopePass(const Params& result) {
        Begin();
        Set(result);
    }
};

}

TEST_CASE("only a consumer with newer results and its block claimed waits") {
    CHECK(WouldWait({kClaimed, 1, 0}));
    CHECK_FALSE(WouldWait({kBlocks, 1, 0}));
    CHECK_FALSE(WouldWait({kClaimed, 0, 0}));
    CHECK_FALSE(WouldWait({kClaimed, 1, 1}));
}

TEST_CASE("the latest parameters are in the third block") {
    CHECK(LatestBlock(kBlocks, kSize) == kBlocks + 32);
}

TEST_CASE("between an envelope's passes the guest waits; the hook has its last result") {
    Effect e;
    e.EnvelopePass(P(1));
    e.EnvelopePass(P(2));
    // the block is claimed again inside each pass, so it stays claimed between them
    CHECK(e.current == kClaimed);
    CHECK_FALSE(e.Get().has_value());
    CHECK(e.HookedGet() == P(2));
    // the guest's own answer, once the next pass puts the block back: no newer
    // than the hook's
    e.Begin();
    CHECK(e.Get() == P(2));
}

TEST_CASE("the hook leaves the hand-over to a later call") {
    Effect e;
    e.EnvelopePass(P(1));
    e.EnvelopePass(P(2));
    CHECK(e.HookedGet() == P(2));
    CHECK(e.newer == 1);
    CHECK(e.current == kClaimed);
    e.EnvelopePass(P(3));
    CHECK(e.HookedGet() == P(3));
    // a call that finds the block put back takes it, as the guest does
    e.Begin();
    CHECK(e.HookedGet() == P(3));
    CHECK(e.newer == 0);
    // and with nothing newer, the block it took
    CHECK(e.HookedGet() == P(3));
}

TEST_CASE("parameters the game sets are read back without a pass") {
    Effect e;
    e.Begin();
    e.Set(P(7));
    e.Begin();  // newer ready
    e.Set(P(8));
    CHECK_FALSE(e.Get().has_value());
    CHECK(e.HookedGet() == P(8));
    e.Begin();
    CHECK(e.Get() == P(8));
}

TEST_CASE("CopySettled copies a block no one is writing") {
    const Params src = P(5);
    Params dst = P(0);
    CHECK(CopySettled(dst.data(), src.data(), kSize));
    CHECK(dst == src);
    CHECK(CopySettled(dst.data(), src.data(), 0));
}
