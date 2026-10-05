// Checks the name a loose file replacing an ARK file has in game:\
// (src/Hooks/loose_name.h): its ".." folders become "(..)" folders, as
// RB3Enhanced lays them out.

#include <doctest/doctest.h>
#include "src/Hooks/loose_name.h"

using band3::LooseName;

TEST_CASE("a path without .. folders is kept") {
    CHECK(LooseName("char/main/shared/gen/prefabs.milo_xbox") ==
          "char/main/shared/gen/prefabs.milo_xbox");
    CHECK(LooseName("charnames.zbm") == "charnames.zbm");
}

TEST_CASE("each .. folder becomes a (..) folder") {
    CHECK(LooseName("../../system/run/config/gen/movie.dtb") ==
          "(..)/(..)/system/run/config/gen/movie.dtb");
    CHECK(LooseName("ui/../../system/x.dtb") == "ui/(..)/(..)/system/x.dtb");
}

TEST_CASE("only a whole .. folder changes") {
    CHECK(LooseName("songs/..x/a..b/x..") == "songs/..x/a..b/x..");
    CHECK(LooseName("./gen/main_xbox.hdr") == "./gen/main_xbox.hdr");
}

TEST_CASE("backslashes separate folders too, and come out as slashes") {
    CHECK(LooseName("..\\system\\run\\x.dtb") == "(..)/system/run/x.dtb");
}

TEST_CASE("a trailing separator and an empty path are kept") {
    CHECK(LooseName("char/main/") == "char/main/");
    CHECK(LooseName("") == "");
}
