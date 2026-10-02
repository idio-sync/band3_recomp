#pragma once
#include <string_view>

namespace band3::gocentral {

// The GoCentral server RB3Enhanced's Xbox 360 builds connect to.
inline constexpr std::string_view kDefaultAddress = "gocentral-xbox.rbenhanced.rocks";

// Whether RB3 asking Quazal for `host` means Rock Central: one of Harmonix's
// *.hmxservices.com servers, or the stand-in address Quazal logs into when it
// bypasses the Xbox Live secure gateway. RB3Enhanced sends both to GoCentral.
bool IsRockCentralHost(std::string_view host);

// Whether `username` can be a player's GoCentral account. GoCentral knows an
// Xbox player by gamertag alone, so a blank one, or the "User" every profile
// here starts as (band3_config.ini's too), would be everyone's account.
bool IsOwnAccountName(std::string_view username);

}
