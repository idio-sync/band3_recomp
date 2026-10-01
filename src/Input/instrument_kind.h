#pragma once
#include <optional>
#include <string_view>
#include "instruments.h"

// The instruments the virtual instrument can be, and what one is pressing.
// Kept apart from virtual_instrument.h, which needs the SDK, so the unit tests
// and the test harness's command parser can use them.

namespace band3::input {

enum class InstrumentKind { kGuitar, kDrums, kKeys, kProGuitarMustang, kProGuitarSquier };

inline constexpr InstrumentKind kInstrumentKinds[] = {
    InstrumentKind::kGuitar, InstrumentKind::kDrums, InstrumentKind::kKeys,
    InstrumentKind::kProGuitarMustang, InstrumentKind::kProGuitarSquier,
};

// the virtual_instrument_type value for a kind, and back
const char* InstrumentKindId(InstrumentKind kind);
std::optional<InstrumentKind> ParseInstrumentKind(std::string_view id);
const char* InstrumentKindLabel(InstrumentKind kind);

Caps360 CapsFor(InstrumentKind kind);

struct InstrumentInputs {
    GuitarInputs guitar;
    DrumInputs drums;
    KeysInputs keys;
    ProGuitarInputs pro_guitar;
};

Gamepad360 Encode(InstrumentKind kind, const InstrumentInputs& in);

}
