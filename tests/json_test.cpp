// Checks the small JSON reader (src/Net/json.cpp) that reads web services'
// replies.

#include <doctest/doctest.h>
#include <string>
#include "src/Net/json.h"

using namespace band3::json;

TEST_CASE("objects, arrays and scalars read back") {
    const auto v = Parse(R"( {"a": 1.5, "b": [true, false, null], "c": {"d": "e"}, "n": -2e3} )");
    REQUIRE(v);
    CHECK((*v)["a"].Number() == 1.5);
    REQUIRE((*v)["b"].Items().size() == 3);
    CHECK((*v)["b"].Items()[0].Bool() == true);
    CHECK((*v)["b"].Items()[1].Bool(true) == false);
    CHECK((*v)["b"].Items()[2].IsNull());
    CHECK((*v)["c"]["d"].Text() == "e");
    CHECK((*v)["n"].Number() == -2000);
}

TEST_CASE("missing members and wrong types read as null, empty or the fallback") {
    const auto v = Parse(R"({"s": "x", "list": [1]})");
    REQUIRE(v);
    CHECK((*v)["nope"].IsNull());
    CHECK((*v)["nope"]["deeper"].IsNull());
    CHECK((*v)["s"]["x"].IsNull());
    CHECK((*v)["s"].Items().empty());
    CHECK((*v)["list"].Text().empty());
    CHECK((*v)["s"].Number(7) == 7);
}

TEST_CASE("numbers in strings read as numbers, and numbers as text") {
    const auto v = Parse(R"({"a": "4", "b": "-1", "c": "", "d": 190, "e": 0.25})");
    REQUIRE(v);
    CHECK((*v)["a"].Number() == 4);
    CHECK((*v)["b"].Number() == -1);
    CHECK((*v)["c"].Number(9) == 9);
    CHECK((*v)["d"].Text() == "190");
    CHECK((*v)["e"].Text() == "0.25");
}

TEST_CASE("string escapes decode, \\u to UTF-8") {
    const auto v = Parse(R"(["a\"b\\c\/d\n", "é中", "🎸", "\ud83c"])");
    REQUIRE(v);
    const auto& items = v->Items();
    CHECK(items[0].Text() == "a\"b\\c/d\n");
    CHECK(items[1].Text() == "\xC3\xA9\xE4\xB8\xAD");
    CHECK(items[2].Text() == "\xF0\x9F\x8E\xB8");  // one character from a surrogate pair
    CHECK(items[3].Text() == "\xEF\xBF\xBD");      // half a pair
}

TEST_CASE("anything that isn't one whole JSON value fails") {
    CHECK(!Parse(""));
    CHECK(!Parse("{"));
    CHECK(!Parse(R"({"a":1,})"));
    CHECK(!Parse("[1 2]"));
    CHECK(!Parse("{} {}"));
    CHECK(!Parse("tru"));
    CHECK(!Parse("\"unterminated"));
    CHECK(!Parse("\"bad \\x escape\""));
    CHECK(!Parse("\"raw\nnewline\""));
    CHECK(!Parse("<html>"));
    CHECK(!Parse(std::string(100, '[') + std::string(100, ']')));  // nested too deep
    CHECK(Parse(std::string(10, '[') + std::string(10, ']')));
}
