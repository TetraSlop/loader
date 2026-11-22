#pragma once

// Shared by the f80 sources: SoftFloat, the encoding predicates and the rounding helper. Not part of the API.

#include "f80/f80.h"

#include <cstring>

extern "C"
{
#include "platform.h"

#include "internals.h"
#include "softfloat.h"
#include "specialize.h"
}

namespace tmweb::f80::detail
{

constexpr std::uint64_t kIntegerBit = 0x8000000000000000ULL;

inline auto exponentOf(const F80 a) -> std::int32_t
{
    return a.signExponent & 0x7FFF;
}

// Unnormals, pseudo-infinities and pseudo-NaNs: a nonzero exponent without the integer bit. The 387 and later
// reject them as invalid operands.
inline auto isUnsupported(const F80 a) -> bool
{
    return exponentOf(a) != 0 && (a.significand & kIntegerBit) == 0;
}

inline auto isNaN(const F80 a) -> bool
{
    return exponentOf(a) == 0x7FFF && (a.significand << 1) != 0;
}

inline auto isSignaling(const F80 a) -> bool
{
    return isNaN(a) && (a.significand & 0x4000000000000000ULL) == 0;
}

inline auto isInf(const F80 a) -> bool
{
    return exponentOf(a) == 0x7FFF && a.significand == kIntegerBit;
}

inline auto isZero(const F80 a) -> bool
{
    return exponentOf(a) == 0 && a.significand == 0;
}

// Denormals and pseudo-denormals (exponent 0 with the integer bit set)
inline auto isDenormal(const F80 a) -> bool
{
    return exponentOf(a) == 0 && a.significand != 0;
}

// The x87 checks operands in this order: unsupported encodings, then NaNs, then the operation's own invalid and
// zero-divide cases. The denormal flag comes last and only when none of those fired.
inline auto denormalFlag(const std::uint16_t raised, const F80 a, const F80 b) -> std::uint16_t
{
    if (raised & (kInvalid | kZeroDiv) || isNaN(a) || isNaN(b))
    {
        return 0;
    }
    return isDenormal(a) || isDenormal(b) ? kDenormal : 0;
}

inline auto toSoft(F80 a) -> extFloat80_t
{
    // A pseudo-denormal means the same as exponent 1, which is what SoftFloat expects
    if (exponentOf(a) == 0 && (a.significand & kIntegerBit))
    {
        a.signExponent |= 1;
    }
    extFloat80_t soft;
    soft.signif  = a.significand;
    soft.signExp = a.signExponent;
    return soft;
}

inline auto fromSoft(const extFloat80_t a) -> F80
{
    return F80{ a.signif, a.signExp };
}

inline auto same(const extFloat80_t a, const extFloat80_t b) -> bool
{
    return a.signif == b.signif && a.signExp == b.signExp;
}

inline auto same(const float32_t a, const float32_t b) -> bool
{
    return a.v == b.v;
}

inline auto same(const float64_t a, const float64_t b) -> bool
{
    return a.v == b.v;
}

inline auto same(const std::int64_t a, const std::int64_t b) -> bool
{
    return a == b;
}

inline constexpr std::uint8_t kSoftRounding[4] = { softfloat_round_near_even, softfloat_round_min, softfloat_round_max, softfloat_round_minMag };

auto softFlags(std::uint_fast8_t soft) -> std::uint16_t;

// Runs op under env's rounding mode and the given precision (80, 64 or 32 bits) and adds its exceptions to env.
// SoftFloat does not say which way it rounded, so an inexact result is computed again toward zero: if the two
// differ, the first was rounded away from zero, which is the x87's C1.
// ponytail: one extra operation per inexact result; patch softfloat_roundPackTo* to record the increment if
// profiling ever shows it.
template <class Op>
auto rounded(Env& env, const std::uint8_t precision, Op op)
{
    softfloat_roundingMode   = kSoftRounding[env.rounding];
    extF80_roundingPrecision = precision;
    softfloat_exceptionFlags = 0;
    const auto result        = op();
    const auto raised        = softfloat_exceptionFlags;
    if ((raised & softfloat_flag_inexact) && env.rounding != kZero)
    {
        softfloat_roundingMode = softfloat_round_minMag;
        if (!same(op(), result))
        {
            env.flags |= kRoundedUp;
        }
    }
    env.flags |= softFlags(raised);
    return result;
}

// A finite nonzero value as a significand with the integer bit set and a biased exponent, below 1 for denormals
struct Normal
{
    std::int32_t  exponent;
    std::uint64_t significand;
};

// Runs op on a fresh Env and adds the denormal-operand flag unless op raised invalid or zero-divide
template <class Op>
auto checked(const F80 a, const F80 b, Env& env, Op op)
{
    auto       local  = Env{ env.precision, env.rounding, 0 };
    const auto result = op(local);
    env.flags |= local.flags | denormalFlag(local.flags, a, b);
    return result;
}

auto normalized(F80 a) -> Normal;
auto invalid(Env& env) -> F80;
auto propagate(F80 a, F80 b, Env& env) -> F80;
// Rounds a normalized significand and exponent to 64 bits by env.rounding, with overflow and underflow
auto pack(bool negative, std::int32_t exponent, std::uint64_t significand, Env& env) -> F80;

} // namespace tmweb::f80::detail
