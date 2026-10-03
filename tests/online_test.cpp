// Checks band3's online helpers (src/Net/online.cpp): the hosts it sends to
// GoCentral, as RB3Enhanced redirects them, the usernames it takes as a
// GoCentral account, and Liveless's address to join.

#include <doctest/doctest.h>
#include "src/Net/online.h"

using band3::online::IsOwnAccountName;
using band3::online::IsRockCentralHost;
using band3::online::ParseEndpoint;

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

TEST_CASE("an address to join, with or without a port") {
    auto e = ParseEndpoint("192.168.1.20", 9103);
    REQUIRE(e);
    CHECK(e->host == "192.168.1.20");
    CHECK(e->port == 9103);

    e = ParseEndpoint(" friend.example.net:9104 ", 9103);
    REQUIRE(e);
    CHECK(e->host == "friend.example.net");
    CHECK(e->port == 9104);
}

TEST_CASE("a host joins their own game, on their own port") {
    using band3::online::ParseJoinAddress;
    auto e = ParseJoinAddress("127.0.0.1", 9203);
    REQUIRE(e);
    CHECK(e->port == 9203);
    e = ParseJoinAddress("localhost", 9203);
    REQUIRE(e);
    CHECK(e->port == 9203);
    // a port given, or another player's game, keeps it or RB3Enhanced's
    e = ParseJoinAddress("127.0.0.1:9103", 9203);
    REQUIRE(e);
    CHECK(e->port == 9103);
    e = ParseJoinAddress("192.168.1.20", 9203);
    REQUIRE(e);
    CHECK(e->port == 9103);
    CHECK_FALSE(ParseJoinAddress("", 9203));
}

TEST_CASE("a blank address or a bad port isn't one") {
    CHECK_FALSE(ParseEndpoint("", 9103));
    CHECK_FALSE(ParseEndpoint("   ", 9103));
    CHECK_FALSE(ParseEndpoint(":9103", 9103));
    CHECK_FALSE(ParseEndpoint("host:", 9103));
    CHECK_FALSE(ParseEndpoint("host:0", 9103));
    CHECK_FALSE(ParseEndpoint("host:65536", 9103));
    CHECK_FALSE(ParseEndpoint("host:91x3", 9103));
}
