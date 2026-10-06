// Checks which player each device feeds (src/Input/player_slots.cpp).

#include <doctest/doctest.h>
#include <vector>
#include "src/Input/player_slots.h"

using namespace band3::input;

namespace {

// `seat`: the player (1-4) the pad had at the last assignment, 0 for a new pad
SlotDevice Pad(int seat = 0) { return {SlotDevice::Kind::kPad, 0, seat}; }
SlotDevice Synthetic() { return {SlotDevice::Kind::kSynthetic}; }
SlotDevice Virtual(int player) { return {SlotDevice::Kind::kVirtual, player}; }
SlotDevice SkippedCopy() { return {SlotDevice::Kind::kSkipped}; }

using Players = std::array<std::vector<size_t>, kPlayers>;
constexpr std::array<bool, kPlayers> kNoneReserved{};

}

TEST_CASE("pads feed players in the order they connected, synthetic devices player 1") {
    const Players players =
        AssignPlayers({Synthetic(), Pad(), Pad()}, kNoneReserved);
    CHECK(players[0] == std::vector<size_t>{0, 1});
    CHECK(players[1] == std::vector<size_t>{2});
    CHECK(players[2].empty());
}

TEST_CASE("a virtual instrument feeds its own player and pads go around it") {
    const Players players =
        AssignPlayers({Pad(), Virtual(2), Pad()}, {false, true, false, false});
    CHECK(players[0] == std::vector<size_t>{0});
    CHECK(players[1] == std::vector<size_t>{1});
    CHECK(players[2] == std::vector<size_t>{2});
}

TEST_CASE("several virtual instruments each keep their player") {
    const Players players = AssignPlayers(
        {Pad(), Virtual(1), Virtual(3), Pad()}, {true, false, true, false});
    CHECK(players[0] == std::vector<size_t>{1});
    CHECK(players[1] == std::vector<size_t>{0});
    CHECK(players[2] == std::vector<size_t>{2});
    CHECK(players[3] == std::vector<size_t>{3});
}

TEST_CASE("a reserved player stays free while its instrument replugs") {
    // player 2's instrument is between unplug and replug: no device, still reserved
    const Players players = AssignPlayers({Pad(), Pad()}, {false, true, false, false});
    CHECK(players[0] == std::vector<size_t>{0});
    CHECK(players[1].empty());
    CHECK(players[2] == std::vector<size_t>{1});
}

TEST_CASE("synthetic devices stay off player 1 while it has a virtual instrument") {
    const Players players =
        AssignPlayers({Synthetic(), Virtual(1)}, {true, false, false, false});
    CHECK(players[0] == std::vector<size_t>{1});
    for (int p = 1; p < kPlayers; p++) CHECK(players[p].empty());
}

TEST_CASE("pads past the last free player are left out") {
    const Players players = AssignPlayers(
        {Virtual(1), Virtual(2), Virtual(3), Pad(), Pad()}, {true, true, true, false});
    CHECK(players[3] == std::vector<size_t>{3});
    for (const auto& p : players) {
        for (size_t d : p) CHECK(d != 4);
    }
}

TEST_CASE("SDL's copy of a HID instrument is left out and later pads close up") {
    const Players players =
        AssignPlayers({Pad(), SkippedCopy(), Pad()}, kNoneReserved);
    CHECK(players[0] == std::vector<size_t>{0});
    CHECK(players[1] == std::vector<size_t>{2});
    CHECK(players[2].empty());
}

TEST_CASE("a pad keeps its player when a HID instrument before it unplugs") {
    // the guitar connected first: its SDL copy, then the guitar, then the pad
    Players players = AssignPlayers({SkippedCopy(), Pad(), Pad()}, kNoneReserved);
    CHECK(players[0] == std::vector<size_t>{1});
    CHECK(players[1] == std::vector<size_t>{2});
    // the guitar unplugs, and its copy with it
    players = AssignPlayers({Pad(2)}, kNoneReserved);
    CHECK(players[0].empty());
    CHECK(players[1] == std::vector<size_t>{0});
    // and plugs back in to the player it left
    players = AssignPlayers({SkippedCopy(), Pad(), Pad(2)}, kNoneReserved);
    CHECK(players[0] == std::vector<size_t>{1});
    CHECK(players[1] == std::vector<size_t>{2});
}

TEST_CASE("a new pad takes the lowest free player, around the pads that have one") {
    // listed first, as the SDK hands it the lowest free ordinal
    const Players players = AssignPlayers({Pad(), Pad(1), Pad(3)}, kNoneReserved);
    CHECK(players[0] == std::vector<size_t>{1});
    CHECK(players[1] == std::vector<size_t>{0});
    CHECK(players[2] == std::vector<size_t>{2});
}

TEST_CASE("a pad on a player a virtual instrument takes moves to a free one") {
    const Players players = AssignPlayers({Pad(1), Pad(2), Virtual(1)}, {true, false, false, false});
    CHECK(players[0] == std::vector<size_t>{2});
    CHECK(players[1] == std::vector<size_t>{1});
    CHECK(players[2] == std::vector<size_t>{0});
}

TEST_CASE("a pad left out takes a player that frees up") {
    Players players = AssignPlayers({Pad(), Pad(), Pad(), Pad(), Pad()}, kNoneReserved);
    for (const auto& p : players) {
        for (size_t d : p) CHECK(d != 4);
    }
    // player 2 unplugs
    players = AssignPlayers({Pad(1), Pad(3), Pad(4), Pad()}, kNoneReserved);
    CHECK(players[1] == std::vector<size_t>{3});
}

TEST_CASE("the launcher reads a device on the last player it doesn't feed") {
    CHECK(ProbePlayer({false, false, false, false}) == 3);
    // player 1's keyboard or second pad
    CHECK(ProbePlayer({true, false, false, false}) == 3);
    // player 4's pad
    CHECK(ProbePlayer({false, false, false, true}) == 2);
    CHECK(ProbePlayer({false, true, true, true}) == 0);
    // can't happen (a device feeds one player at most), but stays in range
    CHECK(ProbePlayer({true, true, true, true}) == 3);
}
