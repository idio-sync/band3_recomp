#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include "src/Game/Modifiers.h"
#include "src/Game/Script.h"

// RB3Enhanced's six modifiers: added to the game's list when ModifierMgr is
// made, and applied where RB3E applies them (source/rb3enhanced.c,
// source/GemHooks.c, source/SongParserHooks.c; the black background is in
// the venue hook in src/patches.cpp). Layouts are rb3-xenon's (GameGem.h,
// SongParser.h) for the same TU5 executable.

extern "C" void __imp__ModifierManager____ct(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__GameGemList__WillBeNoStrum(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__GameGemList__AddGameGem(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__GemManager__GetWidgetByName(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__TrackConfig__GetSlotColor(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__SongParser__PitchToSlot(PPCContext& ctx, uint8_t* base);
REX_EXTERN(RandomInt);

namespace {

using band3::modifiers::Active;
using band3::modifiers::Intern;

constexpr uint32_t kGameGem_Slots = 0xC;        // unsigned int, a bit per lane, green first
constexpr uint32_t kSongParser_TrackType = 0xF8;  // TrackType
constexpr uint32_t kTrackDrum = 0;
// the 2x bass pedal's MIDI note: one below expert's kick (96), so the game
// finds no difficulty for it (the loop ends at 4) and slot -1; RB3E makes it
// an expert (3) kick (slot 0)
constexpr int32_t kDoubleBassPitch = 95;
constexpr uint32_t kNoDifficulty = 4;
constexpr uint32_t kExpert = 3;
constexpr int kLanes = 5;

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

void Store32(uint8_t* base, uint32_t addr, uint32_t value) {
    *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr) = value;
}

// RandomInt(min, max), in [min, max): the game's random numbers, so
// --test_random_seed repeats the shuffles
int32_t Random(const PPCContext& ctx, uint8_t* base, int32_t min, int32_t max) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - 0x400;
    call.r13 = ctx.r13;
    call.r3.u64 = static_cast<uint32_t>(min);
    call.r4.u64 = static_cast<uint32_t>(max);
    RandomInt(call, base);
    return call.r3.s32;
}

// `name`'s index in `group`, or -1
template <size_t N>
int IndexIn(const char* const (&group)[N], const char* name) {
    for (size_t i = 0; i < N; i++) {
        if (std::strcmp(group[i], name) == 0) return static_cast<int>(i);
    }
    return -1;
}

// the gem widgets colour shuffle swaps within: each group's own kind, so a
// beat line, kick, key or cymbal never becomes a plain gem
constexpr const char* kGuitarGems[] = {"gem_green.wid", "gem_red.wid", "gem_yellow.wid",
                                       "gem_blue.wid", "gem_orange.wid"};
constexpr const char* kGuitarHopoGems[] = {"gem_green_hopo.wid", "gem_red_hopo.wid",
                                           "gem_yellow_hopo.wid", "gem_blue_hopo.wid",
                                           "gem_orange_hopo.wid"};
constexpr const char* kDrumGems[] = {"drum_red.wid", "drum_yellow.wid", "drum_blue.wid",
                                     "drum_green.wid"};
constexpr const char* kCymbalGems[] = {"cymbal_gem_red.wid", "cymbal_gem_yellow.wid",
                                       "cymbal_gem_blue.wid", "cymbal_gem_green.wid"};
constexpr const char* kSlotColors[] = {"green", "red", "yellow", "blue", "orange"};

// a random member of the group `name` is in, or 0 when it's in none
template <size_t N>
uint32_t ShuffledFrom(PPCContext& ctx, uint8_t* base, const char* const (&group)[N],
                      const char* name) {
    if (IndexIn(group, name) < 0) return 0;
    return Intern(ctx, base, group[Random(ctx, base, 0, static_cast<int32_t>(N))]);
}

}

// ModifierMgr's constructor reads $syscfg's modifiers list, so RB3E's go on
// the end of it first. Without use_save_value: RB3E warns that saving them
// makes vanilla Wii profiles unloadable.
extern "C" REX_FUNC(ModifierManager____ct)
{
    for (const char* name : band3::modifiers::kRb3eModifiers) {
        band3::RunScript(ctx, base,
                         std::string("{do {push_back {find $syscfg modifiers modifiers} (") +
                             name + ")}}");
    }
    REXLOG_INFO("Added RB3Enhanced's modifiers");
    __imp__ModifierManager____ct(ctx, base);
}

// GameGemList::WillBeNoStrum(GameGemList*, MultiGemInfo*): force HOPOs makes
// every gem one
extern "C" REX_FUNC(GameGemList__WillBeNoStrum)
{
    if (Active(ctx, base, "mod_force_hopos")) {
        ctx.r3.u64 = 1;
        return;
    }
    __imp__GameGemList__WillBeNoStrum(ctx, base);
}

// GameGemList::AddGameGem(GameGemList*, GameGem*, NoStrumState): gem shuffle
// shuffles the gem's five lanes, and mirror mode then swaps green with orange
// and red with blue. (RB3E mirrors the gem as it was before the shuffle, which
// undoes all but yellow's shuffle when both are on.)
extern "C" REX_FUNC(GameGemList__AddGameGem)
{
    const uint32_t gem = ctx.r4.u32;
    const bool shuffle = Active(ctx, base, "mod_gem_shuffle");
    const bool mirror = Active(ctx, base, "mod_mirror_mode");
    if (gem && (shuffle || mirror)) {
        const uint32_t slots = Load32(base, gem + kGameGem_Slots);
        bool lanes[kLanes];
        for (int i = 0; i < kLanes; i++) lanes[i] = (slots >> i) & 1;
        if (shuffle) {
            for (int i = kLanes - 1; i > 0; i--) {
                const int j = Random(ctx, base, 0, i + 1);
                std::swap(lanes[i], lanes[j]);
            }
        }
        if (mirror) {
            std::swap(lanes[0], lanes[4]);
            std::swap(lanes[1], lanes[3]);
        }
        uint32_t out = slots & ~((1u << kLanes) - 1);
        for (int i = 0; i < kLanes; i++) out |= uint32_t(lanes[i]) << i;
        Store32(base, gem + kGameGem_Slots, out);
    }
    __imp__GameGemList__AddGameGem(ctx, base);
}

// GemManager::GetWidgetByName(GemManager*, Symbol): colour shuffle draws each
// gem with a random widget of its kind
extern "C" REX_FUNC(GemManager__GetWidgetByName)
{
    const char* name = ctx.r4.u32 ? rex::memory::GuestPtr<const char*>(base, ctx.r4.u32) : "";
    const bool gem = IndexIn(kGuitarGems, name) >= 0 || IndexIn(kGuitarHopoGems, name) >= 0 ||
                     IndexIn(kDrumGems, name) >= 0 || IndexIn(kCymbalGems, name) >= 0;
    if (gem && Active(ctx, base, "mod_color_shuffle")) {
        uint32_t shuffled = ShuffledFrom(ctx, base, kGuitarGems, name);
        if (!shuffled) shuffled = ShuffledFrom(ctx, base, kGuitarHopoGems, name);
        if (!shuffled) shuffled = ShuffledFrom(ctx, base, kDrumGems, name);
        if (!shuffled) shuffled = ShuffledFrom(ctx, base, kCymbalGems, name);
        if (shuffled) ctx.r4.u64 = shuffled;
    }
    __imp__GemManager__GetWidgetByName(ctx, base);
}

// TrackConfig::GetSlotColor(TrackConfig*, int slot) -> const char*: and its
// sustain tail a random colour too. Only the five guitar colours, so a pro
// keys gem never gets a guitar tail (which doesn't draw).
extern "C" REX_FUNC(TrackConfig__GetSlotColor)
{
    __imp__TrackConfig__GetSlotColor(ctx, base);
    const char* color = ctx.r3.u32 ? rex::memory::GuestPtr<const char*>(base, ctx.r3.u32) : "";
    if (IndexIn(kSlotColors, color) >= 0 && Active(ctx, base, "mod_color_shuffle")) {
        if (const uint32_t shuffled = ShuffledFrom(ctx, base, kSlotColors, color)) {
            ctx.r3.u64 = shuffled;
        }
    }
}

// SongParser::PitchToSlot(SongParser*, int pitch, int& difficulty, int tick):
// double bass reads the drums' 2x bass pedal note as an expert kick
extern "C" REX_FUNC(SongParser__PitchToSlot)
{
    const uint32_t parser = ctx.r3.u32;
    const int32_t pitch = ctx.r4.s32;
    const uint32_t difficulty = ctx.r5.u32;
    const int32_t tick = ctx.r6.s32;
    __imp__SongParser__PitchToSlot(ctx, base);
    if (pitch == kDoubleBassPitch && difficulty &&
        Load32(base, parser + kSongParser_TrackType) == kTrackDrum &&
        Load32(base, difficulty) == kNoDifficulty && Active(ctx, base, "mod_double_bass")) {
        Store32(base, difficulty, kExpert);
        ctx.r3.u64 = 0;  // the kick's slot
        REXLOG_DEBUG("Double bass: an expert kick at tick {}", tick);
    }
}
