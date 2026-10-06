#include "keyboard_search.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <cstring>
#include "src/Game/DataNode.h"
#include "src/Game/Script.h"
#include "src/Game/Symbol.h"
#include "src/Input/input_system.h"
#include "src/Input/keyboard_search_driver.h"
#include "src/Render/native_view.h"
#include "src/settings.h"

// Typing to search the song list (Input/keyboard_search.h), the game's half:
// where the keyboard types, read from Rock Band 3 Deluxe's variables and the
// screen up, and the keys sent as RB3Enhanced sends them, one a frame through
// {ui key $key $shift $ctrl $alt}.

REX_EXTERN(DataVariable);

namespace band3::keyboard_search {

namespace {

using input::keyboard_search::Mode;

// RB3Enhanced's Xbox 360 TU5 addresses and offsets, as rb3e_events.cpp's
constexpr uint32_t kTheBandUI = 0x82DFD2B0;       // BandUI object
constexpr uint32_t kBandUI_CurrentScreen = 0x2C;  // UIScreen*
constexpr uint32_t kUIScreen_Name = 0x18;         // Symbol (char*)
constexpr uint32_t kDataNode_Type = 4;

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

// DataVariable(Symbol) -> DataNode*: the variable's node, made if it's new.
// The game keeps these pointers (they don't move), so each is looked up once.
uint32_t Variable(PPCContext& ctx, uint8_t* base, const char* name) {
    const Symbol symbol(ctx, base, name);
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - 0x100;
    call.r13 = ctx.r13;
    call.r3.u64 = symbol.value(base);
    DataVariable(call, base);
    return call.r3.u32;
}

struct Variables {
    // Deluxe's dx_keyboard.dta, with its keyboard handler, sets $clipboard to
    // "" as it loads: a string only with a Deluxe that has keyboard search
    uint32_t clipboard = 0;
    // the input field focused, an object while there's one
    uint32_t focused_input_field = 0;
};

const Variables& Deluxe(PPCContext& ctx, uint8_t* base) {
    static Variables variables;
    if (!variables.clipboard) {
        variables.clipboard = Variable(ctx, base, "clipboard");
        variables.focused_input_field = Variable(ctx, base, "focused_input_field");
    }
    return variables;
}

bool HasType(uint8_t* base, uint32_t node, DataType type) {
    return node && Load32(base, node + kDataNode_Type) == static_cast<uint32_t>(type);
}

// the song list, or Deluxe's search screen over it
bool OnSongList(uint8_t* base) {
    const uint32_t screen = Load32(base, kTheBandUI + kBandUI_CurrentScreen);
    const uint32_t name = screen ? Load32(base, screen + kUIScreen_Name) : 0;
    if (!name) return false;
    const char* text = rex::memory::GuestPtr<const char*>(base, name);
    return std::strcmp(text, "song_select_screen") == 0 ||
           std::strcmp(text, "dx_search_screen") == 0;
}

Mode CurrentMode(PPCContext& ctx, uint8_t* base) {
    if (!REXCVAR_GET(keyboard_search) || render::InSong() || input::GameInputBlocked()) {
        return Mode::kOff;
    }
    const bool song_list = OnSongList(base);
    const Variables& deluxe = Deluxe(ctx, base);
    if (!HasType(base, deluxe.clipboard, kDataString)) {
        static bool logged = false;
        if (song_list && !logged) {
            logged = true;
            REXLOG_INFO("keyboard_search: no Rock Band 3 Deluxe with keyboard search "
                        "(dx_keyboard.dta) here; the keyboard stays the controller");
        }
        return Mode::kOff;
    }
    if (HasType(base, deluxe.focused_input_field, kDataObject)) return Mode::kField;
    return song_list ? Mode::kSongList : Mode::kOff;
}

}

void RunFrame(PPCContext& ctx, uint8_t* base) {
    // The first key on the song list opens Deluxe's search screen, whose field
    // takes it once the screen is up; until then Deluxe keeps only one key, so
    // the keys after it wait for the field (for at most this many frames).
    constexpr int kWaitForField = 120;
    static int waiting = 0;

    const Mode mode = CurrentMode(ctx, base);
    input::SetKeyboardSearchMode(mode);
    if (mode != Mode::kSongList) waiting = 0;
    if (mode == Mode::kOff) return;
    if (waiting > 0) {
        waiting--;
        return;
    }
    if (const auto key = input::TakeKeyboardSearchKey()) {
        RunScript(ctx, base, input::keyboard_search::Script(*key));
        if (mode == Mode::kSongList) waiting = kWaitForField;
    }
}

}
