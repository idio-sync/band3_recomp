// Checks typing to search the song list (src/Input/keyboard_search.cpp): which
// keys type rather than press their binds, and what each sends Rock Band 3
// Deluxe.

#include <doctest/doctest.h>
#include <cstdint>
#include "src/Input/keyboard_search.h"

using namespace band3::input::keyboard_search;

namespace {

// Windows virtual key codes
constexpr uint16_t kBack = 0x08;
constexpr uint16_t kTab = 0x09;
constexpr uint16_t kReturn = 0x0D;
constexpr uint16_t kShift = 0x10;
constexpr uint16_t kControl = 0x11;
constexpr uint16_t kEscape = 0x1B;
constexpr uint16_t kSpace = 0x20;
constexpr uint16_t kLeft = 0x25;
constexpr uint16_t kUp = 0x26;
constexpr uint16_t kRight = 0x27;
constexpr uint16_t kDown = 0x28;
constexpr uint16_t kDelete = 0x2E;
constexpr uint16_t k1 = 0x31;
constexpr uint16_t kA = 0x41;
constexpr uint16_t kG = 0x47;
constexpr uint16_t kV = 0x56;
constexpr uint16_t kNumpad5 = 0x65;
constexpr uint16_t kF4 = 0x73;
constexpr uint16_t kOem3 = 0xC0;  // ` on a US keyboard

DownAction Down(Mode mode, uint16_t vk) { return OnKeyDown(mode, vk, false, false, false); }

}

TEST_CASE("off, the keyboard is only the controller") {
    for (uint16_t vk : {kA, kSpace, kReturn, kBack, kLeft, kEscape, kF4}) {
        const DownAction action = Down(Mode::kOff, vk);
        CHECK_FALSE(action.consume);
        CHECK_FALSE(action.send);
    }
    CHECK_FALSE(OnChar(Mode::kOff, 'a', false, false, false));
}

TEST_CASE("on the song list the keys that type are kept from their binds and send their character") {
    for (uint16_t vk : {kA, kG, k1, kSpace, kNumpad5, kOem3}) {
        const DownAction action = Down(Mode::kSongList, vk);
        CHECK(action.consume);
        CHECK(action.types);
        CHECK_FALSE(action.send);  // the character comes after
    }
    CHECK(OnChar(Mode::kSongList, 'g', false, false, false) == Key{'g'});
    CHECK(OnChar(Mode::kSongList, 'G', true, false, false) == Key{'G', true});
    CHECK(OnChar(Mode::kSongList, '`', false, false, false) == Key{'`'});
}

TEST_CASE("on the song list the other keys stay the controller") {
    // Enter with nothing focused opens Deluxe's console, and any key the list
    // gets opens the search, so neither the arrows nor Enter go to the game
    for (uint16_t vk : {kReturn, kBack, kTab, kLeft, kUp, kRight, kDown, kDelete, kEscape, kF4,
                        kShift, kControl}) {
        const DownAction action = Down(Mode::kSongList, vk);
        CHECK_FALSE(action.consume);
        CHECK_FALSE(action.send);
    }
}

TEST_CASE("a focused field takes the keys it edits with") {
    CHECK(Down(Mode::kField, kA).consume);
    CHECK_FALSE(Down(Mode::kField, kA).send);
    CHECK(OnChar(Mode::kField, 'a', false, false, false) == Key{'a'});

    const auto sends = [](uint16_t vk, int code) {
        const DownAction action = Down(Mode::kField, vk);
        CHECK(action.consume);
        CHECK(action.send == Key{code});
    };
    sends(kBack, 0x08);
    sends(kTab, 0x09);
    sends(kReturn, 0x0A);
    sends(kLeft, 0x140);
    sends(kRight, 0x141);
    sends(kUp, 0x142);
    sends(kDown, 0x143);
    sends(kDelete, 0x137);

    // shift with an arrow selects
    CHECK(OnKeyDown(Mode::kField, kLeft, true, false, false).send == Key{0x140, true});
}

TEST_CASE("a focused field leaves Escape and the function keys to the controller and band3") {
    for (uint16_t vk : {kEscape, kF4, kShift, kControl}) {
        const DownAction action = Down(Mode::kField, vk);
        CHECK_FALSE(action.consume);
        CHECK_FALSE(action.send);
    }
}

TEST_CASE("ctrl with a letter sends the letter with ctrl, and its control character nothing") {
    // Deluxe's field: ctrl+a selects all, ctrl+c/x/v copy, cut and paste
    const DownAction action = OnKeyDown(Mode::kField, kV, false, true, false);
    CHECK(action.consume);
    CHECK(action.send == Key{'v', false, true});
    CHECK_FALSE(OnChar(Mode::kField, 0x16, false, true, false));
    CHECK(OnKeyDown(Mode::kField, kBack, false, true, false).send == Key{0x08, false, true});
    CHECK(OnKeyDown(Mode::kField, kSpace, false, true, false).send == Key{' ', false, true});

    // on the song list ctrl combinations stay the controller's
    CHECK_FALSE(OnKeyDown(Mode::kSongList, kV, false, true, false).consume);
    CHECK_FALSE(OnChar(Mode::kSongList, 0x16, false, true, false));
}

TEST_CASE("AltGr (ctrl with alt) types its character, without the modifiers") {
    // '@' on a German keyboard is AltGr+Q
    const DownAction action = OnKeyDown(Mode::kField, 0x51, false, true, true);
    CHECK(action.consume);
    CHECK_FALSE(action.send);
    CHECK(OnChar(Mode::kField, '@', false, true, true) == Key{'@'});
    CHECK(OnKeyDown(Mode::kSongList, 0x51, false, true, true).consume);
    CHECK(OnChar(Mode::kSongList, '@', false, true, true) == Key{'@'});
}

TEST_CASE("alt is the window's but for a key that types, which may be AltGr") {
    for (Mode mode : {Mode::kSongList, Mode::kField}) {
        // Alt+Enter and the window's own shortcuts
        for (uint16_t vk : {kReturn, kLeft, kBack, kF4}) {
            const DownAction action = OnKeyDown(mode, vk, false, false, true);
            CHECK_FALSE(action.consume);
            CHECK_FALSE(action.send);
        }
        // the window may give AltGr as alt alone
        CHECK(OnKeyDown(mode, 0x51, false, false, true).consume);
        CHECK(OnChar(mode, '@', false, false, true) == Key{'@'});
    }
}

TEST_CASE("only printable ASCII is typed") {
    // Deluxe's field inserts 32 to 127 only
    CHECK_FALSE(OnChar(Mode::kField, 0x0D, false, false, false));
    CHECK_FALSE(OnChar(Mode::kField, 0x7F, false, false, false));
    CHECK_FALSE(OnChar(Mode::kField, 0xE9, false, false, false));  // é
    CHECK(OnChar(Mode::kField, '~', false, false, false) == Key{'~'});
    CHECK(OnChar(Mode::kField, ' ', false, false, false) == Key{' '});
}

TEST_CASE("a key's character on a US keyboard, for the first key typed") {
    CHECK(UsCharacter(kA, false) == 'a');
    CHECK(UsCharacter(kG, true) == 'G');
    CHECK(UsCharacter(k1, false) == '1');
    CHECK(UsCharacter(k1, true) == '!');
    CHECK(UsCharacter(0x30, true) == ')');
    CHECK(UsCharacter(kSpace, false) == ' ');
    CHECK(UsCharacter(kNumpad5, false) == '5');
    CHECK(UsCharacter(kOem3, false) == '`');
    CHECK(UsCharacter(0xDE, false) == '\'');
    CHECK(UsCharacter(0xDC, false) == '\\');
    CHECK_FALSE(UsCharacter(kReturn, false));
    CHECK_FALSE(UsCharacter(kLeft, false));
}

TEST_CASE("the script is RB3Enhanced's command") {
    CHECK(Script(Key{'a'}) == "{ui key 97 0 0 0}");
    CHECK(Script(Key{0x140, true, false, false}) == "{ui key 320 1 0 0}");
    CHECK(Script(Key{'v', false, true, false}) == "{ui key 118 0 1 0}");
}
