#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/types.h>
#include <cstdint>
#include <cstring>
#include "generated/band3_init.h"
#include "src/Input/input_system.h"
#include "src/Input/instruments.h"
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
// kNumJoypads; pad n reads XInput player n
constexpr uint32_t kNumJoypads = 4;

// a stack below the hooked function's frame, with the caller's r13
PPCContext CallContext(const PPCContext& ctx) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - 0x100;
    call.r13 = ctx.r13;
    return call;
}

// Writes the pro data of every pad whose instrument reports `subtype`. Pads
// that are anything else keep what the game read.
void FillProData(PPCContext& ctx, uint8_t* base, uint8_t subtype) {
    rex::input::InputSystem* input = GameInputSystem();
    if (!input) return;

    for (uint32_t pad = 0; pad < kNumJoypads; pad++) {
        rex::input::X_INPUT_CAPABILITIES caps{};
        if (input->GetCapabilities(pad, 0, &caps) != X_ERROR_SUCCESS) continue;
        if (caps.sub_type != subtype) continue;
        rex::input::X_INPUT_STATE state{};
        if (input->GetState(pad, &state) != X_ERROR_SUCCESS) continue;
        const ProData data = EncodeProData(LoadGamepad(state.gamepad));

        PPCContext call = CallContext(ctx);
        call.r3.u64 = pad;
        JoypadGetPadData(call, base);
        const uint32_t pad_data = call.r3.u32;
        if (!pad_data) continue;
        std::memcpy(REX_RAW_ADDR(pad_data + kJoypadData_ProData), data.data(), data.size());
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
