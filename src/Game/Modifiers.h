#pragma once
#include <cstdint>

struct PPCContext;

// The game's modifiers (ModifierMgr), and RB3Enhanced's six, which band3 adds
// to them as RB3E does. These call the game, so they run on a guest thread,
// from a hook.

namespace band3::modifiers {

// RB3E's modifiers (source/rb3enhanced.c's ModifierManagerConstructorHook);
// Rock Band 3 Deluxe names them in its menus
inline constexpr const char* kRb3eModifiers[] = {
    "mod_black_background",  // no venue: the track over black
    "mod_force_hopos",       // every note a hammer-on
    "mod_mirror_mode",       // green and orange, red and blue swapped
    "mod_color_shuffle",     // gems drawn in random colours
    "mod_gem_shuffle",       // each gem's lanes shuffled
    "mod_double_bass",       // expert drums get the 2x bass pedal notes
};

// whether the modifier is on; false for one the game doesn't have
bool Active(PPCContext& ctx, uint8_t* base, const char* name);

// the Symbol for `name` (its interned string's guest address), made once
uint32_t Intern(PPCContext& ctx, uint8_t* base, const char* name);

}
