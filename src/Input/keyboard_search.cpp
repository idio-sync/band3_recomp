#include "keyboard_search.h"
#include <format>

namespace band3::input::keyboard_search {

namespace {

// Windows virtual key codes (rex::ui::VirtualKey's values)
constexpr uint16_t kVkBack = 0x08;
constexpr uint16_t kVkTab = 0x09;
constexpr uint16_t kVkReturn = 0x0D;
constexpr uint16_t kVkSpace = 0x20;
constexpr uint16_t kVkPrior = 0x21;
constexpr uint16_t kVkNext = 0x22;
constexpr uint16_t kVkEnd = 0x23;
constexpr uint16_t kVkHome = 0x24;
constexpr uint16_t kVkLeft = 0x25;
constexpr uint16_t kVkUp = 0x26;
constexpr uint16_t kVkRight = 0x27;
constexpr uint16_t kVkDown = 0x28;
constexpr uint16_t kVkInsert = 0x2D;
constexpr uint16_t kVkDelete = 0x2E;

// The keys that type a character: space, the digits and letters, the numpad's
// digits and operators, and the punctuation (OEM) keys. What each types comes
// from the window's character, which follows with the layout and shift.
bool TypesCharacter(uint16_t vk) {
    return vk == kVkSpace || (vk >= 0x30 && vk <= 0x39) || (vk >= 0x41 && vk <= 0x5A) ||
           (vk >= 0x60 && vk <= 0x6F) || (vk >= 0xBA && vk <= 0xC0) ||
           (vk >= 0xDB && vk <= 0xDF) || vk == 0xE2;
}

// Dance Central 3's TranslateVK (RB3Enhanced's xbox_keyboard.c), for the keys
// Deluxe's input field edits with. Escape (0x12E) and the function keys aren't
// here: Escape opens band3's pause menu, and band3 and the SDK have the
// function keys.
std::optional<int> EditCode(uint16_t vk) {
    switch (vk) {
    case kVkBack: return 0x08;
    case kVkTab: return 0x09;
    case kVkReturn: return 0x0A;
    case kVkPrior: return 0x13A;
    case kVkNext: return 0x13B;
    case kVkEnd: return 0x139;
    case kVkHome: return 0x138;
    case kVkLeft: return 0x140;
    case kVkUp: return 0x142;
    case kVkRight: return 0x141;
    case kVkDown: return 0x143;
    case kVkInsert: return 0x136;
    case kVkDelete: return 0x137;
    default: return std::nullopt;
    }
}

// ctrl with a key: Deluxe's field takes ctrl+a, c, x, v, space and backspace,
// as the letter's lowercase character (KB_a is 'a')
std::optional<int> CtrlCode(uint16_t vk) {
    if (vk >= 0x41 && vk <= 0x5A) return vk - 0x41 + 'a';
    if (vk >= 0x30 && vk <= 0x39) return vk;
    if (vk == kVkSpace) return ' ';
    return EditCode(vk);
}

}

DownAction OnKeyDown(Mode mode, uint16_t vk, bool shift, bool ctrl, bool alt) {
    if (mode == Mode::kOff) return {};
    // a key that types: alone, with shift, or with AltGr (which the window
    // may give as alt alone, or as ctrl and alt); with ctrl alone it doesn't
    if (TypesCharacter(vk) && (alt || !ctrl)) return {true, std::nullopt, true};
    // the rest with alt are the window's (Alt+Enter)
    if (alt) return {};

    // the list opens the search with any key it gets, and Enter with nothing
    // focused opens Deluxe's console: only the keys that type
    if (mode == Mode::kSongList) return {};

    if (ctrl) {
        if (!TypesCharacter(vk) && !EditCode(vk)) return {};
        const std::optional<int> code = CtrlCode(vk);
        return {true, code ? std::optional<Key>(Key{*code, shift, true, false}) : std::nullopt};
    }
    if (const std::optional<int> code = EditCode(vk)) return {true, Key{*code, shift, false, false}};
    return {};
}

std::optional<Key> OnChar(Mode mode, uint32_t ch, bool shift, bool ctrl, bool alt) {
    if (mode == Mode::kOff) return std::nullopt;
    // ctrl's keys were sent going down
    if (ctrl && !alt) return std::nullopt;
    // Deluxe's field inserts printable ASCII only
    if (ch < 0x20 || ch > 0x7E) return std::nullopt;
    return Key{static_cast<int>(ch), shift, false, false};
}

std::optional<uint32_t> UsCharacter(uint16_t vk, bool shift) {
    if (vk >= 0x41 && vk <= 0x5A) return shift ? vk : vk - 0x41 + 'a';
    if (vk >= 0x30 && vk <= 0x39) return shift ? static_cast<uint32_t>(")!@#$%^&*("[vk - 0x30]) : vk;
    if (vk >= 0x60 && vk <= 0x69) return vk - 0x60 + '0';
    switch (vk) {
    case kVkSpace: return ' ';
    case 0x6A: return '*';
    case 0x6B: return '+';
    case 0x6D: return '-';
    case 0x6E: return '.';
    case 0x6F: return '/';
    case 0xBA: return shift ? ':' : ';';
    case 0xBB: return shift ? '+' : '=';
    case 0xBC: return shift ? '<' : ',';
    case 0xBD: return shift ? '_' : '-';
    case 0xBE: return shift ? '>' : '.';
    case 0xBF: return shift ? '?' : '/';
    case 0xC0: return shift ? '~' : '`';
    case 0xDB: return shift ? '{' : '[';
    case 0xDC: return shift ? '|' : '\\';
    case 0xDD: return shift ? '}' : ']';
    case 0xDE: return shift ? '"' : '\'';
    default: return std::nullopt;
    }
}

std::string Script(const Key& key) {
    return std::format("{{ui key {} {} {} {}}}", key.code, key.shift ? 1 : 0, key.ctrl ? 1 : 0,
                       key.alt ? 1 : 0);
}

}
