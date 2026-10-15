// Checks the launcher's on-screen keyboard (src/Launcher/ui/onscreen_keyboard.h):
// moving over its keys with a controller, typing, deleting, the case key,
// and how it's finished or given up.

#include <doctest/doctest.h>
#include <string>
#include "src/Launcher/ui/onscreen_keyboard.h"

using namespace band3::launcher::ui;
using Result = OnScreenKeyboard::Result;

namespace {

std::string Selected(const OnScreenKeyboard& k) { return k.Key(k.Selected()).label; }

}

TEST_CASE("the keyboard opens on its first key with the text it's given") {
    OnScreenKeyboard k;
    k.Open("abc");
    CHECK(k.Text() == "abc");
    CHECK(Selected(k) == "a");
    CHECK(k.Handle(Action::kSelect) == Result::kNone);
    CHECK(k.Text() == "abca");
}

TEST_CASE("left and right move along a row, wrapping; up and down keep the column") {
    OnScreenKeyboard k;
    k.Open("");
    k.Handle(Action::kRight);
    CHECK(Selected(k) == "b");
    k.Handle(Action::kLeft);
    k.Handle(Action::kLeft);
    CHECK(Selected(k) == "g");
    k.Handle(Action::kDown);
    CHECK(Selected(k) == "n");
    // past the top: the bottom row, whose wide keys take the column they cover
    k.Open("");
    k.Handle(Action::kUp);
    CHECK(Selected(k) == "abc/ABC");
    k.Handle(Action::kRight);
    CHECK(Selected(k) == "Space");
    k.Handle(Action::kRight);
    CHECK(Selected(k) == "Done");
    k.Handle(Action::kRight);
    CHECK(Selected(k) == "abc/ABC");
    // down from a wide key goes back to the column it came from
    k.Open("");
    for (int i = 0; i < 3; i++) k.Handle(Action::kRight);
    CHECK(Selected(k) == "d");
    k.Handle(Action::kUp);
    CHECK(Selected(k) == "Space");
    k.Handle(Action::kDown);
    CHECK(Selected(k) == "d");
}

TEST_CASE("X deletes, Y types a space, the case key swaps the letters") {
    OnScreenKeyboard k;
    k.Open("r\xC3\xA9");  // "ré": a whole character goes
    k.Handle(Action::kSecondary);
    CHECK(k.Text() == "r");
    k.Handle(Action::kReset);
    CHECK(k.Text() == "r ");
    k.Handle(Action::kUp);  // the case key
    k.Handle(Action::kSelect);
    CHECK(k.Upper());
    k.Handle(Action::kDown);
    CHECK(Selected(k) == "A");
    k.Handle(Action::kSelect);
    CHECK(k.Text() == "r A");
}

TEST_CASE("Done or Start finishes, B gives up") {
    OnScreenKeyboard k;
    k.Open("x");
    CHECK(k.Handle(Action::kPlay) == Result::kDone);
    k.Open("x");
    CHECK(k.Handle(Action::kBack) == Result::kCancelled);
    k.Open("x");
    k.Handle(Action::kUp);
    k.Handle(Action::kRight);
    k.Handle(Action::kRight);
    CHECK(Selected(k) == "Done");
    CHECK(k.Handle(Action::kSelect) == Result::kDone);
    // the mouse presses a key by its place
    k.Open("");
    CHECK(k.Press(1) == Result::kNone);
    CHECK(k.Text() == "b");
}

TEST_CASE("a plugged-in keyboard types straight in, up to the limit") {
    OnScreenKeyboard k;
    k.Open("ab", 4);
    k.Type("cde");
    CHECK(k.Text() == "abcd");
    k.Backspace();
    CHECK(k.Text() == "abc");
    // control characters aren't typed
    k.Type("\t\n");
    CHECK(k.Text() == "abc");
}

TEST_CASE("the keys cover the characters paths, addresses and lists need") {
    OnScreenKeyboard k;
    k.Open("");
    std::string typed;
    for (int i = 0; i < k.KeyCount(); i++) {
        if (k.Key(i).label.size() == 1) typed += k.Key(i).label;
    }
    for (char c : std::string("abcdefghijklmnopqrstuvwxyz0123456789'-.:/_,=@?&+")) {
        INFO(c);
        CHECK(typed.find(c) != std::string::npos);
    }
}
