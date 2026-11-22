// Edge cases of f80, with the expected values taken from an x87 (f80_hostdiff checks the same at random)
#include "f80/f80.h"

#include <catch2/catch_test_macros.hpp>

using namespace tmweb::f80;

namespace
{

constexpr std::uint16_t kSingle24 = 0x007F; // what Direct3D 8 leaves set: 24 bits, nearest
constexpr std::uint16_t kDefault  = 0x037F; // FNINIT: 64 bits, nearest

auto value(const std::uint16_t signExponent, const std::uint64_t significand) -> F80
{
    return F80{ significand, signExponent };
}

auto control(const Precision precision, const Rounding rounding) -> std::uint16_t
{
    return static_cast<std::uint16_t>(0x007F | (precision << 8) | (rounding << 10));
}

auto same(const F80 a, const F80 b) -> bool
{
    return a.significand == b.significand && a.signExponent == b.signExponent;
}

const auto kOne        = value(0x3FFF, 0x8000000000000000ULL);
const auto kTwo        = value(0x4000, 0x8000000000000000ULL);
const auto kThree      = value(0x4000, 0xC000000000000000ULL);
const auto kHalf       = value(0x3FFE, 0x8000000000000000ULL);
const auto kMax        = value(0x7FFE, 0xFFFFFFFFFFFFFFFFULL);
const auto kMinNormal  = value(0x0001, 0x8000000000000000ULL);
const auto kTiny       = value(0x0000, 0x0000000000000001ULL);
const auto kPseudoDen  = value(0x0000, 0x8000000000000001ULL);
const auto kUnnormal   = value(0x3FFF, 0x4000000000000000ULL);
const auto kPseudoInf  = value(0x7FFF, 0x0000000000000000ULL);
const auto kQuietNaN   = value(0x7FFF, 0xC000000000001234ULL);
const auto kSignaling  = value(0x7FFF, 0x8000000000005678ULL);
const auto kSignaling2 = value(0x7FFF, 0x8000000000009999ULL);
const auto kInfinity   = value(0x7FFF, 0x8000000000000000ULL);

} // namespace

TEST_CASE("f80 rounds sums to the precision control")
{
    // 1 + 2^-24 is a tie at 24 bits; 1 + 3 * 2^-25 is not
    const auto tie   = value(0x3FFF - 24, 0x8000000000000000ULL);
    const auto above = value(0x3FFF - 24, 0xC000000000000000ULL);
    auto       env   = envFromControl(kSingle24);
    CHECK(same(add(kOne, tie, env), kOne));
    CHECK(env.flags == kInexact);
    env = envFromControl(kSingle24);
    CHECK(same(add(kOne, above, env), value(0x3FFF, 0x8000010000000000ULL)));
    CHECK(env.flags == (kInexact | kRoundedUp));
    env = envFromControl(control(kSingle, kUp));
    CHECK(same(add(kOne, tie, env), value(0x3FFF, 0x8000010000000000ULL)));
    env = envFromControl(control(kDouble, kNearest));
    CHECK(same(add(kOne, tie, env), add(kOne, tie, env)));
    CHECK(same(add(kOne, tie, env), value(0x3FFF, 0x8000008000000000ULL)));
    env = envFromControl(kDefault);
    CHECK(same(add(kOne, kOne, env), kTwo));
    CHECK(env.flags == 0);
}

TEST_CASE("f80 overflow and underflow follow the rounding mode")
{
    auto env = envFromControl(kDefault);
    CHECK(same(mul(kMax, kTwo, env), kInfinity));
    CHECK(env.flags == (kOverflow | kInexact | kRoundedUp));
    env = envFromControl(control(kExtended, kZero));
    CHECK(same(mul(kMax, kTwo, env), kMax));
    CHECK(env.flags == (kOverflow | kInexact));
    // An exact denormal result does not signal underflow when it is masked; an inexact one does
    env = envFromControl(kDefault);
    CHECK(same(div(kMinNormal, kTwo, env), value(0, 0x4000000000000000ULL)));
    CHECK(env.flags == 0);
    env = envFromControl(kDefault);
    static_cast<void>(div(kMinNormal, kThree, env));
    CHECK(env.flags == (kUnderflow | kInexact | kRoundedUp));
}

TEST_CASE("f80 invalid operations give the indefinite")
{
    auto env = envFromControl(kDefault);
    CHECK(same(sub(kInfinity, kInfinity, env), indefinite()));
    CHECK(env.flags == kInvalid);
    env = envFromControl(kDefault);
    CHECK(same(div(zero(), zero(), env), indefinite()));
    env = envFromControl(kDefault);
    CHECK(same(sqrt(negate(kOne), env), indefinite()));
    env = envFromControl(kDefault);
    CHECK(same(sqrt(zero(true), env), zero(true)));
    CHECK(env.flags == 0);
    env = envFromControl(kDefault);
    CHECK(same(div(negate(kOne), zero(), env), infinity(true)));
    CHECK(env.flags == kZeroDiv);
    // The 387 and later reject unnormals and pseudo-infinities
    env = envFromControl(kDefault);
    CHECK(same(add(kUnnormal, kOne, env), indefinite()));
    CHECK(env.flags == kInvalid);
    env = envFromControl(kDefault);
    CHECK(same(mul(kPseudoInf, kOne, env), indefinite()));
    CHECK(classify(kUnnormal) == Class::Unsupported);
}

TEST_CASE("f80 propagates NaNs as the x87 does")
{
    auto env = envFromControl(kDefault);
    CHECK(same(add(kQuietNaN, kOne, env), kQuietNaN));
    CHECK(env.flags == 0);
    // A signaling NaN is quieted and raises invalid; a quiet NaN beats it
    env = envFromControl(kDefault);
    CHECK(same(add(kSignaling, kQuietNaN, env), kQuietNaN));
    CHECK(env.flags == kInvalid);
    env = envFromControl(kDefault);
    CHECK(same(mul(kOne, kSignaling, env), value(0x7FFF, 0xC000000000005678ULL)));
    // Two signaling NaNs: the larger significand
    env = envFromControl(kDefault);
    CHECK(same(sub(kSignaling, kSignaling2, env), value(0x7FFF, 0xC000000000009999ULL)));
    // A NaN operand hides the denormal one
    env = envFromControl(kDefault);
    static_cast<void>(add(kQuietNaN, kTiny, env));
    CHECK(env.flags == 0);
}

TEST_CASE("f80 flags denormal operands")
{
    auto env = envFromControl(kDefault);
    CHECK(same(add(kTiny, zero(), env), kTiny));
    CHECK(env.flags == kDenormal);
    // A pseudo-denormal means the same as exponent 1
    env = envFromControl(kDefault);
    CHECK(same(add(kPseudoDen, zero(), env), value(1, 0x8000000000000001ULL)));
    CHECK(env.flags == kDenormal);
    CHECK(classify(kPseudoDen) == Class::Denormal);
    env = envFromControl(kDefault);
    static_cast<void>(fromFloat(0x00000001, env));
    CHECK(env.flags == kDenormal);
    env = envFromControl(kDefault);
    CHECK(same(fromDouble(0x7FF0000000000001ULL, env), value(0x7FFF, 0xC000000000000800ULL)));
    CHECK(env.flags == kInvalid);
}

TEST_CASE("f80 converts memory operands inside the arithmetic")
{
    // FADD m32 with two signaling NaNs: ST0's significand 8B1A... beats the operand's 80E9DA... (quieted it would be
    // C0E9DA... and win)
    const auto st0      = value(0xFFFF, 0x8B1A85D4C1FBF800ULL);
    bool       denormal = true;
    const auto m        = floatOperand(0x7F80E9DA, denormal);
    CHECK_FALSE(denormal);
    CHECK(same(m, value(0x7FFF, 0x80E9DA0000000000ULL)));
    auto env = envFromControl(kDefault);
    CHECK(same(add(st0, m, env), value(0xFFFF, 0xCB1A85D4C1FBF800ULL)));
    CHECK(env.flags == kInvalid);
    // A denormal m32 under a NaN ST0 raises nothing; under a number it raises kDenormal
    const auto tiny = floatOperand(0x00200A1B, denormal);
    CHECK(denormal);
    env = envFromControl(kDefault);
    CHECK(same(add(kQuietNaN, tiny, env), kQuietNaN));
    denormalOperand(kQuietNaN, tiny, env);
    CHECK(env.flags == 0);
    env = envFromControl(kDefault);
    static_cast<void>(add(kOne, tiny, env));
    denormalOperand(kOne, tiny, env);
    CHECK(env.flags == (kDenormal | kInexact));
    // FMUL m64 with an SNaN operand keeps its payload bits
    CHECK(same(doubleOperand(0x7FF000000000EFE7ULL, denormal), value(0x7FFF, 0x80000000077F3800ULL)));
    CHECK_FALSE(denormal);
}

TEST_CASE("f80 stores round by the rounding mode, not the precision")
{
    const auto oneAndTie = value(0x3FFF, 0x8000008000000000ULL); // 1 + 2^-24
    auto       env       = envFromControl(kDefault);
    CHECK(toFloat(oneAndTie, env) == 0x3F800000);
    CHECK(env.flags == kInexact);
    env = envFromControl(control(kExtended, kUp));
    CHECK(toFloat(oneAndTie, env) == 0x3F800001);
    CHECK(env.flags == (kInexact | kRoundedUp));
    env = envFromControl(kDefault);
    CHECK(toDouble(kSignaling, env) == 0x7FF800000000000AULL); // the payload's top bits, quieted
    CHECK(env.flags == kInvalid);
    env = envFromControl(kSingle24);
    CHECK(toDouble(value(0x3FFE, 0xFFFFFFFFFFFFF800ULL), env) == 0x3FEFFFFFFFFFFFFFULL);
}

TEST_CASE("f80 integer stores round and saturate to the indefinite")
{
    const auto twoAndHalf = value(0x4000, 0xA000000000000000ULL);
    auto       env        = envFromControl(kDefault);
    CHECK(toInt32(twoAndHalf, env) == 2);
    CHECK(env.flags == kInexact);
    env = envFromControl(kDefault);
    CHECK(toInt32(value(0x4000, 0xE000000000000000ULL), env) == 4); // 3.5
    CHECK(env.flags == (kInexact | kRoundedUp));
    env = envFromControl(control(kExtended, kDown));
    CHECK(toInt32(negate(twoAndHalf), env) == -3);
    env = envFromControl(kDefault);
    CHECK(toInt32(value(0x401E, 0x8000000000000000ULL), env) == INT32_MIN); // 2^31
    CHECK(env.flags == kInvalid);
    env = envFromControl(kDefault);
    CHECK(toInt16(value(0x400D, 0xFFFF000000000000ULL), env) == INT16_MIN); // 32767.5 rounds to 32768
    CHECK(env.flags == kInvalid);
    env = envFromControl(kDefault);
    CHECK(toInt64(kQuietNaN, env) == INT64_MIN);
    CHECK(env.flags == kInvalid);
    CHECK(same(fromInt64(INT64_MIN), value(0xC03E, 0x8000000000000000ULL)));
}

TEST_CASE("f80 compares and classifies")
{
    auto env = envFromControl(kDefault);
    CHECK(compare(zero(true), zero(), false, env) == Order::Equal);
    CHECK(compare(negate(kOne), kHalf, false, env) == Order::Less);
    CHECK(compare(kTiny, zero(), false, env) == Order::Greater);
    CHECK(env.flags == kDenormal);
    env = envFromControl(kDefault);
    CHECK(compare(kQuietNaN, kOne, true, env) == Order::Unordered);
    CHECK(env.flags == 0);
    CHECK(compare(kQuietNaN, kOne, false, env) == Order::Unordered);
    CHECK(env.flags == kInvalid);
    CHECK(classify(zero(true)) == Class::Zero);
    CHECK(classify(kInfinity) == Class::Infinity);
    CHECK(classify(kSignaling) == Class::NaN);
    CHECK(classify(kOne) == Class::Normal);
    CHECK(classify(kPseudoInf) == Class::Unsupported);
}

TEST_CASE("f80 load-constant instructions round by the rounding mode")
{
    auto env = envFromControl(kDefault);
    CHECK(same(constant(3, env), value(0x4000, 0xC90FDAA22168C235ULL)));
    CHECK(same(constant(3, env), pi()));
    env = envFromControl(control(kExtended, kDown));
    CHECK(same(constant(3, env), value(0x4000, 0xC90FDAA22168C234ULL)));
    env = envFromControl(control(kExtended, kUp));
    CHECK(same(constant(1, env), value(0x4000, 0xD49A784BCD1B8AFFULL)));
    CHECK(same(constant(0, env), kOne));
    CHECK(same(constant(6, env), zero()));
    CHECK(env.flags == 0);
}

TEST_CASE("f80 rounds to integer, scales and extracts")
{
    auto env = envFromControl(kDefault);
    CHECK(same(roundToInt(value(0x4000, 0xA000000000000000ULL), env), kTwo)); // 2.5
    env = envFromControl(control(kExtended, kUp));
    CHECK(same(roundToInt(negate(kHalf), env), zero(true)));
    env = envFromControl(kDefault);
    CHECK(same(scale(kThree, value(0xC000, 0xB99999999999999AULL), env), value(0x3FFE, 0xC000000000000000ULL))); // 3 * 2^trunc(-2.9)
    env = envFromControl(kDefault);
    CHECK(same(scale(zero(), kInfinity, env), indefinite()));
    CHECK(env.flags == kInvalid);
    env = envFromControl(kDefault);
    CHECK(same(scale(kOne, value(0x400F, 0x8000000000000000ULL), env), kInfinity)); // 2^65536
    CHECK(env.flags == (kOverflow | kInexact | kRoundedUp));
    env           = envFromControl(kDefault);
    F80  fraction = {};
    auto exponent = extract(value(0x4002, 0xC000000000000000ULL), fraction, env); // 12
    CHECK(same(exponent, fromInt64(3)));
    CHECK(same(fraction, value(0x3FFF, 0xC000000000000000ULL)));
    exponent = extract(zero(true), fraction, env);
    CHECK(same(exponent, infinity(true)));
    CHECK(same(fraction, zero(true)));
    CHECK(env.flags == kZeroDiv);
    exponent = extract(kTiny, fraction, env);
    CHECK(same(exponent, fromInt64(-16445)));
}

TEST_CASE("f80 partial remainders set the quotient bits")
{
    constexpr std::uint16_t kC0   = 0x0100;
    constexpr std::uint16_t kC1   = 0x0200;
    constexpr std::uint16_t kC2   = 0x0400;
    constexpr std::uint16_t kC3   = 0x4000;
    const auto              seven = fromInt64(7);
    auto                    env   = envFromControl(kDefault);
    std::uint16_t           codes = 0;
    CHECK(same(partialRemainder(seven, kTwo, false, env, codes), kOne)); // quotient 3
    CHECK(codes == (kC3 | kC1));
    CHECK(same(partialRemainder(seven, kTwo, true, env, codes), negate(kOne))); // quotient 4
    CHECK(codes == kC0);
    // 2^100 mod 3 takes more than one FPREM: C2 says to run it again
    const auto big = value(0x3FFF + 100, 0x8000000000000000ULL);
    auto       r   = partialRemainder(big, kThree, false, env, codes);
    CHECK(codes == kC2);
    r = partialRemainder(r, kThree, false, env, codes);
    CHECK(same(r, kOne));
    CHECK((codes & kC2) == 0);
    CHECK(env.flags == 0);
    CHECK(same(partialRemainder(kOne, zero(), false, env, codes), indefinite()));
}

TEST_CASE("f80 transcendentals")
{
    auto env = envFromControl(kDefault);
    F80  r   = {};
    // FLDPI; FSIN gives -2^-64 because the x87 reduces by its 66-bit pi
    CHECK(sin(pi(), r, env));
    CHECK(same(r, value(0xBFBF, 0x8000000000000000ULL)));
    CHECK(env.flags == (kInexact | kRoundedUp));
    CHECK_FALSE(sin(value(0x403E, 0x8000000000000000ULL), r, env)); // 2^63: out of range, C2
    CHECK(cos(zero(), r, env));
    CHECK(same(r, kOne));
    env = envFromControl(kDefault);
    CHECK(same(atan2(kOne, kOne, env), value(0x3FFE, 0xC90FDAA22168C235ULL)));
    CHECK(same(atan2(zero(), negate(kOne), env), pi()));
    CHECK(same(exp2m1(negate(kOne), env), negate(kHalf)));
    env = envFromControl(kDefault);
    CHECK(same(yLog2X(fromInt64(8), kOne, env), kThree));
    CHECK(env.flags == 0);
    CHECK(same(yLog2X(zero(), kOne, env), infinity(true)));
    CHECK(env.flags == kZeroDiv);
    env = envFromControl(kDefault);
    CHECK(same(yLog2X(negate(kOne), kOne, env), indefinite()));
    CHECK(env.flags == kInvalid);
    env = envFromControl(kDefault);
    CHECK(same(yLog2Xp1(zero(true), kOne, env), zero(true)));
    CHECK(env.flags == 0);
}
