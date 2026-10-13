#pragma once

#include <cstdint>
#include <span>
#include <string>

// Decrypting a .mogg (mogg.h) whose Ogg is encrypted: versions 0x0B to 0x10,
// the ones Rock Band 3 plays (the disc's songs are 0x10; DLC, Rock Band
// Network songs and C3 customs use the others). The Ogg is AES-128 in CTR
// mode; 0x0B's key is fixed, and from 0x0C each mogg's key is worked out from
// values in its header. A port of themethod3's (github.com/DarkRTA/themethod3,
// GPL-3.0), whose keys are public.

namespace band3::video {

// whether DecryptMogg takes a mogg of this version
bool MoggDecryptable(uint32_t version);

// The mogg's Ogg decrypted in place, its first page's "HMXA" made "OggS" again
// as the game does, and its version set to 0x0A, so it reads as an
// unencrypted one. False, with why in `error`, for a version it doesn't take,
// a header too short for its version, or an Ogg that doesn't decrypt to one.
bool DecryptMogg(std::span<uint8_t> mogg, std::string& error);

}
