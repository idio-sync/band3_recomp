#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include "navigation.h"

// The on-screen keyboard, after Rock Band 3's name entry: a grid of keys a
// controller moves over (letters, digits and the marks paths, addresses and
// lists need), then the case key, Space and Done. A types the key, X deletes,
// Y types a space, B gives up, Done or Start finishes; a plugged-in keyboard
// types straight in. Only the keys and the text: the Settings screen draws
// it in its detail panel.

namespace band3::launcher::ui {

class OnScreenKeyboard {
public:
    enum class Result { kNone, kDone, kCancelled };

    struct KeyInfo {
        // as drawn: the letter in the current case, or the key's name
        std::string label;
        int row = 0;
        int column = 0;
        // columns it covers
        int span = 1;
    };

    static constexpr int kColumns = 7;

    // starts over with `text`, on the first key; `max_length` characters at
    // most (0: no limit)
    void Open(std::string text, size_t max_length = 0);

    // a controller's (or the keyboard's) action
    Result Handle(Action action);
    // the mouse's click on key `index`
    Result Press(int index);
    // text from a plugged-in keyboard (UTF-8); control characters are dropped
    void Type(std::string_view text);
    // deletes the last character
    void Backspace();

    const std::string& Text() const { return text_; }
    bool Upper() const { return upper_; }
    int Selected() const { return selected_; }
    void Select(int index);
    int KeyCount() const;
    KeyInfo Key(int index) const;

private:
    // the key in `row` that covers `column`
    int KeyAt(int row, int column) const;
    int RowCount() const;

    std::string text_;
    size_t max_length_ = 0;
    bool upper_ = false;
    int selected_ = 0;
    // the column up and down keep, as a wide key covers several
    int column_ = 0;
};

}
