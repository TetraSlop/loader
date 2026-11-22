#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The PlayOnline IRC wire format used by the game port, as far as the loader's
// LOGIN step needs it. Ported from tmserver's protocol/wire.py; the layout is
// in TetraPackets irc/session.md.
namespace tmloader::wire
{

using Bytes = std::vector<std::uint8_t>;

// PlayOnline's base64: its own alphabet, big-endian groups. Without padding,
// a short final group is cut to the characters it needs.
auto encodeBase64(std::span<const std::uint8_t> data, bool pad) -> std::string;
// Decodes whole groups, filling a short final group with zero; throws on a
// character outside the alphabet.
auto decodeBase64(std::string_view text) -> Bytes;

// The four checksum characters sent before CRLF on every line.
auto checksumChars(std::string_view message) -> std::string;

// MD5 starting from an arbitrary state, as the key schedule below needs.
using Md5State = std::array<std::uint32_t, 4>;
inline constexpr Md5State kMd5Iv{ 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
auto                      md5(std::span<const std::uint8_t> data, const Md5State& state) -> std::array<std::uint8_t, 16>;

// Blowfish rounds with PlayOnline's own key schedule.
class Blowfish
{
public:
    explicit Blowfish(std::span<const std::uint8_t> key);
    auto encrypt(std::uint64_t block) const -> std::uint64_t;

private:
    auto encryptHalves(std::uint32_t low, std::uint32_t high) const -> std::pair<std::uint32_t, std::uint32_t>;
    auto round(std::uint32_t x) const -> std::uint32_t;

    std::array<std::uint32_t, 18>                 p_{};
    std::array<std::array<std::uint32_t, 256>, 4> s_{};
};

// Output feedback from iv, restarted for every line. CR and LF are never
// produced or changed, so encrypting and decrypting are the same operation.
auto crypt64(const Blowfish& cipher, std::uint64_t iv, std::string_view data) -> std::string;

// A message with its checksum and CRLF, encrypted when a cipher is given.
auto frameLine(std::string_view message, const Blowfish* cipher, std::uint64_t iv) -> std::string;
// The message in a received line (CRLF included); throws on a bad frame or checksum.
auto unframeLine(std::string_view line, const Blowfish* cipher, std::uint64_t iv) -> std::string;

// A 256-bit RSA key with public exponent 0xFFFF, as the PlayOnline client uses.
// Both numbers are little-endian.
struct RsaKey
{
    std::array<std::uint8_t, 32> modulus{};
    std::array<std::uint8_t, 32> privateExponent{};
};

inline constexpr std::uint32_t kRsaExponent = 0xffff;

// Two random 128-bit primes with gcd(p - 1, e) = 1, so the modulus is odd and
// at least 255 bits long.
auto generateRsaKey() -> RsaKey;
// RSA on a 32-byte big-endian block. Throws when the block is not below the modulus.
auto rsaDecrypt(const RsaKey& key, std::span<const std::uint8_t, 32> block) -> std::array<std::uint8_t, 32>;
auto rsaEncrypt(std::span<const std::uint8_t, 32> modulus, std::span<const std::uint8_t, 32> block) -> std::array<std::uint8_t, 32>;

// The 8-byte Blowfish key in a decrypted PKCS#1 v1.5 type 2 block, which
// carries it reversed. Throws when the block is not of that form.
auto sessionKey(std::span<const std::uint8_t, 32> block) -> std::array<std::uint8_t, 8>;

} // namespace tmloader::wire
