#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

// Which kind of guitar RB3 is told a guitar is. RB3 goes by the XInput subtype
// (rb3-xenon, ReadSingleXinputJoypad): 6 and 11 (bass) are Rock Band guitars,
// whose left trigger is the pickup switch that picks the overdrive effect; 7 is
// a Guitar Hero guitar (kJoypadXboxRoGuitar), which gets no effect switch. A
// Guitar Hero guitar's left trigger is a tilt sensor instead (PlasticBand), so
// read as a Rock Band guitar, turning it face up changes the effect.
//
// The SDK's SDL backend reports every guitar as 6, but SDL keeps the device's
// own subtype in the GUID of an XInput device, which `auto` goes by. Kept apart
// from input_system.h, which needs the SDK, so the unit tests can use it.

namespace band3::input {

enum class GuitarType {
    kAuto,        // the subtype the guitar reports itself, where it can be read
    kRockBand,    // every guitar a Rock Band guitar, with its pickup switch
    kGuitarHero,  // every guitar a Guitar Hero guitar, without one
};

// guitar_type's value: auto, rock_band or guitar_hero
std::optional<GuitarType> ParseGuitarType(std::string_view value);

// whether RB3 plays a device of this subtype as a 5-fret guitar
bool IsGuitarSubtype(uint8_t subtype);

// The XInput subtype SDL keeps in the GUID of a device its XInput backend reads
// ('x' in byte 14, the subtype in byte 15), from the GUID as SDL_GUIDToString
// writes it. None for any other GUID.
std::optional<uint8_t> XInputSubtypeFromGuid(std::string_view guid);

// The subtype RB3 is to read for a device that reports `reported`, whose own
// subtype is `native` where it's known. Only guitars change.
uint8_t GuitarSubtypeFor(GuitarType type, uint8_t reported, std::optional<uint8_t> native);

}
