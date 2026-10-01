#include "instrument_kind.h"

namespace band3::input {

const char* InstrumentKindId(InstrumentKind kind) {
    switch (kind) {
    case InstrumentKind::kGuitar: return "guitar";
    case InstrumentKind::kDrums: return "drums";
    case InstrumentKind::kKeys: return "keys";
    case InstrumentKind::kProGuitarMustang: return "pro_guitar_mustang";
    case InstrumentKind::kProGuitarSquier: return "pro_guitar_squier";
    }
    return "guitar";
}

std::optional<InstrumentKind> ParseInstrumentKind(std::string_view id) {
    for (InstrumentKind kind : kInstrumentKinds) {
        if (id == InstrumentKindId(kind)) return kind;
    }
    return std::nullopt;
}

const char* InstrumentKindLabel(InstrumentKind kind) {
    switch (kind) {
    case InstrumentKind::kGuitar: return "Guitar";
    case InstrumentKind::kDrums: return "Drums";
    case InstrumentKind::kKeys: return "Keys";
    case InstrumentKind::kProGuitarMustang: return "Pro Guitar (Mustang)";
    case InstrumentKind::kProGuitarSquier: return "Pro Guitar (Squier)";
    }
    return "Guitar";
}

Caps360 CapsFor(InstrumentKind kind) {
    switch (kind) {
    case InstrumentKind::kGuitar: return GuitarCaps();
    case InstrumentKind::kDrums: return DrumCaps();
    case InstrumentKind::kKeys: return KeysCaps();
    case InstrumentKind::kProGuitarMustang: return ProGuitarCaps(ProGuitarModel::kMustang);
    case InstrumentKind::kProGuitarSquier: return ProGuitarCaps(ProGuitarModel::kSquier);
    }
    return GuitarCaps();
}

Gamepad360 Encode(InstrumentKind kind, const InstrumentInputs& in) {
    switch (kind) {
    case InstrumentKind::kGuitar: return EncodeGuitar(in.guitar);
    case InstrumentKind::kDrums: return EncodeDrums(in.drums);
    case InstrumentKind::kKeys: return EncodeKeys(in.keys);
    case InstrumentKind::kProGuitarMustang:
    case InstrumentKind::kProGuitarSquier: return EncodeProGuitar(in.pro_guitar);
    }
    return {};
}

}
