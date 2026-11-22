#include "login/wire.h"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <string_view>

using namespace tmloader;
using namespace tmloader::wire;

// Vectors computed with tmserver's src/tmserver/protocol/wire.py.
namespace
{

// The throwaway test key from tmserver's tests/support/irc.py: p = 2^128 - 173, q = 2^128 - 233.
constexpr std::string_view kModulusLittleEndian = "759d00000000000000000000000000006afeffffffffffffffffffffffffffff";
constexpr std::string_view kExponentBigEndian   = "3a673a673a673a673a673a673a673a0a25e225e225e225e225e225e225e24a2b";
constexpr std::string_view kSessionKey          = "0102030405060708";

auto fromHex(std::string_view hex) -> Bytes
{
    auto bytes = Bytes{};
    for (size_t i = 0; i < hex.size(); i += 2)
    {
        bytes.push_back(static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    }

    return bytes;
}

auto toHex(std::span<const std::uint8_t> bytes) -> std::string
{
    constexpr auto kDigits = "0123456789abcdef";
    auto           hex     = std::string{};
    for (const auto byte : bytes)
    {
        hex += kDigits[byte >> 4];
        hex += kDigits[byte & 15];
    }

    return hex;
}

auto asText(const Bytes& bytes) -> std::string
{
    return { bytes.begin(), bytes.end() };
}

auto testKey() -> RsaKey
{
    auto       key      = RsaKey{};
    const auto modulus  = fromHex(kModulusLittleEndian);
    auto       exponent = fromHex(kExponentBigEndian);
    std::ranges::reverse(exponent);
    std::ranges::copy(modulus, key.modulus.begin());
    std::ranges::copy(exponent, key.privateExponent.begin());
    return key;
}

auto testIv() -> std::uint64_t
{
    auto       iv      = std::uint64_t{};
    const auto modulus = fromHex(kModulusLittleEndian);
    std::memcpy(&iv, modulus.data(), sizeof(iv));
    return iv;
}

} // namespace

TEST_CASE("base64 matches the PlayOnline alphabet and padding", "[login]")
{
    const auto modulus = fromHex(kModulusLittleEndian);
    REQUIRE(encodeBase64(modulus, false) == "E2dTTTTTTTTTTTTTTTTTTc@uXXXXXXXXXXXXXXXXXXN");
    const auto counting = Bytes{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    REQUIRE(encodeBase64(counting, true) == "TTIGTt7nSAY3G7TT");
    REQUIRE(encodeBase64(counting, false) == "TTIGTt7nSAY3G7");
    REQUIRE(toHex(decodeBase64("TSG8In")) == "001083105000");
    const auto decoded = decodeBase64("E2dTTTTTTTTTTTTTTTTTTc@uXXXXXXXXXXXXXXXXXXN");
    REQUIRE(Bytes(decoded.begin(), decoded.begin() + 32) == modulus);
    REQUIRE_THROWS(decodeBase64("abc+"));
}

TEST_CASE("line checksums match the client's", "[login]")
{
    REQUIRE(checksumChars("") == "????");
    REQUIRE(checksumChars("a") == "WO??");
    REQUIRE(checksumChars("abc") == "WuH`");
    REQUIRE(checksumChars("USER x 8 * :abc") == "H}`U");
    REQUIRE(checksumChars("LOGIN EEEEEEEF :tm-local-test") == "r]X?");
}

TEST_CASE("MD5 runs from the standard or a zero state", "[login]")
{
    const auto abc = Bytes{ 'a', 'b', 'c' };
    REQUIRE(toHex(md5(abc, kMd5Iv)) == "900150983cd24fb0d6963f7d28e17f72");
    REQUIRE(toHex(md5(abc, {})) == "c974bc19183bcf1b909e7f07ea1e8fbb");
    auto seventy = Bytes(70);
    for (size_t i = 0; i < seventy.size(); ++i)
    {
        seventy[i] = static_cast<std::uint8_t>(i);
    }

    REQUIRE(toHex(md5(seventy, {})) == "939c3a633e58d3198bc7b21ce21accc7");
}

TEST_CASE("Blowfish uses the PlayOnline key schedule", "[login]")
{
    const auto cipher = Blowfish(fromHex(kSessionKey));
    REQUIRE(cipher.encrypt(0) == 0xc6e03b0eb7e710d8);
    REQUIRE(cipher.encrypt(0x1122334455667788) == 0x9ff3d1c352cb1164);
}

TEST_CASE("lines are framed and encrypted as the server expects", "[login]")
{
    const auto cipher = Blowfish(fromHex(kSessionKey));
    const auto iv     = testIv();
    REQUIRE(iv == 0x9d75);
    const auto framed = frameLine("LOGIN EEEEEEEF :tm-local-test", &cipher, iv);
    REQUIRE(toHex(Bytes(framed.begin(), framed.end())) ==
            "52d0dbee262594ef731e77f7434865bf7ae51f591954454da8e534bda05c395fb50d0a");
    const auto reply = asText(fromHex("24eceed1483ce19a161e77f7434b00c048a8020444041014b3a669f7b54c0763aa100634c56d5ab69508c56c8e0d0a"));
    REQUIRE(unframeLine(reply, &cipher, iv) == ":srv 900 EEEEEEEF 0123456789abcd :created");
    const auto notice = asText(fromHex("3a737276204e4f544943452041555448204142434445466e4d4a5e0d0a"));
    REQUIRE(frameLine(":srv NOTICE AUTH ABCDEF", nullptr, 0) == notice);
    REQUIRE(unframeLine(notice, nullptr, 0) == ":srv NOTICE AUTH ABCDEF");
}

TEST_CASE("encryption never produces or changes CR and LF", "[login]")
{
    const auto cipher = Blowfish(fromHex(kSessionKey));
    const auto line   = std::string(":srv 422 U00000000 :no motd\n\r\n\r\n\r");
    const auto sealed = crypt64(cipher, 0x1122334455667788, line);
    REQUIRE(crypt64(cipher, 0x1122334455667788, sealed) == line);
    const auto framed = frameLine("NOTICE x :y", &cipher, 42);
    REQUIRE(framed.ends_with("\r\n"));
    REQUIRE(framed.find('\n') == framed.size() - 1);
}

TEST_CASE("a bad checksum or frame is rejected", "[login]")
{
    REQUIRE_THROWS(unframeLine(":srv NOTICE AUTH ABCDEFxxxx\r\n", nullptr, 0));
    REQUIRE_THROWS(unframeLine("ab\r\n", nullptr, 0));
    REQUIRE_THROWS(unframeLine(":srv NOTICE AUTH ABCDEFnMJ^\n", nullptr, 0));
}

TEST_CASE("the server's wrapped session key decrypts with the test key", "[login]")
{
    const auto wrapped = decodeBase64("S_H_RuHj_PnYNHNKde_zodbU5EsU7s@_G5yOEGztI6JT");
    REQUIRE(toHex(std::span(wrapped).first(32)) == "0772778fe26cdea15cf09f0bd14dd33349a191d621418af70a468e7424f013de");
    const auto block = rsaDecrypt(testKey(), std::span<const std::uint8_t, 32>(wrapped.data(), 32));
    REQUIRE(toHex(block) == "0002111111111111111111111111111111111111111111000807060504030201");
    REQUIRE(toHex(sessionKey(block)) == kSessionKey);
    REQUIRE(toHex(rsaEncrypt(testKey().modulus, block)) == "0772778fe26cdea15cf09f0bd14dd33349a191d621418af70a468e7424f013de");
}

TEST_CASE("a block that is not PKCS#1 type 2 carries no key", "[login]")
{
    auto block = std::array<std::uint8_t, 32>{};
    REQUIRE_THROWS(sessionKey(block));
    block[1] = 2;
    std::ranges::fill(std::span(block).subspan(2), std::uint8_t{ 0x11 });
    REQUIRE_THROWS(sessionKey(block));
    block[20] = 0;
    REQUIRE_THROWS(sessionKey(block));
}

TEST_CASE("generated keys are odd 256-bit moduli that decrypt their own blocks", "[login]")
{
    for (int i = 0; i < 3; ++i)
    {
        const auto key = generateRsaKey();
        REQUIRE((key.modulus[0] & 1) == 1);
        REQUIRE(key.modulus[31] >= 0x40);
        auto block = std::array<std::uint8_t, 32>{};
        block[1]   = 2;
        std::ranges::fill(std::span(block).subspan(2, 21), std::uint8_t{ 0x5a });
        std::ranges::copy(fromHex(kSessionKey), block.begin() + 24);
        const auto sealed = rsaEncrypt(key.modulus, block);
        REQUIRE(rsaDecrypt(key, sealed) == block);
    }
}
