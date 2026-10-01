#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <cstdint>
#include <string>
#include <vector>
#include "src/settings.h"

// RB3Enhanced's UnlockClothing and AllowGoldOnAllDifficulties
// (source/rb3enhanced.c's ApplyConfigurablePatches). RB3E rewrites
// instructions at fixed addresses; a recompiled game can't, so each patch is
// a hook on the function around it that has the same effect. Layouts are
// rb3-xenon's (game/Scoring.h) for the same TU5 executable.

extern "C" void __imp__ProfileAssets__HasAsset(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__BandProfile__HasCampaignKey(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__CustomizePanel__Load(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__MetaPerformer__SelectRandomVenue(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__Scoring__ComputeStarThresholds(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__Scoring__GetSoloNumStarsFloat(PPCContext& ctx, uint8_t* base);
REX_EXTERN(SongDB__GetBaseScores);
REX_EXTERN(Scoring__GetPlayerScoreInfo);
REX_EXTERN(Scoring__GetNumStarsFloat);

namespace {

constexpr uint32_t kTheSongDBPtr = 0x82E023F8;  // SongDB*
// PlayerScoreInfo, 32 bytes: mDifficulty at 4, mSoloStarThresholds (a
// vector<int>, begin and end) at 20
constexpr uint32_t kPlayerScoreInfo_Size = 32;
constexpr uint32_t kPlayerScoreInfo_Difficulty = 4;
constexpr uint32_t kPlayerScoreInfo_SoloThresholds = 20;
constexpr uint32_t kExpert = 3;
// what ComputeStarThresholds sets a star it rules out to
constexpr uint32_t kUnreachable = 999999999;

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

void Store32(uint8_t* base, uint32_t addr, uint32_t value) {
    *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr) = value;
}

PPCContext CallContext(const PPCContext& ctx) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - 0x100;
    call.r13 = ctx.r13;
    return call;
}

// set while CustomizePanel::Load or MetaPerformer::SelectRandomVenue runs:
// their BandProfile::HasCampaignKey calls are the face paint, tattoo and video
// venue checks RB3E patches, and nothing else
thread_local int g_unlock_scope = 0;

struct UnlockScope {
    UnlockScope() { g_unlock_scope++; }
    ~UnlockScope() { g_unlock_scope--; }
};

}

// ProfileAssets::HasAsset(ProfileAssets*, Symbol): whether a piece of clothing
// has been earned; RB3E makes it always true
extern "C" REX_FUNC(ProfileAssets__HasAsset)
{
    if (REXCVAR_GET(unlock_clothing)) {
        ctx.r3.u64 = 1;
        return;
    }
    __imp__ProfileAssets__HasAsset(ctx, base);
}

// BandProfile::HasCampaignKey(BandProfile*, Symbol), true inside the two
// functions above with unlock_clothing on, as RB3E's li r3,1 at those calls
extern "C" REX_FUNC(BandProfile__HasCampaignKey)
{
    if (g_unlock_scope == 0) {
        __imp__BandProfile__HasCampaignKey(ctx, base);
        return;
    }
    const uint32_t key = ctx.r4.u32;
    if (REXCVAR_GET(unlock_clothing)) {
        ctx.r3.u64 = 1;
    } else {
        __imp__BandProfile__HasCampaignKey(ctx, base);
    }
    REXLOG_DEBUG("Campaign key {}: {}",
                 key ? rex::memory::GuestPtr<const char*>(base, key) : "(none)",
                 ctx.r3.u8 ? "unlocked" : "locked");
}

// face paint and tattoos
extern "C" REX_FUNC(CustomizePanel__Load)
{
    UnlockScope scope;
    __imp__CustomizePanel__Load(ctx, base);
}

// video venues
extern "C" REX_FUNC(MetaPerformer__SelectRandomVenue)
{
    UnlockScope scope;
    __imp__MetaPerformer__SelectRandomVenue(ctx, base);
}

// Scoring::ComputeStarThresholds(Scoring*, bool debug). Two of RB3E's gold
// patches are in it: it rules out the band's stars from 6 on (gold) unless
// every player is on expert, and each player's stars whose threshold works
// out to 0. Every player looks expert while it runs (it reads their
// difficulty for nothing else), and afterwards each player's ruled-out stars
// go back to 0, the threshold it computed, which is what NOPing that store
// leaves; nothing else stores 999999999 in a player's thresholds.
extern "C" REX_FUNC(Scoring__ComputeStarThresholds)
{
    if (!REXCVAR_GET(gold_on_all_difficulties)) {
        __imp__Scoring__ComputeStarThresholds(ctx, base);
        return;
    }

    // SongDB::GetBaseScores(SongDB*) -> vector<PlayerScoreInfo>&
    uint32_t infos = 0, count = 0;
    if (const uint32_t song_db = Load32(base, kTheSongDBPtr)) {
        PPCContext call = CallContext(ctx);
        call.r3.u64 = song_db;
        SongDB__GetBaseScores(call, base);
        const uint32_t scores = call.r3.u32;
        infos = Load32(base, scores);
        count = (Load32(base, scores + 4) - infos) / kPlayerScoreInfo_Size;
    }
    const uint32_t scoring = ctx.r3.u32;
    std::vector<uint32_t> difficulties(count);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t at = infos + i * kPlayerScoreInfo_Size + kPlayerScoreInfo_Difficulty;
        difficulties[i] = Load32(base, at);
        Store32(base, at, kExpert);
    }

    __imp__Scoring__ComputeStarThresholds(ctx, base);

    for (uint32_t i = 0; i < count; i++) {
        const uint32_t info = infos + i * kPlayerScoreInfo_Size;
        Store32(base, info + kPlayerScoreInfo_Difficulty, difficulties[i]);
        const uint32_t begin = Load32(base, info + kPlayerScoreInfo_SoloThresholds);
        const uint32_t end = Load32(base, info + kPlayerScoreInfo_SoloThresholds + 4);
        std::string stars;
        for (uint32_t at = begin; at < end; at += 4) {
            if (Load32(base, at) == kUnreachable) Store32(base, at, 0);
            stars += " " + std::to_string(Load32(base, at));
        }
        REXLOG_DEBUG("Gold on all difficulties: player {} (difficulty {}) star scores{}", i,
                     difficulties[i], stars);
    }
    // Scoring::mStarThresholds, the band's (a vector<int> at 192)
    std::string band;
    for (uint32_t at = Load32(base, scoring + 192); at < Load32(base, scoring + 196); at += 4) {
        band += " " + std::to_string(Load32(base, at));
    }
    REXLOG_DEBUG("Gold on all difficulties: band star scores{}", band);
}

namespace {

// Scoring::GetSoloNumStarsFloat without its cap: GetNumStarsFloat of the
// player's own thresholds, as the original computes before capping
void UncappedSoloStars(PPCContext& ctx, uint8_t* base) {
    const uint32_t scoring = ctx.r3.u32;
    const uint32_t score = ctx.r4.u32;

    // Scoring::GetPlayerScoreInfo(Scoring*, TrackType) -> PlayerScoreInfo*
    PPCContext call = CallContext(ctx);
    call.r3.u64 = scoring;
    call.r4.u64 = ctx.r5.u32;
    Scoring__GetPlayerScoreInfo(call, base);
    const uint32_t info = call.r3.u32;
    if (!info) {
        __imp__Scoring__GetSoloNumStarsFloat(ctx, base);
        return;
    }

    // Scoring::GetNumStarsFloat(Scoring*, int score, const vector<int>& thresholds)
    call = CallContext(ctx);
    call.r3.u64 = scoring;
    call.r4.u64 = score;
    call.r5.u64 = info + kPlayerScoreInfo_SoloThresholds;
    Scoring__GetNumStarsFloat(call, base);
    ctx.f1.f64 = call.f1.f64;
}

}

// Scoring::GetSoloNumStarsFloat(Scoring*, int score, TrackType): caps a
// player who isn't on expert at 5 stars, which RB3E's third patch removes
extern "C" REX_FUNC(Scoring__GetSoloNumStarsFloat)
{
    const uint32_t score = ctx.r4.u32;
    if (REXCVAR_GET(gold_on_all_difficulties)) {
        UncappedSoloStars(ctx, base);
    } else {
        __imp__Scoring__GetSoloNumStarsFloat(ctx, base);
    }
    thread_local double last = -1;
    if (ctx.f1.f64 != last) {
        last = ctx.f1.f64;
        REXLOG_DEBUG("Solo stars: {:.2f} at score {}", ctx.f1.f64, score);
    }
}
