#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/types.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include "generated/band3_init.h"
#include "src/Input/input_lock.h"
#include "src/Input/input_system.h"
#include "src/Input/instruments.h"
#include "src/Input/pro_instrument_status.h"
#include "src/Input/xinput_state.h"

// Pro Keys and Pro Guitar. RB3 reads a keytar's keys and a pro guitar's frets
// and strings from 16 bytes of each pad's JoypadData, which it fills through
// XamInputRawState; ReXGlue only stubs that, so the bytes never arrive. The two
// functions that read them, UsbMidiGuitar::Poll and UsbMidiKeyboard::Poll (run
// at the end of JoypadPollCommon), get the bytes written first, from the same
// XInput state the game already reads for the pad (see EncodeProData).
// Offsets are rb3-xenon's (src/system/os/Joypad.h).

extern "C" void __imp__UsbMidiGuitar__Poll(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__UsbMidiKeyboard__Poll(PPCContext& ctx, uint8_t* base);
REX_EXTERN(JoypadGetPadData);

namespace {

using namespace band3::input;
using rex::X_RESULT;

// JoypadData::mProGuitarData, read as ProKeysData by the keyboard
constexpr uint32_t kJoypadData_ProData = 0x34;
// JoypadData::mType, the JoypadType the Poll functions pick pads by
constexpr uint32_t kJoypadData_Type = 0x6C;
// kNumJoypads; pad n reads XInput player n
constexpr uint32_t kNumJoypads = 4;

// a stack below the hooked function's frame, with the caller's r13
PPCContext CallContext(const PPCContext& ctx) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - 0x100;
    call.r13 = ctx.r13;
    return call;
}

bool IsProSubtype(uint8_t subtype) {
    return subtype == kSubtypeProGuitar || subtype == kSubtypeKeytar;
}

// Writes the pro data of every pad whose instrument reports `subtype`. Pads
// that are anything else keep what the game read. Records each pad for the
// Instrument Lab, leaving the other hook's pads to it.
void FillProData(PPCContext& ctx, uint8_t* base, uint8_t subtype) {
    rex::input::InputSystem* input = GameInputSystem();
    if (!input) return;

    for (uint32_t pad = 0; pad < kNumJoypads; pad++) {
        rex::input::X_INPUT_CAPABILITIES caps{};
        bool connected;
        {
            std::lock_guard<std::recursive_mutex> lock(InputLock());
            connected = input->GetCapabilities(pad, 0, &caps) == X_ERROR_SUCCESS;
        }
        const bool mine = connected && caps.sub_type == subtype;
        if (connected && !mine && IsProSubtype(caps.sub_type)) continue;

        PPCContext call = CallContext(ctx);
        call.r3.u64 = pad;
        JoypadGetPadData(call, base);
        const uint32_t pad_data = call.r3.u32;
        if (!pad_data) continue;
        const uint32_t game_type = REX_LOAD_U32(pad_data + kJoypadData_Type);

        rex::input::X_INPUT_STATE state{};
        bool have_state = false;
        if (mine) {
            std::lock_guard<std::recursive_mutex> lock(InputLock());
            have_state = input->GetState(pad, &state) == X_ERROR_SUCCESS;
        }
        if (!have_state) {
            RecordProPad(static_cast<int>(pad), connected, connected ? caps.sub_type : 0,
                         game_type, nullptr);
            continue;
        }
        const ProData data = EncodeProData(LoadGamepad(state.gamepad));
        std::memcpy(REX_RAW_ADDR(pad_data + kJoypadData_ProData), data.data(), data.size());
        RecordProPad(static_cast<int>(pad), true, caps.sub_type, game_type, &data);
    }
}

}

// Mustang and Squier pro guitars
extern "C" REX_FUNC(UsbMidiGuitar__Poll)
{
    FillProData(ctx, base, kSubtypeProGuitar);
    __imp__UsbMidiGuitar__Poll(ctx, base);
}

// keytars, and MIDI Pro Adapters in keys mode
extern "C" REX_FUNC(UsbMidiKeyboard__Poll)
{
    FillProData(ctx, base, kSubtypeKeytar);
    __imp__UsbMidiKeyboard__Poll(ctx, base);
}
