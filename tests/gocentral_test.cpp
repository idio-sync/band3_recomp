// Checks which hosts band3 sends to GoCentral (src/Net/gocentral.cpp): the
// ones RB3Enhanced redirects.

#include <doctest/doctest.h>
#include "src/Net/gocentral.h"

using band3::gocentral::IsOwnAccountName;
using band3::gocentral::IsRockCentralHost;

TEST_CASE("a blank or default username isn't an account of one's own") {
    CHECK_FALSE(IsOwnAccountName(""));
    CHECK_FALSE(IsOwnAccountName("   "));
    CHECK_FALSE(IsOwnAccountName("User"));
    CHECK_FALSE(IsOwnAccountName("user"));
    CHECK_FALSE(IsOwnAccountName(" USER "));
    CHECK(IsOwnAccountName("Users"));
    CHECK(IsOwnAccountName("Use"));
    CHECK(IsOwnAccountName("StageDiver42"));
}

TEST_CASE("Rock Central's servers go to GoCentral") {
    CHECK(IsRockCentralHost("rb3-xbox360.hmxservices.com"));
    CHECK(IsRockCentralHost("DummySandboxAddress.quazal.com"));
}

TEST_CASE("other hosts are left alone") {
    CHECK_FALSE(IsRockCentralHost(""));
    CHECK_FALSE(IsRockCentralHost(".hmxservices.com"));
    CHECK_FALSE(IsRockCentralHost("hmxservices.com"));
    CHECK_FALSE(IsRockCentralHost("example.com"));
    CHECK_FALSE(IsRockCentralHost("gocentral-xbox.rbenhanced.rocks"));
    CHECK_FALSE(IsRockCentralHost("other.quazal.com"));
}
