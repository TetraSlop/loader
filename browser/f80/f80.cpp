// The x87's arithmetic on Berkeley SoftFloat 3e (third_party/softfloat). SoftFloat gives the IEEE results; this
// file adds what the x87 does around them: unsupported encodings, the denormal-operand flag, C1, FPREM's partial
// reduction, FSCALE, FXTRACT and the load-constant rounding. The transcendentals are in f80_transcendental.cpp.
#include "f80/f80_internal.h"

#include <iterator>

namespace tmweb::f80
{
namespace detail
{

auto softFlags(const std::uint_fast8_t soft) -> std::uint16_t
{
    std::uint16_t flags = 0;
    flags |= (soft & softfloat_flag_invalid) ? kInvalid : 0;
    flags |= (soft & softfloat_flag_infinite) ? kZeroDiv : 0;
    flags |= (soft & softfloat_flag_overflow) ? kOverflow : 0;
    flags |= (soft & softfloat_flag_underflow) ? kUnderflow : 0;
    flags |= (soft & softfloat_flag_inexact) ? kInexact : 0;
    return flags;
}

auto invalid(Env& env) -> F80
{
    env.flags |= kInvalid;
    return indefinite();
}

auto propagate(const F80 a, const F80 b, Env& env) -> F80
{
    softfloat_exceptionFlags = 0;
    const auto z             = softfloat_propagateNaNExtF80UI(a.signExponent, a.significand, b.signExponent, b.significand);
    env.flags |= softFlags(softfloat_exceptionFlags);
    return F80{ z.v0, static_cast<std::uint16_t>(z.v64) };
}

auto normalized(const F80 a) -> Normal
{
    if (exponentOf(a) != 0)
    {
        return Normal{ exponentOf(a), a.significand };
    }
    const auto shift = softfloat_countLeadingZeros64(a.significand);
    return Normal{ 1 - shift, a.significand << shift };
}

auto pack(const bool negative, const std::int32_t exponent, const std::uint64_t significand, Env& env) -> F80
{
    return fromSoft(rounded(env, 80, [&]
                            {
                                return softfloat_roundPackToExtF80(negative, exponent, significand, 0, 80);
                            }));
}

} // namespace detail

using namespace detail;

namespace
{

constexpr std::uint16_t kC0 = 0x0100;
constexpr std::uint16_t kC1 = 0x0200;
constexpr std::uint16_t kC2 = 0x0400;
constexpr std::uint16_t kC3 = 0x4000;

// FPREM's partial step when the exponents differ by 64 or more: the x87 leaves an exponent difference between 32
// and 63 for the next step.
constexpr int kPartialBits = 32;

auto precisionBits(const Env& env) -> std::uint8_t
{
    switch (env.precision)
    {
        case kSingle:
            return 32;
        case kDouble:
            return 64;
        default:
            return 80;
    }
}

auto arithmetic(const F80 a, const F80 b, Env& env, extFloat80_t (*op)(extFloat80_t, extFloat80_t)) -> F80
{
    if (isUnsupported(a) || isUnsupported(b))
    {
        return invalid(env);
    }
    auto local  = Env{ env.precision, env.rounding, 0 };
    auto result = rounded(local, precisionBits(env), [&]
                          {
                              return op(toSoft(a), toSoft(b));
                          });
    env.flags |= local.flags | denormalFlag(local.flags, a, b);
    return fromSoft(result);
}

// An exact remainder: significand times 2^(exponent - bias - 63), normalized here
auto packExact(const bool negative, const std::int32_t exponent, const std::uint64_t significand, Env& env) -> F80
{
    const auto shift = softfloat_countLeadingZeros64(significand);
    return pack(negative, exponent - shift, significand << shift, env);
}

auto magnitudeLess(const F80 a, const F80 b) -> bool
{
    const auto na = normalized(a);
    const auto nb = normalized(b);
    if (isZero(a) || isZero(b))
    {
        return isZero(a) && !isZero(b);
    }
    return na.exponent != nb.exponent ? na.exponent < nb.exponent : na.significand < nb.significand;
}

} // namespace

auto envFromControl(const std::uint16_t controlWord) -> Env
{
    return Env{ static_cast<Precision>((controlWord >> 8) & 3), static_cast<Rounding>((controlWord >> 10) & 3), 0 };
}

auto zero(const bool negative) -> F80
{
    return F80{ 0, static_cast<std::uint16_t>(negative ? 0x8000 : 0) };
}

auto one() -> F80
{
    return F80{ kIntegerBit, 0x3FFF };
}

auto infinity(const bool negative) -> F80
{
    return F80{ kIntegerBit, static_cast<std::uint16_t>(negative ? 0xFFFF : 0x7FFF) };
}

auto indefinite() -> F80
{
    return F80{ 0xC000000000000000ULL, 0xFFFF };
}

auto pi() -> F80
{
    return F80{ 0xC90FDAA22168C235ULL, 0x4000 };
}

auto constant(const std::uint8_t which, Env& env) -> F80
{
    // The top 64 bits of each constant's significand and whether the bits below are above half. None is exact, so
    // rounding up adds one and rounding down or toward zero truncates.
    struct Constant
    {
        std::uint16_t signExponent;
        std::uint64_t significand;
        bool          nearestUp;
    };

    static constexpr Constant kConstants[] = {
        { 0x3FFF, 0x8000000000000000ULL, false }, // 1
        { 0x4000, 0xD49A784BCD1B8AFEULL, false }, // log2(10)
        { 0x3FFF, 0xB8AA3B295C17F0BBULL, true },  // log2(e)
        { 0x4000, 0xC90FDAA22168C234ULL, true },  // pi
        { 0x3FFD, 0x9A209A84FBCFF798ULL, true },  // log10(2)
        { 0x3FFE, 0xB17217F7D1CF79ABULL, true },  // ln(2)
    };
    if (which == 0)
    {
        return one();
    }
    if (which >= std::size(kConstants))
    {
        return zero();
    }
    const auto& c  = kConstants[which];
    const bool  up = env.rounding == kUp || (env.rounding == kNearest && c.nearestUp);
    return F80{ c.significand + (up ? 1 : 0), c.signExponent };
}

auto fromInt64(const std::int64_t value) -> F80
{
    return fromSoft(i64_to_extF80(value));
}

auto fromFloat(const std::uint32_t bits, Env& env) -> F80
{
    if ((bits & 0x7F800000) == 0 && (bits & 0x007FFFFF) != 0)
    {
        env.flags |= kDenormal;
    }
    softfloat_exceptionFlags = 0;
    const auto result        = f32_to_extF80(float32_t{ bits });
    env.flags |= softFlags(softfloat_exceptionFlags);
    return fromSoft(result);
}

auto fromDouble(const std::uint64_t bits, Env& env) -> F80
{
    if ((bits & 0x7FF0000000000000ULL) == 0 && (bits & 0x000FFFFFFFFFFFFFULL) != 0)
    {
        env.flags |= kDenormal;
    }
    softfloat_exceptionFlags = 0;
    const auto result        = f64_to_extF80(float64_t{ bits });
    env.flags |= softFlags(softfloat_exceptionFlags);
    return fromSoft(result);
}

auto floatOperand(const std::uint32_t bits, bool& denormal) -> F80
{
    denormal = (bits & 0x7F800000) == 0 && (bits & 0x007FFFFF) != 0;
    if ((bits & 0x7F800000) == 0x7F800000 && (bits & 0x007FFFFF) != 0)
    {
        return F80{ kIntegerBit | (static_cast<std::uint64_t>(bits & 0x007FFFFF) << 40),
                    static_cast<std::uint16_t>((bits >> 16 & 0x8000) | 0x7FFF) };
    }
    return fromSoft(f32_to_extF80(float32_t{ bits }));
}

auto doubleOperand(const std::uint64_t bits, bool& denormal) -> F80
{
    denormal = (bits & 0x7FF0000000000000ULL) == 0 && (bits & 0x000FFFFFFFFFFFFFULL) != 0;
    if ((bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL && (bits & 0x000FFFFFFFFFFFFFULL) != 0)
    {
        return F80{ kIntegerBit | ((bits & 0x000FFFFFFFFFFFFFULL) << 11),
                    static_cast<std::uint16_t>((bits >> 48 & 0x8000) | 0x7FFF) };
    }
    return fromSoft(f64_to_extF80(float64_t{ bits }));
}

void denormalOperand(const F80 a, const F80 operand, Env& env)
{
    if (!(env.flags & (kInvalid | kZeroDiv)) && !isNaN(a) && !isNaN(operand))
    {
        env.flags |= kDenormal;
    }
}

auto toFloat(const F80 value, Env& env) -> std::uint32_t
{
    if (isUnsupported(value))
    {
        env.flags |= kInvalid;
        return 0xFFC00000;
    }
    return rounded(env, 80, [&]
                   {
                       return extF80_to_f32(toSoft(value));
                   })
        .v;
}

auto toDouble(const F80 value, Env& env) -> std::uint64_t
{
    if (isUnsupported(value))
    {
        env.flags |= kInvalid;
        return 0xFFF8000000000000ULL;
    }
    return rounded(env, 80, [&]
                   {
                       return extF80_to_f64(toSoft(value));
                   })
        .v;
}

auto toInt16(const F80 value, Env& env) -> std::int16_t
{
    auto       local  = Env{ env.precision, env.rounding, 0 };
    const auto result = toInt32(value, local);
    if (result < INT16_MIN || result > INT16_MAX)
    {
        env.flags |= kInvalid;
        return INT16_MIN;
    }
    env.flags |= local.flags;
    return static_cast<std::int16_t>(result);
}

auto toInt32(const F80 value, Env& env) -> std::int32_t
{
    if (isUnsupported(value))
    {
        env.flags |= kInvalid;
        return INT32_MIN;
    }
    return static_cast<std::int32_t>(
        rounded(env, 80, [&]
                {
                    return extF80_to_i32(toSoft(value), softfloat_roundingMode, true);
                }));
}

auto toInt64(const F80 value, Env& env) -> std::int64_t
{
    if (isUnsupported(value))
    {
        env.flags |= kInvalid;
        return INT64_MIN;
    }
    return static_cast<std::int64_t>(
        rounded(env, 80, [&]
                {
                    return extF80_to_i64(toSoft(value), softfloat_roundingMode, true);
                }));
}

auto add(const F80 a, const F80 b, Env& env) -> F80
{
    return arithmetic(a, b, env, extF80_add);
}

auto sub(const F80 a, const F80 b, Env& env) -> F80
{
    return arithmetic(a, b, env, extF80_sub);
}

auto mul(const F80 a, const F80 b, Env& env) -> F80
{
    return arithmetic(a, b, env, extF80_mul);
}

auto div(const F80 a, const F80 b, Env& env) -> F80
{
    return arithmetic(a, b, env, extF80_div);
}

auto sqrt(const F80 a, Env& env) -> F80
{
    return arithmetic(a, a, env, [](const extFloat80_t x, extFloat80_t)
                      {
                          return extF80_sqrt(x);
                      });
}

auto roundToInt(const F80 a, Env& env) -> F80
{
    return arithmetic(a, a, env, [](const extFloat80_t x, extFloat80_t)
                      {
                          return extF80_roundToInt(x, softfloat_roundingMode, true);
                      });
}

auto scale(const F80 a, const F80 b, Env& env) -> F80
{
    if (isUnsupported(a) || isUnsupported(b))
    {
        return invalid(env);
    }
    if (isNaN(a) || isNaN(b))
    {
        return propagate(a, b, env);
    }
    if (isInf(b))
    {
        // 0 * 2^+inf and inf * 2^-inf have no value
        if (isNegative(b) ? isInf(a) : isZero(a))
        {
            return invalid(env);
        }
        env.flags |= denormalFlag(0, a, b);
        if (isZero(a) || isInf(a))
        {
            return a;
        }
        return isNegative(b) ? zero(isNegative(a)) : infinity(isNegative(a));
    }
    env.flags |= denormalFlag(0, a, b);
    if (isZero(a) || isInf(a))
    {
        return a;
    }
    // trunc(b), clamped: beyond 2^17 every finite a overflows or underflows anyway
    std::int32_t n  = 0;
    const auto   nb = normalized(b);
    if (nb.exponent >= 0x3FFF + 17)
    {
        n = 1 << 17;
    }
    else if (nb.exponent >= 0x3FFF)
    {
        n = static_cast<std::int32_t>(nb.significand >> (63 - (nb.exponent - 0x3FFF)));
    }
    const auto na = normalized(a);
    return pack(isNegative(a), na.exponent + (isNegative(b) ? -n : n), na.significand, env);
}

auto partialRemainder(const F80 a, const F80 b, const bool ieee, Env& env, std::uint16_t& conditionCodes) -> F80
{
    conditionCodes = 0;
    if (isUnsupported(a) || isUnsupported(b))
    {
        return invalid(env);
    }
    if (isNaN(a) || isNaN(b))
    {
        return propagate(a, b, env);
    }
    if (isInf(a) || isZero(b))
    {
        return invalid(env);
    }
    env.flags |= denormalFlag(0, a, b);
    if (isZero(a) || isInf(b))
    {
        return fromSoft(toSoft(a));
    }
    const auto na       = normalized(a);
    auto       nb       = normalized(b);
    auto       negative = isNegative(a);
    auto       d        = na.exponent - nb.exponent;
    if (d < 0)
    {
        // |a| < |b|: the quotient is 0, or 1 for FPREM1 when |a| > |b| / 2
        if (!ieee || d < -1 || na.significand <= nb.significand)
        {
            return fromSoft(toSoft(a));
        }
        conditionCodes = kC1;
        return packExact(!negative, na.exponent, 2 * nb.significand - na.significand, env);
    }
    const bool partial = d >= 64;
    if (partial)
    {
        // Reduce by 2^(d - n) times b, leaving an exponent difference of n for the next FPREM
        const auto n = kPartialBits + (d - kPartialBits) % 32;
        nb.exponent += d - n;
        d = n;
    }
    // Long division of a's significand shifted left by d by b's: q gets the quotient's low bits, r stays below b
    std::uint64_t q = 0;
    std::uint64_t r = na.significand;
    if (r >= nb.significand)
    {
        r -= nb.significand;
        q = 1;
    }
    for (int i = 0; i < d; ++i)
    {
        const bool carry = (r >> 63) != 0; // r is 65 bits wide here; the wrapped subtraction is still exact
        r <<= 1;
        q <<= 1;
        if (carry || r >= nb.significand)
        {
            r -= nb.significand;
            q |= 1;
        }
    }
    if (ieee && !partial && (r > nb.significand - r || (r == nb.significand - r && (q & 1))))
    {
        ++q;
        r        = nb.significand - r;
        negative = !negative;
    }
    conditionCodes = partial ? kC2 : static_cast<std::uint16_t>(((q & 4) ? kC0 : 0) | ((q & 2) ? kC3 : 0) | ((q & 1) ? kC1 : 0));
    if (r == 0)
    {
        return zero(isNegative(a));
    }
    return packExact(negative, nb.exponent, r, env);
}

auto extract(const F80 a, F80& significand, Env& env) -> F80
{
    if (isUnsupported(a))
    {
        significand = indefinite();
        return invalid(env);
    }
    if (isNaN(a))
    {
        significand = propagate(a, a, env);
        return significand;
    }
    if (isZero(a))
    {
        env.flags |= kZeroDiv;
        significand = a;
        return infinity(true);
    }
    if (isInf(a))
    {
        significand = a;
        return infinity(false);
    }
    env.flags |= denormalFlag(0, a, a);
    const auto n = normalized(a);
    significand  = F80{ n.significand, static_cast<std::uint16_t>((a.signExponent & 0x8000) | 0x3FFF) };
    return fromInt64(n.exponent - 0x3FFF);
}

auto negate(F80 a) -> F80
{
    a.signExponent ^= 0x8000;
    return a;
}

auto abs(F80 a) -> F80
{
    a.signExponent &= 0x7FFF;
    return a;
}

auto compare(const F80 a, const F80 b, const bool quiet, Env& env) -> Order
{
    if (isUnsupported(a) || isUnsupported(b))
    {
        env.flags |= kInvalid;
        return Order::Unordered;
    }
    if (isNaN(a) || isNaN(b))
    {
        if (!quiet || isSignaling(a) || isSignaling(b))
        {
            env.flags |= kInvalid;
        }
        return Order::Unordered;
    }
    env.flags |= denormalFlag(0, a, b);
    if (isZero(a) && isZero(b))
    {
        return Order::Equal;
    }
    if (isNegative(a) != isNegative(b))
    {
        return isNegative(a) ? Order::Less : Order::Greater;
    }
    if (!magnitudeLess(a, b) && !magnitudeLess(b, a))
    {
        return Order::Equal;
    }
    return magnitudeLess(a, b) != isNegative(a) ? Order::Less : Order::Greater;
}

auto classify(const F80 a) -> Class
{
    if (isUnsupported(a))
    {
        return Class::Unsupported;
    }
    if (exponentOf(a) == 0)
    {
        return a.significand == 0 ? Class::Zero : Class::Denormal;
    }
    if (exponentOf(a) == 0x7FFF)
    {
        return isInf(a) ? Class::Infinity : Class::NaN;
    }
    return Class::Normal;
}

auto isNegative(const F80 a) -> bool
{
    return (a.signExponent & 0x8000) != 0;
}

auto toHostDouble(const F80 a) -> double
{
    auto       env  = Env{};
    const auto bits = toDouble(a, env);
    double     value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

auto fromHostDouble(const double value) -> F80
{
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    auto env = Env{};
    return fromDouble(bits, env);
}

} // namespace tmweb::f80
