// native_vorbis: the inverse MDCT and the codebook decoder of the songs'
// Vorbis decoder run natively (src/Audio/vorbis_mdct.h, vorbis_codebook.h).
// The game decodes every song's and menu preview's mogg on one decode thread
// (VorbisReader::DecodeThreadPoll, rb3-xenon's synth/VorbisReader.cpp) with
// its own libvorbis; mdct_backward turns each channel's block of coefficients
// into samples, about a third of that thread's time recompiled, and a quarter
// of that native. Reading the codewords (decode_packed_entry_number and the
// vector decoders that add each one's values to the residue) is about a
// quarter more, in many small calls.
//
// Both are pure functions of what they're handed, so nothing is kept between
// calls: seeks, previews that start mid-song, a song change and several
// readers at once are all just more calls. Each call reads the game's tables
// where they are, so nothing goes stale either. The codebook functions take
// the guest's bit reader (its oggpack_buffer) at entry and leave it as the
// game's would, failures near a packet's end included.
//
// native_vorbis_check runs both on every call, keeps the game's results, and
// reports every 5 s how many samples the native inverse MDCT got different and
// by how much, and whether the native codebook functions returned, read and
// added what the game's did: none different, as they compute what the
// recompiled ones do, rounding for rounding.

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
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include "generated/band3_init.h"
#include "src/Audio/vorbis_codebook.h"
#include "src/Audio/vorbis_mdct.h"
#include "src/settings.h"

namespace {

using band3::audio::VorbisBitReader;
using band3::audio::VorbisCodebook;
using band3::audio::VorbisGuest;
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

// the codebook functions native_vorbis takes over
enum class Book { kPacked, kDecode, kDecodevAdd, kDecodevsAdd, kDecodevvAdd, kCount };
constexpr std::array<const char*, size_t(Book::kCount)> kBookNames = {
    "decode_packed_entry_number", "vorbis_book_decode", "vorbis_book_decodev_add",
    "vorbis_book_decodevs_add", "vorbis_book_decodevv_add"};

// what native_vorbis_check found in them since its last report
struct BookCheck {
    std::array<uint64_t, size_t(Book::kCount)> calls{};
    std::array<uint64_t, size_t(Book::kCount)> different{};
    uint64_t floats = 0;
    std::string first;  // the first difference
};
BookCheck g_book_check;
Clock::time_point g_book_next_report{};

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
    // read first: the codebook functions are called ~10^5 times a second
    if (g_enabled_logged.load(std::memory_order_relaxed) != int(enabled) &&
        g_enabled_logged.exchange(int(enabled)) != int(enabled)) {
        REXLOG_INFO("Native Vorbis: {}",
                    enabled ? "on: the songs' inverse MDCT and codebook decoding run natively"
                            : "off: the game's inverse MDCT and codebook decoding run");
    }
    if (g_check_logged.load(std::memory_order_relaxed) != int(check) &&
        g_check_logged.exchange(int(check)) != int(check) && check) {
        REXLOG_INFO("Native Vorbis check: on: the game's and the native inverse MDCT and "
                    "codebook decoding both run on every call, the game's results are used, "
                    "and every 5 s the log says how far apart they came out");
        // counting from now: not on top of what it counted before it was
        // last turned off
        std::lock_guard lock(g_mutex);
        g_check = {};
        g_next_report = {};
        g_book_check = {};
        g_book_next_report = {};
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

// libvorbis 1.2.0's codebook and libogg 1.1.3's oggpack_buffer as the game
// lays them out (read off decode_packed_entry_number, oggpack_look and
// oggpack_adv)
constexpr uint32_t kBook_Dim = 0;
constexpr uint32_t kBook_Entries = 4;
constexpr uint32_t kBook_UsedEntries = 8;
constexpr uint32_t kBook_Valuelist = 16;
constexpr uint32_t kBook_Codelist = 20;
constexpr uint32_t kBook_DecIndex = 24;
constexpr uint32_t kBook_DecCodelengths = 28;
constexpr uint32_t kBook_DecFirsttable = 32;
constexpr uint32_t kBook_DecFirsttablen = 36;
constexpr uint32_t kBook_DecMaxlength = 40;
constexpr uint32_t kOpb_Endbyte = 0;
constexpr uint32_t kOpb_Endbit = 4;
constexpr uint32_t kOpb_Buffer = 8;
constexpr uint32_t kOpb_Ptr = 12;
constexpr uint32_t kOpb_Storage = 16;

// oggpack_look's mask table, which the native decoder has built in
constexpr uint32_t kLookMask = 0x82CADC98;
constexpr int kLookMaskWords = 33;

// floats one vector decode may add to: a residue partition's are a few
// hundred, so more than this is no call the game makes
constexpr int64_t kMaxFloats = 1 << 20;
// channels a decodevv_add may deal to: Vorbis has at most 255
constexpr int32_t kMaxChannels = 256;

// whether the game's mask table is the native one's: not read yet (-1), no, yes
std::atomic<int> g_mask{-1};
// set once its warning is logged, read first so a rejected call takes no lock
std::atomic<bool> g_book_unusable_logged{false};

// guest memory as the recompiled code addresses it, for the native decoder
VorbisGuest Guest(const uint8_t* base) { return {base, REX_PHYS_HOST_OFFSET(0xE0000000u)}; }

VorbisCodebook LoadBook(uint8_t* base, uint32_t book) {
    return {int32_t(REX_LOAD_U32(book + kBook_Dim)),
            int32_t(REX_LOAD_U32(book + kBook_Entries)),
            int32_t(REX_LOAD_U32(book + kBook_UsedEntries)),
            REX_LOAD_U32(book + kBook_Valuelist),
            REX_LOAD_U32(book + kBook_Codelist),
            REX_LOAD_U32(book + kBook_DecIndex),
            REX_LOAD_U32(book + kBook_DecCodelengths),
            REX_LOAD_U32(book + kBook_DecFirsttable),
            int32_t(REX_LOAD_U32(book + kBook_DecFirsttablen)),
            int32_t(REX_LOAD_U32(book + kBook_DecMaxlength))};
}

VorbisBitReader LoadReader(uint8_t* base, uint32_t opb) {
    return {REX_LOAD_U32(opb + kOpb_Endbyte), REX_LOAD_U32(opb + kOpb_Endbit),
            REX_LOAD_U32(opb + kOpb_Buffer), REX_LOAD_U32(opb + kOpb_Ptr),
            REX_LOAD_U32(opb + kOpb_Storage)};
}

// what oggpack_adv writes
void StoreReader(uint8_t* base, uint32_t opb, const VorbisBitReader& b) {
    REX_STORE_U32(opb + kOpb_Endbyte, b.endbyte);
    REX_STORE_U32(opb + kOpb_Endbit, b.endbit);
    REX_STORE_U32(opb + kOpb_Ptr, b.ptr);
}

// whether the game's mask table is the one the native decoder has built in,
// read once
bool MaskMatches(uint8_t* base) {
    const int known = g_mask.load(std::memory_order_relaxed);
    if (known >= 0) return known == 1;
    std::lock_guard lock(g_mutex);
    if (g_mask.load() < 0) {
        int match = 1;
        for (int i = 0; i < kLookMaskWords; i++) {
            const uint32_t address = kLookMask + 4 * uint32_t(i);
            const uint32_t game = REX_LOAD_U32(address);
            const uint32_t native = band3::audio::VorbisLookMask(i);
            if (game != native) {
                REXLOG_WARN("Native Vorbis: the game's oggpack_look mask {} at {:08X} is "
                            "{:08X}, not {:08X}; the game's codebook decoding runs",
                            i, address, game, native);
                match = 0;
                break;
            }
        }
        g_mask.store(match);
    }
    return g_mask.load() == 1;
}

// a call the native decoder doesn't take: the game's runs
void LogBookUnusable(Book fn, const VorbisCodebook& book, const VorbisBitReader& b,
                     const char* why) {
    if (g_book_unusable_logged.load(std::memory_order_relaxed)) return;
    std::lock_guard lock(g_mutex);
    if (g_book_unusable_logged.exchange(true)) return;
    REXLOG_WARN("Native Vorbis: a {} call the native codebook decoder doesn't take ({}: dim {}, "
                "{} of {} entries used, a first table of {} bits, codewords up to {} bits, the "
                "reader at byte {} bit {} of {}); the game's runs for it",
                kBookNames[size_t(fn)], why, book.dim, book.used_entries, book.entries,
                book.dec_firsttablen, book.dec_maxlength, b.endbyte, b.endbit, b.storage);
}

// whether the native decoder takes the book and the reader for fn
bool Takes(uint8_t* base, Book fn, const VorbisCodebook& book, const VorbisBitReader& b) {
    const char* why = nullptr;
    if (!band3::audio::VorbisCodebookDecodable(book)) {
        why = "the book";
    } else if (!band3::audio::VorbisBitReaderUsable(b)) {
        why = "the reader";
    } else if (fn == Book::kDecode && book.dec_index == 0) {
        why = "no entry index";
    } else if (fn >= Book::kDecodevAdd && !band3::audio::VorbisCodebookHasValues(book)) {
        why = "no values";
    }
    if (why) {
        LogBookUnusable(fn, book, b, why);
        return false;
    }
    return MaskMatches(base);
}

// a vector decoder's floats: the guest's, and the native decoder's copy of
// them under native_vorbis_check
struct Floats {
    uint32_t guest;
    const uint8_t* native;
    int64_t count;
    int32_t channel;
};

// native_vorbis_check: what the game's fn returned in r3, left in the reader
// at opb and in the floats, against the native one's
void CompareBook(uint8_t* base, Book fn, uint64_t game, uint64_t native, uint32_t opb,
                 const VorbisBitReader& reader, const Floats* floats, size_t regions) {
    const VorbisBitReader left = LoadReader(base, opb);
    std::string what;
    if (game != native) {
        what = std::format("returned {} where the game's returned {}", int64_t(native),
                           int64_t(game));
    } else if (left.endbyte != reader.endbyte || left.endbit != reader.endbit ||
               left.ptr != reader.ptr) {
        what = std::format("left the reader at byte {} bit {} (ptr {:08X}) where the game's "
                           "left it at byte {} bit {} (ptr {:08X})",
                           reader.endbyte, reader.endbit, reader.ptr, left.endbyte,
                           left.endbit, left.ptr);
    }
    uint64_t count = 0;
    for (size_t r = 0; r < regions; r++) {
        const Floats& f = floats[r];
        count += uint64_t(f.count);
        if (!what.empty() || f.count <= 0) continue;
        const uint8_t* guest = REX_RAW_ADDR(f.guest);
        if (std::memcmp(guest, f.native, size_t(f.count) * 4) == 0) continue;
        for (int64_t i = 0; i < f.count; i++) {
            uint32_t game_raw;
            uint32_t native_raw;
            std::memcpy(&game_raw, guest + 4 * i, sizeof game_raw);
            std::memcpy(&native_raw, f.native + 4 * i, sizeof native_raw);
            if (game_raw == native_raw) continue;
            what = std::format("made float {} of channel {} {:08X} where the game's made it "
                               "{:08X}",
                               i, f.channel, std::byteswap(native_raw), std::byteswap(game_raw));
            break;
        }
    }
    const auto now = Clock::now();
    std::lock_guard lock(g_mutex);
    BookCheck& c = g_book_check;
    c.calls[size_t(fn)]++;
    c.floats += count;
    if (!what.empty()) {
        c.different[size_t(fn)]++;
        if (c.first.empty()) c.first = std::string(kBookNames[size_t(fn)]) + " " + what;
    }
    if (g_book_next_report == Clock::time_point{}) g_book_next_report = now + kReportInterval;
    if (now < g_book_next_report) return;
    uint64_t different = 0;
    std::string calls;
    for (size_t i = 0; i < size_t(Book::kCount); i++) {
        if (!calls.empty()) calls += ", ";
        calls += std::format("{} {}", kBookNames[i], c.calls[i]);
        if (c.different[i]) calls += std::format(" ({} different)", c.different[i]);
        different += c.different[i];
    }
    if (different == 0) {
        REXLOG_INFO("Native Vorbis check: last 5 s codebook calls: {}; {} floats added; all the "
                    "same as the game's",
                    calls, c.floats);
    } else {
        REXLOG_WARN("Native Vorbis check: last 5 s codebook calls: {}; {} floats added; the "
                    "first different: {}",
                    calls, c.floats, c.first);
    }
    c = {};
    g_book_next_report = now + kReportInterval;
}

// the codebook hooks' start: whether either setting has the native decoder run
bool Wanted(bool& check) {
    const bool enabled = REXCVAR_GET(native_vorbis);
    check = REXCVAR_GET(native_vorbis_check);
    LogSettings(enabled, check);
    return enabled || check;
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
    const bool usable = band3::audio::VorbisMdctTablesValid(t) && apart;
    if (!usable) LogUnusable(t, in, out);
    if (!usable || !ConstantsMatch(base)) {
        __imp__mdct_backward(ctx, base);
        return;
    }

    // where the native one works: allocated by a thread's first call (the
    // decode thread's), not reserved in every thread of the process
    thread_local std::vector<float> samples;
    if (samples.empty()) samples.resize(kMaxSamples);
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

// decode_packed_entry_number(codebook* book, oggpack_buffer* b) (0x82C37078):
// the next codeword's sorted index, or -1; every residue value and floor value
// of a song goes through it, ~10^5 times a second
extern "C" void __imp__decode_packed_entry_number(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(decode_packed_entry_number) {
    bool check;
    if (!Wanted(check)) {
        __imp__decode_packed_entry_number(ctx, base);
        return;
    }
    const VorbisCodebook book = LoadBook(base, ctx.r3.u32);
    const uint32_t opb = ctx.r4.u32;
    VorbisBitReader b = LoadReader(base, opb);
    if (!Takes(base, Book::kPacked, book, b)) {
        __imp__decode_packed_entry_number(ctx, base);
        return;
    }
    const int32_t entry = band3::audio::VorbisDecodePackedEntryNumber(Guest(base), book, b);
    if (!check) {
        StoreReader(base, opb, b);
        ctx.r3.s64 = entry;
        return;
    }
    __imp__decode_packed_entry_number(ctx, base);
    CompareBook(base, Book::kPacked, ctx.r3.u64, uint64_t(int64_t(entry)), opb, b, nullptr, 0);
}

// vorbis_book_decode(codebook* book, oggpack_buffer* b) (0x82C37240): the next
// codeword's entry number, or -1 (the floor's values and the residue's
// partition classes)
extern "C" void __imp__vorbis_book_decode(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(vorbis_book_decode) {
    bool check;
    if (!Wanted(check)) {
        __imp__vorbis_book_decode(ctx, base);
        return;
    }
    const VorbisCodebook book = LoadBook(base, ctx.r3.u32);
    const uint32_t opb = ctx.r4.u32;
    VorbisBitReader b = LoadReader(base, opb);
    if (!Takes(base, Book::kDecode, book, b)) {
        __imp__vorbis_book_decode(ctx, base);
        return;
    }
    const int64_t entry = band3::audio::VorbisBookDecode(Guest(base), book, b);
    if (!check) {
        StoreReader(base, opb, b);
        ctx.r3.s64 = entry;
        return;
    }
    __imp__vorbis_book_decode(ctx, base);
    CompareBook(base, Book::kDecode, ctx.r3.u64, uint64_t(entry), opb, b, nullptr, 0);
}

// vorbis_book_decodev_add(codebook* book, float* a, oggpack_buffer* b, int n)
// (0x82C373A8) and vorbis_book_decodevs_add (0x82C37280), the same arguments:
// a residue partition's values added to a. f_01inverse calls them through a
// function pointer, which reaches these hooks as a direct call does: the
// SDK's indirect calls look the address up in a table filled with the same
// (weak, overridden) function symbols.
namespace {

using VectorDecoder = int32_t (*)(const VorbisGuest&, const VorbisCodebook&, uint8_t*,
                                  VorbisBitReader&, int32_t);
using Span = int64_t (*)(int32_t dim, int32_t n);

void DecodeVector(PPCContext& ctx, uint8_t* base, Book fn, PPCFunc* game, VectorDecoder native,
                  Span span) {
    bool check;
    if (!Wanted(check)) {
        game(ctx, base);
        return;
    }
    const VorbisCodebook book = LoadBook(base, ctx.r3.u32);
    const uint32_t a = ctx.r4.u32;
    const uint32_t opb = ctx.r5.u32;
    const int32_t n = ctx.r6.s32;
    VorbisBitReader b = LoadReader(base, opb);
    if (!Takes(base, fn, book, b)) {
        game(ctx, base);
        return;
    }
    const int64_t count = span(book.dim, n);
    if (count > kMaxFloats || (count > 0 && a == 0)) {
        LogBookUnusable(fn, book, b, count > kMaxFloats ? "too many values" : "no floats");
        game(ctx, base);
        return;
    }
    // the guest's adds keep denormals
    ctx.fpscr.disableFlushMode();
    if (!check) {
        ctx.r3.s64 = native(Guest(base), book, REX_RAW_ADDR(a), b, n);
        StoreReader(base, opb, b);
        return;
    }
    // the native one adds to a copy of every float the game's can add to
    thread_local std::vector<uint8_t> copy;
    copy.resize(size_t(count) * 4);
    if (count > 0) std::memcpy(copy.data(), REX_RAW_ADDR(a), copy.size());
    const int32_t r = native(Guest(base), book, copy.data(), b, n);
    game(ctx, base);
    const Floats floats{a, copy.data(), count, 0};
    CompareBook(base, fn, ctx.r3.u64, uint64_t(int64_t(r)), opb, b, &floats, 1);
}

}

extern "C" void __imp__vorbis_book_decodev_add(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(vorbis_book_decodev_add) {
    DecodeVector(ctx, base, Book::kDecodevAdd, __imp__vorbis_book_decodev_add,
                 band3::audio::VorbisBookDecodevAdd, band3::audio::VorbisDecodevAddSpan);
}

extern "C" void __imp__vorbis_book_decodevs_add(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(vorbis_book_decodevs_add) {
    DecodeVector(ctx, base, Book::kDecodevsAdd, __imp__vorbis_book_decodevs_add,
                 band3::audio::VorbisBookDecodevsAdd, band3::audio::VorbisDecodevsAddSpan);
}

// vorbis_book_decodevv_add(codebook* book, float** a, long offset, int ch,
// oggpack_buffer* b, int n) (0x82C37678): res2_inverse's interleaved
// residue, each entry's values dealt across the ch channels
extern "C" void __imp__vorbis_book_decodevv_add(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(vorbis_book_decodevv_add) {
    bool check;
    if (!Wanted(check)) {
        __imp__vorbis_book_decodevv_add(ctx, base);
        return;
    }
    const VorbisCodebook book = LoadBook(base, ctx.r3.u32);
    const uint32_t a = ctx.r4.u32;
    const int32_t offset = ctx.r5.s32;
    const int32_t ch = ctx.r6.s32;
    const uint32_t opb = ctx.r7.u32;
    const int32_t n = ctx.r8.s32;
    VorbisBitReader b = LoadReader(base, opb);
    if (!Takes(base, Book::kDecodevvAdd, book, b)) {
        __imp__vorbis_book_decodevv_add(ctx, base);
        return;
    }
    if (ch <= 0 || ch > kMaxChannels) {
        LogBookUnusable(Book::kDecodevvAdd, book, b, "the channels");
        __imp__vorbis_book_decodevv_add(ctx, base);
        return;
    }
    // each channel's floats from offset / ch: where the game's adds, and how
    // many it can; a channel it never reaches isn't looked up, as in the guest
    const uint32_t first = uint32_t(offset / ch);
    thread_local std::vector<Floats> floats;
    floats.resize(size_t(ch));
    int64_t total = 0;
    bool usable = true;
    for (int32_t c = 0; c < ch; c++) {
        const int64_t count = band3::audio::VorbisDecodevvAddSpan(book.dim, offset, ch, n, c);
        const uint32_t channel = count > 0 ? REX_LOAD_U32(a + 4 * uint32_t(c)) : 0;
        floats[size_t(c)] = {channel + 4 * first, nullptr, count, c};
        total += count;
        usable = usable && (count == 0 || channel != 0);
    }
    if (!usable || total > kMaxFloats) {
        LogBookUnusable(Book::kDecodevvAdd, book, b,
                        total > kMaxFloats ? "too many values" : "no floats");
        __imp__vorbis_book_decodevv_add(ctx, base);
        return;
    }
    thread_local std::vector<uint8_t*> channels;
    channels.resize(size_t(ch));
    // the guest's adds keep denormals
    ctx.fpscr.disableFlushMode();
    if (!check) {
        for (int32_t c = 0; c < ch; c++) {
            const Floats& f = floats[size_t(c)];
            channels[size_t(c)] = f.count > 0 ? REX_RAW_ADDR(f.guest) : nullptr;
        }
        ctx.r3.s64 = band3::audio::VorbisBookDecodevvAdd(Guest(base), book, channels.data(),
                                                          offset, ch, b, n);
        StoreReader(base, opb, b);
        return;
    }
    // the native one adds to a copy of every float the game's can add to
    thread_local std::vector<uint8_t> copy;
    copy.resize(size_t(total) * 4);
    size_t at = 0;
    for (int32_t c = 0; c < ch; c++) {
        Floats& f = floats[size_t(c)];
        f.native = copy.data() + at;
        channels[size_t(c)] = copy.data() + at;
        if (f.count > 0) std::memcpy(copy.data() + at, REX_RAW_ADDR(f.guest), size_t(f.count) * 4);
        at += size_t(f.count) * 4;
    }
    const int32_t r = band3::audio::VorbisBookDecodevvAdd(Guest(base), book, channels.data(),
                                                         offset, ch, b, n);
    __imp__vorbis_book_decodevv_add(ctx, base);
    CompareBook(base, Book::kDecodevvAdd, ctx.r3.u64, uint64_t(int64_t(r)), opb, b,
                floats.data(), floats.size());
}
