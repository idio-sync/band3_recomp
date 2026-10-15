// native_vorbis: the inverse MDCT of the songs' Vorbis decoder run natively
// (src/Audio/vorbis_mdct.h). The game decodes every song's and menu preview's
// mogg on one decode thread (VorbisReader::DecodeThreadPoll, rb3-xenon's
// synth/VorbisReader.cpp) with its own libvorbis; mdct_backward turns each
// channel's block of coefficients into samples, about a third of that
// thread's time recompiled, and a quarter of that native.
//
// It's a pure function of its lookup and its coefficients, so nothing is kept
// between calls: seeks, previews that start mid-song, a song change and
// several readers at once are all just more calls. Each call reads the
// game's tables where they are, so nothing goes stale either.
//
// native_vorbis_check runs both on every block, keeps the game's result, and
// reports every 5 s how many samples the native one got different and by how
// much: none, as it computes what the recompiled one does rounding for
// rounding.

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include "generated/band3_init.h"
#include "src/Audio/vorbis_mdct.h"
#include "src/settings.h"

namespace {

using band3::audio::VorbisMdctTables;
using Clock = std::chrono::steady_clock;

// libvorbis's mdct_lookup as the game's mdct_init (0x82C357D0) fills it
constexpr uint32_t kLookup_N = 0;
constexpr uint32_t kLookup_Log2n = 4;
constexpr uint32_t kLookup_Trig = 8;
constexpr uint32_t kLookup_Trig2 = 12;  // the game's own: the pairs turned a quarter
constexpr uint32_t kLookup_Bitrev = 16;

constexpr int kMaxSamples = 8192;  // Vorbis's largest block

// the constants the game's butterflies and bit-reverse load, which
// vorbis_mdct.cpp has built in
struct Constant {
    uint32_t address;
    float value;
};
constexpr std::array<Constant, 4> kConstants = {{
    {0x821CEA40, band3::audio::kVorbisPi1_8},
    {0x821651F4, band3::audio::kVorbisPi2_8},
    {0x821651F0, band3::audio::kVorbisPi3_8},
    {0x82075090, 0.5f},
}};

constexpr auto kReportInterval = std::chrono::seconds(5);
// VorbisReader::DecodeThreadPoll makes 16-bit samples of these: x * 32767
constexpr double kSampleStep = 1.0 / 32767.0;

std::mutex g_mutex;
std::atomic<int> g_enabled_logged{-1};
std::atomic<int> g_check_logged{-1};
// whether the game's constants are the native one's: not read yet (-1), no, yes
std::atomic<int> g_constants{-1};
bool g_unusable_logged = false;
bool g_difference_logged = false;

// what native_vorbis_check found since its last report
struct Check {
    uint64_t transforms = 0;
    uint64_t samples = 0;
    uint64_t different = 0;
    double largest = 0.0;  // |game's - native| of the most different sample
    uint32_t sizes = 0;    // bit log2n set for each block size seen
};
Check g_check;
Clock::time_point g_next_report{};

void LoadFloats(const uint8_t* guest, float* host, int count) {
    for (int i = 0; i < count; i++) {
        uint32_t raw;
        std::memcpy(&raw, guest + 4 * i, sizeof raw);
        host[i] = std::bit_cast<float>(std::byteswap(raw));
    }
}

void StoreFloats(const float* host, uint8_t* guest, int count) {
    for (int i = 0; i < count; i++) {
        const uint32_t raw = std::byteswap(std::bit_cast<uint32_t>(host[i]));
        std::memcpy(guest + 4 * i, &raw, sizeof raw);
    }
}

// the settings, at the first call and when they change
void LogSettings(bool enabled, bool check) {
    if (g_enabled_logged.exchange(int(enabled)) != int(enabled)) {
        REXLOG_INFO("Native Vorbis: {}", enabled ? "on: the songs' inverse MDCT runs natively"
                                                 : "off: the game's inverse MDCT runs");
    }
    if (g_check_logged.exchange(int(check)) != int(check) && check) {
        REXLOG_INFO("Native Vorbis check: on: the game's and the native inverse MDCT both run "
                    "on every block, the game's is used, and every 5 s the log says how far "
                    "apart they came out");
    }
}

// whether the game's constants are the ones the native transform has built
// in, read once
bool ConstantsMatch(uint8_t* base) {
    const int known = g_constants.load(std::memory_order_relaxed);
    if (known >= 0) return known == 1;
    std::lock_guard lock(g_mutex);
    if (g_constants.load() < 0) {
        int match = 1;
        for (const Constant& c : kConstants) {
            const uint32_t game = REX_LOAD_U32(c.address);
            const uint32_t native = std::bit_cast<uint32_t>(c.value);
            if (game != native) {
                REXLOG_WARN("Native Vorbis: the game's inverse MDCT constant at {:08X} is "
                            "{:08X}, not the {:08X} the native one has; the game's runs",
                            c.address, game, native);
                match = 0;
            }
        }
        g_constants.store(match);
    }
    return g_constants.load() == 1;
}

// a lookup or buffers the native transform doesn't take: the game's runs
void LogUnusable(const VorbisMdctTables& t, uint32_t in, uint32_t out) {
    std::lock_guard lock(g_mutex);
    if (g_unusable_logged) return;
    g_unusable_logged = true;
    REXLOG_WARN("Native Vorbis: an inverse MDCT of n {} (log2 {}) from {:08X} to {:08X} isn't "
                "one the native one takes; the game's runs for it",
                t.n, t.log2n, in, out);
}

// native_vorbis_check: the game's samples, as it left them in `out`, against
// the native ones
void Compare(const VorbisMdctTables& t, const uint8_t* game, const float* native) {
    uint64_t different = 0;
    double largest = 0.0;
    int first = -1;
    for (int i = 0; i < t.n; i++) {
        uint32_t raw;
        std::memcpy(&raw, game + 4 * i, sizeof raw);
        const uint32_t game_bits = std::byteswap(raw);
        const uint32_t native_bits = std::bit_cast<uint32_t>(native[i]);
        if (game_bits == native_bits) continue;
        different++;
        if (first < 0) first = i;
        largest = std::max(largest, std::fabs(double(std::bit_cast<float>(game_bits)) -
                                              double(native[i])));
    }
    const auto now = Clock::now();
    std::lock_guard lock(g_mutex);
    if (different && !g_difference_logged) {
        g_difference_logged = true;
        uint32_t raw;
        std::memcpy(&raw, game + 4 * first, sizeof raw);
        REXLOG_WARN("Native Vorbis check: the first different sample: n {}, sample {}: the "
                    "game's {:08X}, the native {:08X}",
                    t.n, first, std::byteswap(raw), std::bit_cast<uint32_t>(native[first]));
    }
    g_check.transforms++;
    g_check.samples += uint64_t(t.n);
    g_check.different += different;
    g_check.largest = std::max(g_check.largest, largest);
    g_check.sizes |= 1u << t.log2n;
    if (g_next_report == Clock::time_point{}) g_next_report = now + kReportInterval;
    if (now < g_next_report) return;
    std::string sizes;
    for (int b = 6; b <= 13; b++) {
        if (g_check.sizes & (1u << b)) sizes += (sizes.empty() ? "" : "/") + std::to_string(1 << b);
    }
    if (g_check.different == 0) {
        REXLOG_INFO("Native Vorbis check: last 5 s {} transforms (n {}), {} samples, all the "
                    "same as the game's",
                    g_check.transforms, sizes, g_check.samples);
    } else {
        REXLOG_WARN("Native Vorbis check: last 5 s {} transforms (n {}), {} samples, {} "
                    "different from the game's, by up to {:.3g} of full scale ({:.3g} of a "
                    "16-bit step)",
                    g_check.transforms, sizes, g_check.samples, g_check.different,
                    g_check.largest, g_check.largest / kSampleStep);
    }
    g_check = {};
    g_next_report = now + kReportInterval;
}

}

// mdct_backward(mdct_lookup* init, float* in, float* out) (0x82C36480): one
// channel's inverse MDCT, n/2 coefficients in, n samples out, for every
// channel of every block (mapping0_inverse_poll's last step, with in == out)
extern "C" void __imp__mdct_backward(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(mdct_backward) {
    const bool enabled = REXCVAR_GET(native_vorbis);
    const bool check = REXCVAR_GET(native_vorbis_check);
    LogSettings(enabled, check);
    if (!enabled && !check) {
        __imp__mdct_backward(ctx, base);
        return;
    }
    const uint32_t lookup = ctx.r3.u32;
    const uint32_t in = ctx.r4.u32;
    const uint32_t out = ctx.r5.u32;
    // a table's host address; null for none
    const auto table = [&](uint32_t offset) -> const uint8_t* {
        const uint32_t address = REX_LOAD_U32(lookup + offset);
        return address ? REX_RAW_ADDR(address) : nullptr;
    };
    const VorbisMdctTables t{int32_t(REX_LOAD_U32(lookup + kLookup_N)),
                             int32_t(REX_LOAD_U32(lookup + kLookup_Log2n)), table(kLookup_Trig),
                             table(kLookup_Trig2), table(kLookup_Bitrev)};
    // in place, or apart: the native one reads all its coefficients first
    const bool apart = in == out || uint64_t(in) + 2ull * uint32_t(t.n) <= out ||
                       uint64_t(out) + 4ull * uint32_t(t.n) <= in;
    if (!band3::audio::VorbisMdctTablesValid(t) || !apart || !ConstantsMatch(base)) {
        if (!band3::audio::VorbisMdctTablesValid(t) || !apart) LogUnusable(t, in, out);
        __imp__mdct_backward(ctx, base);
        return;
    }

    thread_local std::array<float, kMaxSamples> samples;
    LoadFloats(REX_RAW_ADDR(in), samples.data(), t.n / 2);
    // the recompiled code runs its scalar float math with denormals kept
    ctx.fpscr.disableFlushMode();
    band3::audio::VorbisMdctBackward(t, samples.data());
    if (!check) {
        StoreFloats(samples.data(), REX_RAW_ADDR(out), t.n);
        return;
    }
    // the game's runs too, and is what the game uses
    __imp__mdct_backward(ctx, base);
    Compare(t, REX_RAW_ADDR(out), samples.data());
}
