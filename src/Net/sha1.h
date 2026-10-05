#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace band3::net {

// SHA-1 and HMAC-SHA1 (RFC 3174, RFC 2104), for the Liveless Rooms login proof
// RB3Enhanced signs with the Xbox's XeCryptHmacSha. Not for anything that needs
// SHA-1 to be secure: it's here to match a protocol, and band3 has no other.
std::array<uint8_t, 20> Sha1(std::span<const uint8_t> data);
std::array<uint8_t, 20> HmacSha1(std::span<const uint8_t> key, std::span<const uint8_t> data);

}
