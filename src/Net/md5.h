#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace band3::net {

// MD5 (RFC 1321), for Last.fm's api_sig (src/Net/scrobble.h), which is an MD5
// of the call's parameters. Not for anything that needs MD5 to be secure: it's
// here to match a protocol.
std::array<uint8_t, 16> Md5(std::span<const uint8_t> data);

// the digest as 32 lowercase hex digits
std::string Md5Hex(std::string_view text);

}
