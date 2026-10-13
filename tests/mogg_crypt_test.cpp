// Checks decrypting a .mogg (src/Video/mogg_crypt.cpp) against moggs
// encrypted by themethod3, the decryptor it was ported from: one of each
// version, and two whose first page is marked HMXA. The moggs are made up
// (random header values, an Ogg page's start), not a song's.

#include <doctest/doctest.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "src/Video/mogg_crypt.h"

using namespace band3::video;

namespace {

using Bytes = std::vector<uint8_t>;

struct Vector {
    uint32_t version;
    bool hmxa;
    const char* encrypted;
    const char* decrypted;
};

const Vector kVectors[] = {
    {0x0B, false,
         "0b0000003400000010000000204e000002000000371a125121b5956143c0ee2d55fb638c77fee0b6f5f9275faf297e2c"
         "97df5404f88f242b90df4ba76d21f9727d4726c02e6faecfd519aa0a419691cbad6a0ebc2590a44621402c9eb44010e0"
         "771cfb43cf002d655799d7665c0e7879ae7894e524bd8a0fadf8d733ea6cbe7a8e2c7b53",
         "0a0000003400000010000000204e000002000000371a125121b5956143c0ee2d55fb638c77fee0b6f5f9275faf297e2c"
         "97df54044f676753620b5821cf68259ccbee0207ffcd7464abf7bb7d6a25e6bfa294890d6b90f256c646e9f06e6e5a05"
         "a9bf717fd7480059a9144b373ec5809d93fb7a4cfcb8a073991ef0d302318f048f797471"},
    {0x0C, false,
         "0c0000006c00000010000000204e000002000000e8aabd633f262e25125e84554c96c97852d0c6dbb0ddb6ed4a29e3ea"
         "e33985e100039f6f599ca6c674f725c41e97bc42c57921e806a1ff0de100e89365a8aaaecf81b41262f37cfc813ae8f7"
         "ca55497837cc699587b29e757e46373565ca732efc3d62421df60616a1dac08ce832a1f21d6191a1a70f16d5fc8bcb99"
         "d16bc95571d0f7a5d7534021b06b5ccdc10507b8bf6bb98912746aff75504f7aba5d9679869454393da702b2",
         "0a0000006c00000010000000204e000002000000e8aabd633f262e25125e84554c96c97852d0c6dbb0ddb6ed4a29e3ea"
         "e33985e100039f6f599ca6c674f725c41e97bc42c57921e806a1ff0de100e89365a8aaaecf81b41262f37cfc813ae8f7"
         "ca55497837cc699587b29e754f67675393cb680a8fc302bd8c68c0d9aaaa738dccc4b2ac75d3054f175725d3514f90fc"
         "d9e0c55e1ebed056edcc092787a02f1902eb3ea2edeba1f5806cad88d8f787610836ca4bfdb8ac3680866f61"},
    {0x0D, false,
         "0d0000006c00000010000000204e000002000000993a69765d97c8e9e1fc1b7c42322e642da3ac016cc1467ce42949a8"
         "3094b7bd0dcb6ad9502df46b1a8625eb713f767d8b25ce6b604b439e58dbea6729bcca4f337277cffda00e07940576e9"
         "ebec22729851d2d1644ff0b40ba75a01e27fd4d4235c40d4805afdca03f303533dde232565a631ee952e96bb6cc4f0ca"
         "92c434a8f57483dce8cef1c5d4e72000058f7b62838132b9a35b4c8db30bf9fdbcb4236368718078035cb3e5",
         "0a0000006c00000010000000204e000002000000993a69765d97c8e9e1fc1b7c42322e642da3ac016cc1467ce42949a8"
         "3094b7bd0dcb6ad9502df46b1a8625eb713f767d8b25ce6b604b439e58dbea6729bcca4f337277cffda00e07940576e9"
         "ebec22729851d2d1644ff0b44f676753929b56c722cf63087eb190e0522356170810f1e8e5f917e2aee0f103ac525963"
         "1d9c842d676896b7a146c7df4fed2118532af6c878d48ed62333bf95fdb16069afcfd2cbffa40a95e867d5b0"},
    {0x0E, false,
         "0e0000006c00000010000000204e0000020000004acb14887b0861aeb19ab1a439cd94500975922627a4d50a7f29ae66"
         "7cefe8991a93354247bd4311c0152513c4e830b751d07aefbaf6872fcfb6ed3becd0eaf097633a8b984da112a6d104dc"
         "0d82fb6bf8d53b0d42ed43f367921d2099cefbd5056c8fd8716204de2e952f2717656ab1cd741d6a4e9ed91168f1465e"
         "2745a962534ecf270c1daaccfe0a423bedcdf1264583ad38eddbc619b8cb1308f0cbc0723b9392c53884e974",
         "0a0000006c00000010000000204e0000020000004acb14887b0861aeb19ab1a439cd94500975922627a4d50a7f29ae66"
         "7cefe8991a93354247bd4311c0152513c4e830b751d07aefbaf6872fcfb6ed3becd0eaf097633a8b984da112a6d104dc"
         "0d82fb6bf8d53b0d42ed43f34f676753926a4485b6dbc45271fb60e7fb9c39a0455b3023551f29764468be33065522c9"
         "625844fcaf115b1855c0859717391317a469afee03bc7cb6c6f9d0a2216b38715569db4b018f69f34f473bff"},
    {0x0F, false,
         "0f0000006c00000010000000204e000002000000fa5bc09b9979fa72803847cc2f68fa3de448784ce38864991a281424"
         "c8491975275b00ab3e4e91b665a4253b1790eaf2177c277314a0cbc04691ef0fafe40a91fb54fd4834fa341eb99d92ce"
         "2e18d4645859a448208a963284b66fdd2bde1c5870dee50a6eb230ac87acd60662de7236164257dc97dde02a099ba0ff"
         "638ac398a35cc054c4ac371d9c25178514fb6bba579ea35581600d1d5eee467edf2ce3de3bbc5b87afaedc21",
         "0a0000006c00000010000000204e000002000000fa5bc09b9979fa72803847cc2f68fa3de448784ce38864991a281424"
         "c8491975275b00ab3e4e91b665a4253b1790eaf2177c277314a0cbc04691ef0fafe40a91fb54fd4834fa341eb99d92ce"
         "2e18d4645859a448208a96324f676753913a324349e7269d634530eea3151c2a81a66f5ec5443b09daf18a636157eb30"
         "a71503cbf8bb2179093a434fdf850517f4a867138ea5699669c0e1af46251178fc02e3cb037bc752b727a14d"},
    {0x10, false,
         "100000006c00000010000000204e000002000000abeb6badb7ea94364fd6def326036029bf1a5e719e6cf327b4287ae2"
         "14a44a513423cb1435dedf5b0b3325636a39a52ddd28d4f76e4a0f51bd6cf1e372f92b325e44bf05cfa7c729cc681fc0"
         "4faead5db9de0e84fe28e870d8404742c31fc51453d4b1d62c29b406d48cc92f33b7051d21d8247e18dd8385da58e0a4"
         "a34f9edf3b856a5b00212e369f2db153d59321adf5031e7bdaf47bd5b9bc67db2d451dc83253e992e569fc6c",
         "0a0000006c00000010000000204e000002000000abeb6badb7ea94364fd6def326036029bf1a5e719e6cf327b4287ae2"
         "14a44a513423cb1435dedf5b0b3325636a39a52ddd28d4f76e4a0f51bd6cf1e372f92b325e44bf05cfa7c729cc681fc0"
         "4faead5db9de0e84fe28e8704f676753900a2001dcf287e7558ffff54b8effb3bef2ad9a356a4d9c70795693bb5ab497"
         "ebd1c2994165e7dabdb40107a6d1f71645e71f39188d57770b87f2bc6bdfea80a39beb4a056726b01e08079c"},
    {0x10, true,
         "100000006c00000010000000204e0000020000005c7b17bfd55b2dfb1f74741b1d9fc5159bed44975a5083b64f28dfa0"
         "61ff7c2d40eb967e2c6f2d00b1c2258bbde25f68a3d4807bc9f553e23546f3b7350d4bd3c23582c16a545a34de34adb3"
         "70448656196277c0dcc63baff8315c41b886f09febff5ea277e50969505149323882279e8d473db97d6337f8e3005745"
         "e9a2a67d682a4988464347ec808e9f0d41425163b83e18cc3dcd083a7ddcdbb0310553dcc7544419768faa46",
         "0a0000006c00000010000000204e0000020000005c7b17bfd55b2dfb1f74741b1d9fc5159bed44975a5083b64f28dfa0"
         "61ff7c2d40eb967e2c6f2d00b1c2258bbde25f68a3d4807bc9f553e23546f3b7350d4bd3c23582c16a545a34de34adb3"
         "70448656196277c0dcc63baf4f67675390da0ebf6ffee932e3c8d552f307e23d5032c4efa58f5f2f060222c3165d7dfe"
         "308d8168890ead3c712ebfbf6e1de9159627d85fa3764457ae4d03c98f99c3884934f4ca0753840f86e86deb"},
    {0x0C, true,
         "0c0000006c00000010000000204e0000020000000d0bc3d2f3ccc7bfee120b43133a2b0176bf2abc15331244e928455e"
         "ad59ad094db361e723ff7ba5565124b2118a19a2697f2dff239f9673ac21f58af8216b742626457e0601ed3ff1ff3ba5"
         "91da5e4f7ae7e0fbba638dee2d4c5d3354a73a0aa536ed9f910768fc98ce51cbb57bd66c9e9af2a10dafbf40032b0da2"
         "e0384aa1dc8a8090371fe484dc88b4f76c4407e73fc963a8ba3f31a8ef6d7349b59609ddc51196427dbf1d2a",
         "0a0000006c00000010000000204e0000020000000d0bc3d2f3ccc7bfee120b43133a2b0176bf2abc15331244e928455e"
         "ad59ad094db361e723ff7ba5565124b2118a19a2697f2dff239f9673ac21f58af8216b742626457e0601ed3ff1ff3ba5"
         "91da5e4f7ae7e0fbba638dee4f6767538fa9fc7d020a4a7ccdf788699b80c5c66967582f15b571c29d8beef370604664"
         "754a4037d2b8739d25a77d77366adb14e66690852e5e3238511414d6b4539c90f0cdfc4a093fe36dedc9d33a"},
};

Bytes Hex(std::string_view hex) {
    Bytes out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(uint8_t(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    return out;
}

}

TEST_CASE("each encrypted version decrypts as themethod3 decrypts it") {
    for (const Vector& v : kVectors) {
        CAPTURE(v.version);
        CAPTURE(v.hmxa);
        Bytes mogg = Hex(v.encrypted);
        REQUIRE(mogg[0] == v.version);
        CHECK(MoggDecryptable(v.version));
        std::string error;
        REQUIRE(DecryptMogg(mogg, error));
        CHECK(error.empty());
        CHECK(mogg == Hex(v.decrypted));
        CHECK(mogg[0] == 0x0A);
    }
}

TEST_CASE("a mogg that's already plain, or of a later version, isn't decrypted") {
    CHECK_FALSE(MoggDecryptable(0x0A));
    CHECK_FALSE(MoggDecryptable(0x11));
    Bytes mogg = Hex(kVectors[0].decrypted);
    const Bytes before = mogg;
    std::string error;
    CHECK_FALSE(DecryptMogg(mogg, error));
    CHECK(error.find("0x0A") != std::string::npos);
    CHECK(mogg == before);
}

TEST_CASE("a mogg that decrypts to something other than an Ogg fails") {
    Bytes mogg = Hex(kVectors[5].encrypted);
    // its key index changed: another key
    const size_t base = 20 + 2 * 8;
    mogg[base + 64] ^= 1;
    std::string error;
    CHECK_FALSE(DecryptMogg(mogg, error));
    CHECK(error == "it didn't decrypt to an Ogg stream");
}

TEST_CASE("a header too short for its version fails") {
    Bytes mogg = Hex(kVectors[5].encrypted);
    // the seek map claims more entries than the header holds
    mogg[16] = 40;
    std::string error;
    CHECK_FALSE(DecryptMogg(mogg, error));
    CHECK(error == "its header is cut short");
    mogg.resize(12);
    CHECK_FALSE(DecryptMogg(mogg, error));
}
