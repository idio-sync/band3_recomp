#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "generated/band3_init.h"
#include "src/Game/Modifiers.h"
#include "src/History/score_history.h"
#include "src/Hooks/speed.h"
#include "src/Test/test_server.h"
#include "src/config.h"
#include "src/settings.h"

// Score history (src/History/score_history.h): each player's results when a
// song is won, from MetaPerformer::TriggerSongCompletion, which the song's
// script calls once the band finishes it (rb3-xenon,
// src/band3/meta_band/MetaPerformer.cpp). It walks the band's active players,
// guests included, where SongStatusMgr only hears about players with a
// profile. The players are read before it runs, while MetaPerformer's
// cheating flag (which it clears) still says whether autoplay was used, and
// kept only if it goes on to MetaPerformer::CompleteSong: it returns early
// without completing anything in some states.
//
// Offsets are TU5's, checked in the recompiled code (generated/): the Stats
// getters and setters for Stats', TriggerSongCompletion for the rest.

extern "C" void __imp__MetaPerformer__TriggerSongCompletion(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__MetaPerformer__CompleteSong(PPCContext& ctx, uint8_t* base);
REX_EXTERN(MetaPerformer__GetScoreTypeForUser);
REX_EXTERN(MetaPerformer__IsNoFailActive);
REX_EXTERN(MetaPerformer__Song);
REX_EXTERN(BandSongMgr__GetSongIDFromShortname);
REX_EXTERN(BandSongMgr__Data);
REX_EXTERN(Performer__GetScore);

namespace {

namespace history = band3::history;

// globals
constexpr uint32_t kTheGamePtr = 0x82E02128;  // Game*
constexpr uint32_t kTheSongMgr = 0x82DFE7B4;  // BandSongMgr object

// struct offsets
constexpr uint32_t kGame_Band = 0xDC;                     // Band*, Game::GetBand
constexpr uint32_t kBand_ActivePlayers = 0x2C;            // std::vector<Player*>
constexpr uint32_t kMetaPerformer_Cheating = 0x350;       // bool
constexpr uint32_t kPlayer_Stats = 0x10;                  // Stats, in Performer
constexpr uint32_t kPlayer_Quarantined = 0x230;           // bool, in Performer
constexpr uint32_t kPlayer_User = 0x260;                  // BandUser*
constexpr uint32_t kPlayerVtable_GetNumStars = 36;        // int GetNumStars() const
constexpr uint32_t kPlayerVtable_IsAutoplay = 204;        // bool IsAutoplay() const
constexpr uint32_t kBandUser_Difficulty = 0x8;
constexpr uint32_t kStats_NotesHitFraction = 0x18;        // float
constexpr uint32_t kStats_DeployCount = 0x20;
constexpr uint32_t kStats_SoloPercentage = 0x38;          // the best solo
constexpr uint32_t kStats_HasSolos = 0xB5;                // bool
constexpr uint32_t kStats_HitStreaks = 0xD0;              // std::vector<StreakInfo>, longest first
constexpr uint32_t kStats_UnisonCompleted = 0x198;
constexpr uint32_t kStats_UnisonCount = 0x19C;
constexpr uint32_t kStreakInfo_Duration = 0x4;
constexpr uint32_t kSongMetadata_Title = 0x4C;            // String {vtable, length, buf}
constexpr uint32_t kSongMetadata_Artist = 0x58;
constexpr uint32_t kString_Length = 0x4;
constexpr uint32_t kString_Buf = 0x8;

// RB3's ScoreType, in order (rb3, src/band3/game/Defines.h)
constexpr const char* kScoreTypes[] = {"drum",      "bass",        "guitar",    "vocals",
                                       "harmony",   "keys",        "real_drum", "real_guitar",
                                       "real_bass", "real_keys"};

// RB3Enhanced's modifiers that change what's played (Modifiers.h), by the
// names a run keeps
constexpr struct {
    const char* game;
    const char* name;
} kGameplayModifiers[] = {
    {"mod_force_hopos", "force_hopos"},     {"mod_mirror_mode", "mirror_mode"},
    {"mod_color_shuffle", "color_shuffle"}, {"mod_gem_shuffle", "gem_shuffle"},
    {"mod_double_bass", "double_bass"},
};

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

uint8_t Load8(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<uint8_t*>(base, addr);
}

float LoadFloat(uint8_t* base, uint32_t addr) {
    return std::bit_cast<float>(Load32(base, addr));
}

const char* GuestStr(uint8_t* base, uint32_t addr) {
    return addr ? rex::memory::GuestPtr<const char*>(base, addr) : nullptr;
}

std::string ReadGameString(uint8_t* base, uint32_t string_addr) {
    const uint32_t length = Load32(base, string_addr + kString_Length);
    const uint32_t buf = Load32(base, string_addr + kString_Buf);
    if (!buf) return {};
    return std::string(GuestStr(base, buf), std::min<uint32_t>(length, 1024));
}

// a stack below the hooked function's frame, with the caller's r13
PPCContext CallContext(const PPCContext& ctx) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - 0x100;
    call.r13 = ctx.r13;
    return call;
}

// a virtual of the object at `object` returning in r3
uint32_t CallVirtual(const PPCContext& outer, uint8_t* base, uint32_t object, uint32_t slot) {
    const uint32_t vtable = Load32(base, object);
    PPCContext ctx = CallContext(outer);
    ctx.r3.u64 = object;
    REX_CALL_INDIRECT_FUNC(Load32(base, vtable + slot));
    return ctx.r3.u32;
}

struct Song {
    std::string shortname;
    int32_t id = 0;
    std::string title;
    std::string artist;
};

Song ReadSong(const PPCContext& ctx, uint8_t* base, uint32_t meta_performer) {
    Song song;
    // MetaPerformer::Song(Symbol* out, MetaPerformer*)
    PPCContext call = CallContext(ctx);
    const uint32_t out = call.r1.u32 + 0x80;
    call.r3.u64 = out;
    call.r4.u64 = meta_performer;
    MetaPerformer__Song(call, base);
    const uint32_t shortname = Load32(base, out);
    if (!shortname) return song;
    song.shortname = GuestStr(base, shortname);

    // BandSongMgr::GetSongIDFromShortname(BandSongMgr*, Symbol, int fail)
    call = CallContext(ctx);
    call.r3.u64 = kTheSongMgr;
    call.r4.u64 = shortname;
    call.r5.u64 = 1;
    BandSongMgr__GetSongIDFromShortname(call, base);
    song.id = call.r3.s32;

    // BandSongMgr::Data(BandSongMgr*, int song_id) -> SongMetadata*
    call = CallContext(ctx);
    call.r3.u64 = kTheSongMgr;
    call.r4.u64 = static_cast<uint32_t>(song.id);
    BandSongMgr__Data(call, base);
    if (const uint32_t metadata = call.r3.u32) {
        song.title = ReadGameString(base, metadata + kSongMetadata_Title);
        song.artist = ReadGameString(base, metadata + kSongMetadata_Artist);
    }
    return song;
}

std::vector<std::string> ReadModifiers(PPCContext& ctx, uint8_t* base, uint32_t meta_performer) {
    std::vector<std::string> out;
    // MetaPerformer::IsNoFailActive(MetaPerformer*) -> bool
    PPCContext call = CallContext(ctx);
    call.r3.u64 = meta_performer;
    MetaPerformer__IsNoFailActive(call, base);
    if (call.r3.u32 & 0xFF) out.push_back("no_fail");
    for (const auto& m : kGameplayModifiers) {
        if (band3::modifiers::Active(ctx, base, m.game)) out.push_back(m.name);
    }
    return out;
}

// whether this session keeps history: a harness session only in
// test_score_history's file, with autoplay runs (which it plays with)
bool Recording() {
    if (!history::Get()) return false;
    return band3::test::Enabled() || REXCVAR_GET(score_history);
}

// the runs TriggerSongCompletion read, kept once it reaches CompleteSong
bool g_completing = false;
std::vector<history::Run> g_pending;

std::vector<history::Run> ReadRuns(PPCContext& ctx, uint8_t* base, uint32_t meta_performer) {
    std::vector<history::Run> runs;
    const uint32_t game = Load32(base, kTheGamePtr);
    if (!game) return runs;
    const uint32_t band = Load32(base, game + kGame_Band);
    if (!band) return runs;
    const uint32_t players_begin = Load32(base, band + kBand_ActivePlayers);
    const uint32_t players_end = Load32(base, band + kBand_ActivePlayers + 4);
    if (!players_begin || players_end < players_begin) return runs;
    const int band_size = static_cast<int>((players_end - players_begin) / 4);
    if (band_size < 1 || band_size > 4) return runs;

    const Song song = ReadSong(ctx, base, meta_performer);
    if (song.shortname.empty()) return runs;
    const bool harness = band3::test::Enabled();
    // autoplay at any point in the song (TriggerSongCompletion's own test), or
    // band3's autoplay setting
    const bool cheating =
        Load8(base, meta_performer + kMetaPerformer_Cheating) != 0 || REXCVAR_GET(autoplay);
    const std::vector<std::string> modifiers = ReadModifiers(ctx, base, meta_performer);
    const int speed = static_cast<int>(std::lround(band3::speed::MusicSpeed() * 100));
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();

    for (int i = 0; i < band_size; i++) {
        const uint32_t player = Load32(base, players_begin + i * 4);
        if (!player) continue;
        const uint32_t user = Load32(base, player + kPlayer_User);
        // a player who dropped out mid-song
        if (!user || Load8(base, player + kPlayer_Quarantined)) continue;

        history::Run run;
        run.autoplay = cheating || (CallVirtual(ctx, base, player, kPlayerVtable_IsAutoplay) & 0xFF);
        if (run.autoplay && !harness) continue;

        // MetaPerformer::GetScoreTypeForUser(MetaPerformer*, BandUser*) -> ScoreType
        PPCContext call = CallContext(ctx);
        call.r3.u64 = meta_performer;
        call.r4.u64 = user;
        MetaPerformer__GetScoreTypeForUser(call, base);
        const uint32_t score_type = call.r3.u32;
        if (score_type >= std::size(kScoreTypes)) continue;

        // Performer::GetScore(Performer*) -> int, 0 for a no-score failure
        call = CallContext(ctx);
        call.r3.u64 = player;
        Performer__GetScore(call, base);
        run.score = call.r3.s32;

        const uint32_t stats = player + kPlayer_Stats;
        run.time = now;
        run.shortname = song.shortname;
        run.song_id = song.id;
        run.title = song.title;
        run.artist = song.artist;
        run.part = kScoreTypes[score_type];
        run.diff = static_cast<int>(Load32(base, user + kBandUser_Difficulty));
        run.stars = static_cast<int>(CallVirtual(ctx, base, player, kPlayerVtable_GetNumStars));
        // floored, as the results screen shows it: 100 only for every note
        run.pct = static_cast<int>(std::floor(LoadFloat(base, stats + kStats_NotesHitFraction) * 100));
        const uint32_t streaks = Load32(base, stats + kStats_HitStreaks);
        const uint32_t streaks_end = Load32(base, stats + kStats_HitStreaks + 4);
        run.streak = streaks && streaks_end > streaks
                         ? static_cast<int>(Load32(base, streaks + kStreakInfo_Duration))
                         : 0;
        run.od_deploys = static_cast<int>(Load32(base, stats + kStats_DeployCount));
        if (Load8(base, stats + kStats_HasSolos)) {
            run.solo_pct = static_cast<int>(Load32(base, stats + kStats_SoloPercentage));
        }
        if (band_size > 1 && Load32(base, stats + kStats_UnisonCount) > 0) {
            run.unison = static_cast<int>(Load32(base, stats + kStats_UnisonCompleted));
        }
        run.band_size = band_size;
        run.speed = speed;
        run.modifiers = modifiers;

        if (!history::Plausible(run)) {
            REXLOG_WARN("Score history: skipping an implausible {} {} run of {} (score {}, stars "
                        "{}, {}%)",
                        run.part, run.diff, run.shortname, run.score, run.stars, run.pct);
            continue;
        }
        runs.push_back(std::move(run));
    }
    return runs;
}

}  // namespace

// MetaPerformer::TriggerSongCompletion(MetaPerformer*)
extern "C" REX_FUNC(MetaPerformer__TriggerSongCompletion)
{
    if (!Recording()) return __imp__MetaPerformer__TriggerSongCompletion(ctx, base);
    g_pending = ReadRuns(ctx, base, ctx.r3.u32);
    g_completing = true;
    __imp__MetaPerformer__TriggerSongCompletion(ctx, base);
    g_completing = false;
    g_pending.clear();
}

// MetaPerformer::CompleteSong(MetaPerformer*, std::vector<BandUser*>&, const
// BandStatsInfo*, bool): the song counted as finished
extern "C" REX_FUNC(MetaPerformer__CompleteSong)
{
    __imp__MetaPerformer__CompleteSong(ctx, base);
    if (!g_completing) return;
    g_completing = false;
    history::Store* store = history::Get();
    if (!store) return;
    for (auto& run : g_pending) {
        REXLOG_INFO("Score history: {} {} {} - {} points, {} stars, {}%", run.shortname, run.part,
                    run.diff, run.score, run.stars, run.pct);
        store->Append(std::move(run));
    }
    g_pending.clear();
}

namespace band3::history {

// score_history.jsonl in the user data folder, or a harness session's
// test_score_history file; called once the folders are final
void OpenForGame() {
    if (band3::test::Enabled()) {
        const std::string path = REXCVAR_GET(test_score_history);
        if (!path.empty()) Open(rex::to_path(path));
        return;
    }
    const auto root = band3::UserDataRoot();
    if (!root.empty()) Open(root / "score_history.jsonl");
}

}
