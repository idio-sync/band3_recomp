#pragma once
#include <cstdint>
#include <cstring>

struct PPCContext;

// The game's modifiers (ModifierMgr), and RB3Enhanced's six, which band3 adds
// to them as RB3E does. These call the game, so they run on a guest thread,
// from a hook.

namespace band3::modifiers {

struct Rb3eModifier {
    const char* name;
    const char* label;  // RB3E's English name, for a locale without one
};

// RB3E's modifiers (source/rb3enhanced.c's ModifierManagerConstructorHook),
// with the names its Locale::Localize hook gives them (source/LocaleHooks.c's
// newLocales). Rock Band 3 Deluxe names four in its locale, but not gem
// shuffle or double bass, leaving those to RB3E.
inline constexpr Rb3eModifier kRb3eModifiers[] = {
    {"mod_black_background", "Black Background"},  // no venue: the track over black
    {"mod_force_hopos", "Force HOPOs"},            // every note a hammer-on
    {"mod_mirror_mode", "Mirror Mode"},            // green and orange, red and blue swapped
    {"mod_color_shuffle", "Gem Color Shuffle"},    // gems drawn in random colours
    {"mod_gem_shuffle", "Note Shuffle"},           // each gem's lanes shuffled
    {"mod_double_bass", "Double Bass Pedal"},      // expert drums get the 2x bass pedal notes
};

// RB3E's name for one of its modifiers' locale tokens, or null
inline const char* FallbackName(const char* token) {
    for (const auto& modifier : kRb3eModifiers) {
        if (std::strcmp(modifier.name, token) == 0) return modifier.label;
    }
    return nullptr;
}

// whether the modifier is on; false for one the game doesn't have
bool Active(PPCContext& ctx, uint8_t* base, const char* name);

// the Symbol for `name` (its interned string's guest address), made once
uint32_t Intern(PPCContext& ctx, uint8_t* base, const char* name);

}
