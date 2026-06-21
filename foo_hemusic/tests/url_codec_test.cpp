#include "net/url_codec.h"

#include <string>

#include <catch2/catch_test_macros.hpp>

using hemusic::url::percentDecode;
using hemusic::url::percentEncode;

TEST_CASE("percentDecode inverts percent-encoding") {
    CHECK(percentDecode("abcXYZ-_.~09") == "abcXYZ-_.~09");
    CHECK(percentDecode("a%20b%2Fc") == "a b/c");
    CHECK(percentDecode("%41%42") == "AB");  // uppercase hex
    CHECK(percentDecode("%61%62") == "ab");  // lowercase hex
}

TEST_CASE("percentDecode leaves '+' literal (space is %20, both ends ours)") {
    CHECK(percentDecode("a+b") == "a+b");
}

TEST_CASE("percentDecode passes malformed escapes through verbatim") {
    CHECK(percentDecode("%") == "%");      // dangling
    CHECK(percentDecode("%4") == "%4");    // one digit short
    CHECK(percentDecode("%zz") == "%zz");  // non-hex digits
    CHECK(percentDecode("a%2") == "a%2");  // truncated at end
}

TEST_CASE("encode/decode round-trips arbitrary bytes including UTF-8") {
    // \xE4\xBD\xA0 is U+4F60; kept as raw bytes so the test needs no /utf-8.
    for (const std::string& s :
         {std::string("plain"), std::string("a b&c=d"),
          std::string("100%/path?x"), std::string("\xE4\xBD\xA0 hi"),
          std::string("+plus+"), std::string("")}) {
        CHECK(percentDecode(percentEncode(s)) == s);
    }
}
