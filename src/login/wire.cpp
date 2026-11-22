#include <windows.h>

#include "login/wire.h"
#include <algorithm>
#include <bcrypt.h>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace tmloader::wire
{
namespace
{

constexpr std::string_view kBase64Alphabet = "TSG8IncW3HFKokOg79qzeCmZs2yBYEQVAUxR5rbwi4P@jMDLtpvad0f_J1hlN6uX";
constexpr size_t           kScheduleBytes  = 0x1000;

constexpr std::array<std::uint32_t, 64> kMd5Constants{
    0xd76aa478,
    0xe8c7b756,
    0x242070db,
    0xc1bdceee,
    0xf57c0faf,
    0x4787c62a,
    0xa8304613,
    0xfd469501,
    0x698098d8,
    0x8b44f7af,
    0xffff5bb1,
    0x895cd7be,
    0x6b901122,
    0xfd987193,
    0xa679438e,
    0x49b40821,
    0xf61e2562,
    0xc040b340,
    0x265e5a51,
    0xe9b6c7aa,
    0xd62f105d,
    0x02441453,
    0xd8a1e681,
    0xe7d3fbc8,
    0x21e1cde6,
    0xc33707d6,
    0xf4d50d87,
    0x455a14ed,
    0xa9e3e905,
    0xfcefa3f8,
    0x676f02d9,
    0x8d2a4c8a,
    0xfffa3942,
    0x8771f681,
    0x6d9d6122,
    0xfde5380c,
    0xa4beea44,
    0x4bdecfa9,
    0xf6bb4b60,
    0xbebfbc70,
    0x289b7ec6,
    0xeaa127fa,
    0xd4ef3085,
    0x04881d05,
    0xd9d4d039,
    0xe6db99e5,
    0x1fa27cf8,
    0xc4ac5665,
    0xf4292244,
    0x432aff97,
    0xab9423a7,
    0xfc93a039,
    0x655b59c3,
    0x8f0ccc92,
    0xffeff47d,
    0x85845dd1,
    0x6fa87e4f,
    0xfe2ce6e0,
    0xa3014314,
    0x4e0811a1,
    0xf7537e82,
    0xbd3af235,
    0x2ad7d2bb,
    0xeb86d391,
};
constexpr std::array<int, 16> kMd5Shifts{ 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };

// Factors of the public exponent 0xFFFF; p - 1 must avoid them all.
constexpr std::array<std::uint32_t, 4> kExponentFactors{ 3, 5, 17, 257 };
constexpr int                          kMillerRabinRounds  = 40;
constexpr std::uint32_t                kTrialDivisionLimit = 2000;

// Unsigned numbers of up to 512 bits, least significant limb first. The RSA
// numbers are 256 bits, so every product fits.
using Number = std::array<std::uint32_t, 16>;

auto fromLittleEndian(std::span<const std::uint8_t> bytes) -> Number
{
    auto number = Number{};
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        number[i / 4] |= static_cast<std::uint32_t>(bytes[i]) << (8 * (i % 4));
    }

    return number;
}

auto toLittleEndian(const Number& number) -> std::array<std::uint8_t, 32>
{
    auto bytes = std::array<std::uint8_t, 32>{};
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        bytes[i] = static_cast<std::uint8_t>(number[i / 4] >> (8 * (i % 4)));
    }

    return bytes;
}

auto fromBigEndian(std::span<const std::uint8_t, 32> bytes) -> Number
{
    auto reversed = std::array<std::uint8_t, 32>{};
    std::ranges::reverse_copy(bytes, reversed.begin());
    return fromLittleEndian(reversed);
}

auto toBigEndian(const Number& number) -> std::array<std::uint8_t, 32>
{
    auto bytes = toLittleEndian(number);
    std::ranges::reverse(bytes);
    return bytes;
}

auto small(std::uint32_t value) -> Number
{
    auto number = Number{};
    number[0]   = value;
    return number;
}

auto compare(const Number& a, const Number& b) -> int
{
    for (size_t i = a.size(); i-- > 0;)
    {
        if (a[i] != b[i])
        {
            if (a[i] < b[i])
            {
                return -1;
            }

            return 1;
        }
    }

    return 0;
}

// a -= b, for a >= b.
void subtract(Number& a, const Number& b)
{
    auto borrow = std::uint64_t{};
    for (size_t i = 0; i < a.size(); ++i)
    {
        const auto difference = static_cast<std::uint64_t>(a[i]) - b[i] - borrow;
        a[i]                  = static_cast<std::uint32_t>(difference);
        borrow                = (difference >> 32) & 1;
    }
}

void addSmall(Number& a, std::uint32_t value)
{
    auto carry = static_cast<std::uint64_t>(value);
    for (size_t i = 0; i < a.size() && carry; ++i)
    {
        const auto sum = static_cast<std::uint64_t>(a[i]) + carry;
        a[i]           = static_cast<std::uint32_t>(sum);
        carry          = sum >> 32;
    }
}

auto multiplySmall(const Number& a, std::uint32_t value) -> Number
{
    auto product = Number{};
    auto carry   = std::uint64_t{};
    for (size_t i = 0; i < a.size(); ++i)
    {
        const auto partial = static_cast<std::uint64_t>(a[i]) * value + carry;
        product[i]         = static_cast<std::uint32_t>(partial);
        carry              = partial >> 32;
    }

    return product;
}

// Returns the quotient and stores the remainder.
auto divideSmall(const Number& a, std::uint32_t divisor, std::uint32_t& remainder) -> Number
{
    auto quotient = Number{};
    auto rest     = std::uint64_t{};
    for (size_t i = a.size(); i-- > 0;)
    {
        const auto current = (rest << 32) | a[i];
        quotient[i]        = static_cast<std::uint32_t>(current / divisor);
        rest               = current % divisor;
    }

    remainder = static_cast<std::uint32_t>(rest);
    return quotient;
}

auto remainderSmall(const Number& a, std::uint32_t divisor) -> std::uint32_t
{
    auto remainder = std::uint32_t{};
    divideSmall(a, divisor, remainder);
    return remainder;
}

auto bitLength(const Number& a) -> size_t
{
    for (size_t i = a.size(); i-- > 0;)
    {
        if (a[i])
        {
            return 32 * i + std::bit_width(a[i]);
        }
    }

    return 0;
}

auto testBit(const Number& a, size_t bit) -> bool
{
    return (a[bit / 32] >> (bit % 32)) & 1;
}

void shiftLeftOne(Number& a, bool low)
{
    for (size_t i = a.size(); i-- > 1;)
    {
        a[i] = (a[i] << 1) | (a[i - 1] >> 31);
    }

    a[0] = (a[0] << 1) | static_cast<std::uint32_t>(low);
}

void shiftRightOne(Number& a)
{
    for (size_t i = 0; i + 1 < a.size(); ++i)
    {
        a[i] = (a[i] >> 1) | (a[i + 1] << 31);
    }

    a.back() >>= 1;
}

// Full product of two numbers below 2^256.
auto multiply(const Number& a, const Number& b) -> Number
{
    auto product = Number{};
    for (size_t i = 0; i < 8; ++i)
    {
        auto carry = std::uint64_t{};
        for (size_t j = 0; j < 8; ++j)
        {
            const auto partial = static_cast<std::uint64_t>(a[i]) * b[j] + product[i + j] + carry;
            product[i + j]     = static_cast<std::uint32_t>(partial);
            carry              = partial >> 32;
        }

        product[i + 8] = static_cast<std::uint32_t>(carry);
    }

    return product;
}

// value mod modulus, one bit at a time; modulus is below 2^256.
auto reduce(const Number& value, const Number& modulus) -> Number
{
    auto remainder = Number{};
    for (size_t bit = bitLength(value); bit-- > 0;)
    {
        shiftLeftOne(remainder, testBit(value, bit));
        if (compare(remainder, modulus) >= 0)
        {
            subtract(remainder, modulus);
        }
    }

    return remainder;
}

auto multiplyMod(const Number& a, const Number& b, const Number& modulus) -> Number
{
    return reduce(multiply(a, b), modulus);
}

auto powerMod(const Number& base, const Number& exponent, const Number& modulus) -> Number
{
    auto result  = reduce(small(1), modulus);
    auto squared = reduce(base, modulus);
    for (size_t bit = 0, bits = bitLength(exponent); bit < bits; ++bit)
    {
        if (testBit(exponent, bit))
        {
            result = multiplyMod(result, squared, modulus);
        }

        squared = multiplyMod(squared, squared, modulus);
    }

    return result;
}

void randomBytes(std::span<std::uint8_t> bytes)
{
    if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    {
        throw std::runtime_error("Cannot generate random bytes");
    }
}

auto random128() -> Number
{
    auto bytes = std::array<std::uint8_t, 16>{};
    randomBytes(bytes);
    return fromLittleEndian(bytes);
}

auto smallPrimes() -> const std::vector<std::uint32_t>&
{
    static const auto primes = []() -> std::vector<std::uint32_t>
    {
        auto composite = std::vector<bool>(kTrialDivisionLimit, false);
        auto found     = std::vector<std::uint32_t>{};
        for (std::uint32_t i = 2; i < kTrialDivisionLimit; ++i)
        {
            if (!composite[i])
            {
                found.push_back(i);
                for (auto j = i * i; j < kTrialDivisionLimit; j += i)
                {
                    composite[j] = true;
                }
            }
        }

        return found;
    }();
    return primes;
}

// Miller-Rabin with random bases, for an odd candidate above the small primes.
auto probablyPrime(const Number& candidate) -> bool
{
    auto minusOne = candidate;
    subtract(minusOne, small(1));
    auto odd     = minusOne;
    auto squares = 0;
    while (!testBit(odd, 0))
    {
        shiftRightOne(odd);
        ++squares;
    }

    auto baseRange = candidate;
    subtract(baseRange, small(3));
    for (int round = 0; round < kMillerRabinRounds; ++round)
    {
        auto base = reduce(random128(), baseRange);
        addSmall(base, 2);
        auto x = powerMod(base, odd, candidate);
        if (compare(x, small(1)) == 0 || compare(x, minusOne) == 0)
        {
            continue;
        }

        auto witness = true;
        for (int i = 1; i < squares && witness; ++i)
        {
            x       = multiplyMod(x, x, candidate);
            witness = compare(x, minusOne) != 0;
        }

        if (witness)
        {
            return false;
        }
    }

    return true;
}

// A random prime of exactly 128 bits with its top two bits set and gcd(p - 1, 0xFFFF) = 1.
auto randomPrime() -> Number
{
    for (;;)
    {
        auto candidate = random128();
        candidate[3] |= 0xc0000000;
        candidate[0] |= 1;
        const auto usable = std::ranges::none_of(kExponentFactors, [&candidate](std::uint32_t factor) -> bool
                                                 {
                                                     return remainderSmall(candidate, factor) == 1;
                                                 }) &&
                            std::ranges::none_of(smallPrimes(), [&candidate](std::uint32_t prime) -> bool
                                                 {
                                                     return remainderSmall(candidate, prime) == 0;
                                                 });
        if (usable && probablyPrime(candidate))
        {
            return candidate;
        }
    }
}

// The inverse of value modulo the small modulus; they must be coprime.
auto inverseSmall(std::uint32_t value, std::uint32_t modulus) -> std::uint32_t
{
    auto [oldR, r] = std::pair<std::int64_t, std::int64_t>{ value, modulus };
    auto [oldS, s] = std::pair<std::int64_t, std::int64_t>{ 1, 0 };
    while (r != 0)
    {
        const auto quotient = oldR / r;
        oldR                = std::exchange(r, oldR - quotient * r);
        oldS                = std::exchange(s, oldS - quotient * s);
    }

    return static_cast<std::uint32_t>(((oldS % modulus) + modulus) % modulus);
}

auto readBigEndian32(const std::vector<std::uint8_t>& bytes, size_t offset) -> std::uint32_t
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

auto checksum(std::string_view message) -> std::uint32_t
{
    auto       sum   = std::uint32_t{};
    const auto whole = message.size() & ~size_t{ 3 };
    for (size_t i = 0; i < whole; i += 4)
    {
        auto word = std::uint32_t{};
        std::memcpy(&word, message.data() + i, sizeof(word));
        sum += word;
    }

    // A short tail is read little-endian into the high bytes.
    auto tail = std::uint32_t{};
    for (size_t i = whole; i < message.size(); ++i)
    {
        tail |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(message[i])) << (8 * (i - whole));
    }

    if (whole < message.size())
    {
        sum += tail << (8 * (4 - (message.size() - whole)));
    }

    return sum;
}

} // namespace

auto encodeBase64(std::span<const std::uint8_t> data, bool pad) -> std::string
{
    auto text = std::string{};
    for (size_t i = 0; i < data.size(); i += 3)
    {
        const auto length = std::min<size_t>(3, data.size() - i);
        auto       group  = std::uint32_t{};
        for (size_t j = 0; j < 3; ++j)
        {
            group <<= 8;
            if (j < length)
            {
                group |= data[i + j];
            }
        }

        auto count = size_t{ 4 };
        if (!pad && length < 3)
        {
            count = length + 1;
        }

        for (size_t j = 0; j < count; ++j)
        {
            text += kBase64Alphabet[(group >> (18 - 6 * j)) & 63];
        }
    }

    return text;
}

auto decodeBase64(std::string_view text) -> Bytes
{
    auto bytes = Bytes{};
    for (size_t i = 0; i < text.size(); i += 4)
    {
        auto group = std::uint32_t{};
        for (size_t j = 0; j < 4; ++j)
        {
            auto value = size_t{};
            if (i + j < text.size())
            {
                value = kBase64Alphabet.find(text[i + j]);
                if (value == std::string_view::npos)
                {
                    throw std::runtime_error("Invalid character in PlayOnline base64");
                }
            }

            group = (group << 6) | static_cast<std::uint32_t>(value);
        }

        bytes.push_back(static_cast<std::uint8_t>(group >> 16));
        bytes.push_back(static_cast<std::uint8_t>(group >> 8));
        bytes.push_back(static_cast<std::uint8_t>(group));
    }

    return bytes;
}

auto checksumChars(std::string_view message) -> std::string
{
    const auto sum   = checksum(message);
    auto       chars = std::string{};
    for (const auto shift : { 26, 20, 14, 8 })
    {
        chars += static_cast<char>(((sum >> shift) & 0x3f) + 0x3f);
    }

    return chars;
}

auto md5(std::span<const std::uint8_t> data, const Md5State& state) -> std::array<std::uint8_t, 16>
{
    auto message = Bytes(data.begin(), data.end());
    message.push_back(0x80);
    message.resize(message.size() + (55 - data.size() % 64 + 64) % 64);
    const auto bits = static_cast<std::uint64_t>(data.size()) * 8;
    for (int i = 0; i < 8; ++i)
    {
        message.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
    }

    auto digest = state;
    for (size_t offset = 0; offset < message.size(); offset += 64)
    {
        auto words = std::array<std::uint32_t, 16>{};
        std::memcpy(words.data(), message.data() + offset, sizeof(words));
        auto [a, b, c, d] = digest;
        for (std::uint32_t i = 0; i < 64; ++i)
        {
            auto f = std::uint32_t{};
            auto g = std::uint32_t{};
            if (i < 16)
            {
                f = (b & c) | (~b & d);
                g = i;
            }
            else if (i < 32)
            {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            }
            else if (i < 48)
            {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            }
            else
            {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }

            f = f + a + kMd5Constants[i] + words[g];
            a = d;
            d = c;
            c = b;
            b = b + std::rotl(f, kMd5Shifts[(i / 16) * 4 + i % 4]);
        }

        digest = { digest[0] + a, digest[1] + b, digest[2] + c, digest[3] + d };
    }

    auto bytes = std::array<std::uint8_t, 16>{};
    std::memcpy(bytes.data(), digest.data(), bytes.size());
    return bytes;
}

// sqBlowFishInit: the key is stretched to 4 KiB by MD5, which seeds P (read
// with a one-byte stride) and S; both are then rekeyed by encrypting zero.
Blowfish::Blowfish(std::span<const std::uint8_t> key)
{
    auto buffer = Bytes(key.begin(), key.end());
    auto state  = kMd5Iv;
    while (buffer.size() < kScheduleBytes)
    {
        const auto digest = md5(buffer, state);
        state             = {};
        buffer.insert(buffer.end(), digest.begin(), digest.begin() + std::min(digest.size(), kScheduleBytes - buffer.size()));
    }

    for (size_t i = 0; i < p_.size(); ++i)
    {
        p_[i] = readBigEndian32(buffer, i) ^ readBigEndian32(buffer, 4 * i);
    }

    for (size_t box = 0; box < s_.size(); ++box)
    {
        for (size_t j = 0; j < s_[box].size(); ++j)
        {
            s_[box][j] = readBigEndian32(buffer, 4 * (256 * box + j));
        }
    }

    for (size_t i = 0; i < p_.size(); i += 2)
    {
        std::tie(p_[i], p_[i + 1]) = encryptHalves(0, 0);
    }

    for (auto& box : s_)
    {
        for (size_t j = 0; j < box.size(); j += 2)
        {
            std::tie(box[j], box[j + 1]) = encryptHalves(0, 0);
        }
    }
}

auto Blowfish::round(std::uint32_t x) const -> std::uint32_t
{
    return ((s_[0][x >> 24] + s_[1][(x >> 16) & 255]) ^ s_[2][(x >> 8) & 255]) + s_[3][x & 255];
}

auto Blowfish::encryptHalves(std::uint32_t low, std::uint32_t high) const -> std::pair<std::uint32_t, std::uint32_t>
{
    for (size_t i = 0; i < 16; ++i)
    {
        const auto x = low ^ p_[i];
        low          = high ^ round(x);
        high         = x;
    }

    return { high ^ p_[17], low ^ p_[16] };
}

auto Blowfish::encrypt(std::uint64_t block) const -> std::uint64_t
{
    const auto [low, high] = encryptHalves(static_cast<std::uint32_t>(block), static_cast<std::uint32_t>(block >> 32));
    return low | (static_cast<std::uint64_t>(high) << 32);
}

auto crypt64(const Blowfish& cipher, std::uint64_t iv, std::string_view data) -> std::string
{
    auto output = std::string(data);
    auto state  = iv;
    for (size_t i = 0; i < output.size(); ++i)
    {
        if (i % 8 == 0)
        {
            state = cipher.encrypt(state);
        }

        const auto plain     = static_cast<std::uint8_t>(data[i]);
        const auto encrypted = static_cast<std::uint8_t>(plain ^ (state >> (8 * (i % 8))));
        if (plain != '\r' && plain != '\n' && encrypted != '\r' && encrypted != '\n')
        {
            output[i] = static_cast<char>(encrypted);
        }
    }

    return output;
}

auto frameLine(std::string_view message, const Blowfish* cipher, std::uint64_t iv) -> std::string
{
    const auto line = std::string(message) + checksumChars(message) + "\r\n";
    if (cipher)
    {
        return crypt64(*cipher, iv, line);
    }

    return line;
}

auto unframeLine(std::string_view line, const Blowfish* cipher, std::uint64_t iv) -> std::string
{
    auto plain = std::string(line);
    if (cipher)
    {
        plain = crypt64(*cipher, iv, line);
    }

    if (plain.size() < 6 || !plain.ends_with("\r\n"))
    {
        throw std::runtime_error("Malformed line from the server");
    }

    const auto message = plain.substr(0, plain.size() - 6);
    if (checksumChars(message) != plain.substr(plain.size() - 6, 4))
    {
        throw std::runtime_error("Bad checksum on a line from the server");
    }

    return message;
}

auto generateRsaKey() -> RsaKey
{
    const auto p = randomPrime();
    auto       q = randomPrime();
    while (compare(p, q) == 0)
    {
        q = randomPrime();
    }

    auto pMinusOne = p;
    auto qMinusOne = q;
    subtract(pMinusOne, small(1));
    subtract(qMinusOne, small(1));
    const auto totient = multiply(pMinusOne, qMinusOne);
    // d = (1 + k * totient) / e, with k chosen so that e divides the numerator.
    const auto k         = (kRsaExponent - inverseSmall(remainderSmall(totient, kRsaExponent), kRsaExponent)) % kRsaExponent;
    auto       numerator = multiplySmall(totient, k);
    addSmall(numerator, 1);
    auto       remainder = std::uint32_t{};
    const auto exponent  = divideSmall(numerator, kRsaExponent, remainder);
    return { toLittleEndian(multiply(p, q)), toLittleEndian(exponent) };
}

auto rsaDecrypt(const RsaKey& key, std::span<const std::uint8_t, 32> block) -> std::array<std::uint8_t, 32>
{
    const auto modulus = fromLittleEndian(key.modulus);
    const auto value   = fromBigEndian(block);
    if (compare(value, modulus) >= 0)
    {
        throw std::runtime_error("RSA block is not below the modulus");
    }

    return toBigEndian(powerMod(value, fromLittleEndian(key.privateExponent), modulus));
}

auto rsaEncrypt(std::span<const std::uint8_t, 32> modulus, std::span<const std::uint8_t, 32> block) -> std::array<std::uint8_t, 32>
{
    const auto number = fromLittleEndian(modulus);
    const auto value  = fromBigEndian(block);
    if (compare(value, number) >= 0)
    {
        throw std::runtime_error("RSA block is not below the modulus");
    }

    return toBigEndian(powerMod(value, small(kRsaExponent), number));
}

auto sessionKey(std::span<const std::uint8_t, 32> block) -> std::array<std::uint8_t, 8>
{
    if (block[0] != 0 || block[1] != 2)
    {
        throw std::runtime_error("The server's key exchange did not decrypt");
    }

    const auto separator = std::find(block.begin() + 2, block.end(), 0);
    if (separator == block.end() || block.end() - separator != 9)
    {
        throw std::runtime_error("The server's key exchange did not carry an 8-byte key");
    }

    auto key = std::array<std::uint8_t, 8>{};
    std::reverse_copy(separator + 1, block.end(), key.begin());
    return key;
}

} // namespace tmloader::wire
