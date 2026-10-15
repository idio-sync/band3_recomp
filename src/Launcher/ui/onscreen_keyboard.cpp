#include "onscreen_keyboard.h"
#include <cctype>

namespace band3::launcher::ui {

namespace {

// the character keys, a row of kColumns each
constexpr std::string_view kRows[] = {
    "abcdefg", "hijklmn", "opqrstu", "vwxyz'-", "1234567", "890.:/_", ",=@?&+#",
};
constexpr int kCharRows = static_cast<int>(std::size(kRows));
constexpr int kCharKeys = kCharRows * OnScreenKeyboard::kColumns;

// the bottom row's keys, after the characters
constexpr int kCaseKey = kCharKeys;
constexpr int kSpaceKey = kCharKeys + 1;
constexpr int kDoneKey = kCharKeys + 2;

struct WideKey {
    const char* label;
    int column;
    int span;
};
constexpr WideKey kBottom[] = {{"abc/ABC", 0, 2}, {"Space", 2, 3}, {"Done", 5, 2}};

bool Continuation(unsigned char c) { return (c & 0xC0) == 0x80; }

size_t Characters(std::string_view text) {
    size_t count = 0;
    for (const char c : text) count += !Continuation(static_cast<unsigned char>(c));
    return count;
}

}

void OnScreenKeyboard::Open(std::string text, size_t max_length) {
    text_ = std::move(text);
    max_length_ = max_length;
    upper_ = false;
    selected_ = 0;
    column_ = 0;
}

int OnScreenKeyboard::KeyCount() const { return kCharKeys + static_cast<int>(std::size(kBottom)); }

int OnScreenKeyboard::RowCount() const { return kCharRows + 1; }

OnScreenKeyboard::KeyInfo OnScreenKeyboard::Key(int index) const {
    if (index < kCharKeys) {
        char c = kRows[index / kColumns][static_cast<size_t>(index % kColumns)];
        if (upper_) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return {.label = std::string(1, c), .row = index / kColumns, .column = index % kColumns};
    }
    const WideKey& key = kBottom[index - kCharKeys];
    return {.label = key.label, .row = kCharRows, .column = key.column, .span = key.span};
}

int OnScreenKeyboard::KeyAt(int row, int column) const {
    if (row < kCharRows) return row * kColumns + column;
    for (int i = 0; i < static_cast<int>(std::size(kBottom)); i++) {
        if (column < kBottom[i].column + kBottom[i].span) return kCharKeys + i;
    }
    return kDoneKey;
}

void OnScreenKeyboard::Select(int index) {
    if (index < 0 || index >= KeyCount()) return;
    selected_ = index;
    column_ = Key(index).column;
}

OnScreenKeyboard::Result OnScreenKeyboard::Handle(Action action) {
    const KeyInfo key = Key(selected_);
    switch (action) {
    case Action::kUp:
    case Action::kDown: {
        const int rows = RowCount();
        const int row = (key.row + (action == Action::kUp ? rows - 1 : 1)) % rows;
        selected_ = KeyAt(row, column_);
        break;
    }
    case Action::kLeft:
    case Action::kRight: {
        const int step = action == Action::kLeft ? -1 : 1;
        if (key.row < kCharRows) {
            Select(key.row * kColumns + (key.column + step + kColumns) % kColumns);
        } else {
            const int count = static_cast<int>(std::size(kBottom));
            Select(kCharKeys + (selected_ - kCharKeys + step + count) % count);
        }
        break;
    }
    case Action::kSelect: return Press(selected_);
    case Action::kSecondary: Backspace(); break;
    case Action::kReset: Type(" "); break;
    case Action::kBack: return Result::kCancelled;
    case Action::kPlay: return Result::kDone;
    default: break;
    }
    return Result::kNone;
}

OnScreenKeyboard::Result OnScreenKeyboard::Press(int index) {
    if (index < 0 || index >= KeyCount()) return Result::kNone;
    selected_ = index;
    if (index < kCharKeys) {
        Type(Key(index).label);
    } else if (index == kCaseKey) {
        upper_ = !upper_;
    } else if (index == kSpaceKey) {
        Type(" ");
    } else if (index == kDoneKey) {
        return Result::kDone;
    }
    return Result::kNone;
}

void OnScreenKeyboard::Type(std::string_view text) {
    size_t count = Characters(text_);
    for (size_t i = 0; i < text.size();) {
        // one character: its lead byte and what continues it
        size_t end = i + 1;
        while (end < text.size() && Continuation(static_cast<unsigned char>(text[end]))) end++;
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const bool control = lead < 0x20 || lead == 0x7F;
        if (!control && (max_length_ == 0 || count < max_length_)) {
            text_.append(text.substr(i, end - i));
            count++;
        }
        i = end;
    }
}

void OnScreenKeyboard::Backspace() {
    while (!text_.empty() && Continuation(static_cast<unsigned char>(text_.back()))) {
        text_.pop_back();
    }
    if (!text_.empty()) text_.pop_back();
}

}
