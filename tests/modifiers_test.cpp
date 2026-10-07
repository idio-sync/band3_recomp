// Checks the names RB3Enhanced's modifiers fall back to (src/Game/Modifiers.h)
// when the game's locale has none: Rock Band 3 Deluxe names four of them but
// leaves Note Shuffle and Double Bass Pedal to RB3E's Locale::Localize hook.

#include <doctest/doctest.h>
#include <cstring>
#include "src/Game/Modifiers.h"

TEST_CASE("RB3E's modifiers fall back to RB3E's names") {
    CHECK(std::strcmp(band3::modifiers::FallbackName("mod_gem_shuffle"), "Note Shuffle") == 0);
    CHECK(std::strcmp(band3::modifiers::FallbackName("mod_double_bass"), "Double Bass Pedal") ==
          0);
    CHECK(std::strcmp(band3::modifiers::FallbackName("mod_black_background"),
                      "Black Background") == 0);
}

TEST_CASE("other tokens have no fallback") {
    CHECK(band3::modifiers::FallbackName("mod_gem_shuffle_desc") == nullptr);
    CHECK(band3::modifiers::FallbackName("mod_nofail") == nullptr);
    CHECK(band3::modifiers::FallbackName("") == nullptr);
}

TEST_CASE("every RB3E modifier has a name") {
    for (const auto& modifier : band3::modifiers::kRb3eModifiers) {
        CHECK(band3::modifiers::FallbackName(modifier.name) == modifier.label);
    }
}
