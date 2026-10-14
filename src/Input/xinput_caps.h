#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include "instruments.h"

// An Xbox 360 drum kit's own capabilities, for one SDL reads. The SDK's SDL
// driver makes up every controller's capabilities (no force feedback flag, -1
// in each stick), and SDL can't read the real ones, so RB3 took every 360 kit
// for an RB1 kit (IsRb2Drums): no cymbals or velocity, each face button its
// pad, so cymbals played as pads. Windows' XInput can read them, so band3 asks
// it for the kit's flags and stick capabilities (an RB2-or-later kit's hardware
// ids, the ION Drum Rocker's 0x15E4/0x0130/0x0100 among them) and gives the
// game those. The XInput backend passes them through already.

namespace band3::input {

// what SDL's XInput backend keeps in a device's GUID: the subtype, and the USB
// vendor and product when SDL had them (0 when it didn't)
struct XInputGuidIds {
    uint16_t vendor = 0;
    uint16_t product = 0;
    uint8_t subtype = 0;
};
std::optional<XInputGuidIds> XInputIdsFromGuid(std::string_view guid);

// a connected Windows XInput pad, as XInputGetCapabilitiesEx reads it (vendor
// and product 0 where only XInputGetCapabilities was there)
struct WindowsPad {
    uint16_t vendor = 0;
    uint16_t product = 0;
    Caps360 caps;
};

// The capabilities of the Windows pads that are the device SDL reads as `ids`:
// those of its subtype, and its vendor and product where both sides know them.
// Two kits of one model report the same, so which of them it is doesn't
// matter; when the pads that could be it disagree, nothing.
std::optional<Caps360> MatchWindowsCaps(const XInputGuidIds& ids,
                                        std::span<const WindowsPad> pads);

// `reported` (what the SDK read for the device with `guid`) with the flags and
// stick capabilities Windows reports for it, if it's a drum kit SDL's XInput
// backend reads and Windows has a match; otherwise nothing. Windows only;
// remembers a kit's answer, which can't change while it's plugged in.
std::optional<Caps360> WindowsDrumCaps(std::string_view guid, const Caps360& reported);

}
