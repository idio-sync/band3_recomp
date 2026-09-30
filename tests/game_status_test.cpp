// Checks what the Instrument Lab keeps of the game's own decisions: the mic
// each player sings through (src/Audio/mic_mapping_status.cpp), logged only
// when it changes, and the per-controller lag table
// (src/Input/joypad_lag_status.cpp), shown only once a row is complete.

#include <doctest/doctest.h>
#include <vector>
#include "src/Audio/mic_mapping_status.h"
#include "src/Input/joypad_lag_status.h"

using namespace band3;

TEST_CASE("the mic mapping reports only changes") {
    const std::vector<audio::MicMappingMic> one_mic{{0, true}};
    const std::vector<audio::MicMappingPlayer> first_has_it{{0, -1}, {-1, -1}};

    CHECK(!audio::GetMicMapping().seen);
    CHECK(audio::RecordMicMapping(first_has_it, one_mic));
    CHECK(!audio::RecordMicMapping(first_has_it, one_mic));

    const std::vector<audio::MicMappingPlayer> second_wants_it{{0, -1}, {-1, 0}};
    CHECK(audio::RecordMicMapping(second_wants_it, one_mic));
    CHECK(audio::RecordMicMapping(second_wants_it, {{0, false}}));

    const audio::MicMapping mapping = audio::GetMicMapping();
    CHECK(mapping.seen);
    CHECK(mapping.refreshes == 4);
    REQUIRE(mapping.players.size() == 2);
    CHECK(mapping.players[1].preferred == 0);
    REQUIRE(mapping.mics.size() == 1);
    CHECK(!mapping.mics[0].locked);
}

TEST_CASE("a lag row shows once every context is in") {
    constexpr uint32_t kXboxGuitar = 5;
    CHECK(!input::JoypadLag(kXboxGuitar));
    for (int c = 0; c < input::kLagContexts - 1; c++) {
        input::RecordJoypadLag(kXboxGuitar, c, 40.0f + c);
    }
    CHECK(!input::JoypadLag(kXboxGuitar));
    input::RecordJoypadLag(kXboxGuitar, input::kLagContexts - 1, 45.0f);

    const auto row = input::JoypadLag(kXboxGuitar);
    REQUIRE(row);
    CHECK((*row)[1] == 41.0f);
    CHECK((*row)[input::kLagContexts - 1] == 45.0f);

    // out of the table: ignored, not written past it
    input::RecordJoypadLag(input::kLagJoypadTypes, 0, 1.0f);
    input::RecordJoypadLag(kXboxGuitar, input::kLagContexts, 1.0f);
    CHECK(!input::JoypadLag(input::kLagJoypadTypes));
    CHECK((*input::JoypadLag(kXboxGuitar))[0] == 40.0f);
}
