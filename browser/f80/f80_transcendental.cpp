// FSIN..FYL2XP1. Each is computed in SoftFloat's 128-bit format (113-bit significand) from series on a reduced
// argument, then rounded once to 64 bits by the control word's rounding mode, so results are correctly rounded in
// all but rare cases. The x87 itself is only accurate to about one ulp, so a result can differ from the hardware's
// in the last bit; f80_hostdiff measures how often.
#include "f80/f80_internal.h"

#include <array>

namespace tmweb::f80
{

using namespace detail;

namespace
{

using F128 = float128_t;

// Correctly rounded 128-bit constants, low word first (generated with Python's decimal at 250 digits)
constexpr F128 kPi            = { { 0x8469898CC51701B8ULL, 0x4000921FB54442D1ULL } };
constexpr F128 kHalfPi        = { { 0x8469898CC51701B8ULL, 0x3FFF921FB54442D1ULL } };
constexpr F128 kQuarterPi     = { { 0x8469898CC51701B8ULL, 0x3FFE921FB54442D1ULL } };
constexpr F128 kThreeFourthPi = { { 0x234F272993D1414AULL, 0x40002D97C7F3321DULL } };
constexpr F128 kLn2           = { { 0xF35793C7673007E6ULL, 0x3FFE62E42FEFA39EULL } };
constexpr F128 kLog2E         = { { 0xE1777D0FFDA0D23AULL, 0x3FFF71547652B82FULL } };
constexpr F128 kTwoOverPi     = { { 0x2A53F84EAFA3EA6AULL, 0x3FFE45F306DC9C88ULL } };

// FSIN, FCOS, FSINCOS and FPTAN reduce their argument by the x87's 66-bit pi, not the true one, which is what makes
// large arguments come out as they do on hardware (measured on an AMD Zen 5; Intel documents the same 66 bits).
// Here pi66/2 * 2^383, least significant 32 bits first.
constexpr std::array<std::uint32_t, 12> kHalfPiFixed = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xC0000000, 0x2168C234, 0xC90FDAA2 };

// Internal arithmetic rounds to nearest; its exception flags are discarded
auto operator+(const F128 a, const F128 b) -> F128
{
    return f128_add(a, b);
}

auto operator-(const F128 a, const F128 b) -> F128
{
    return f128_sub(a, b);
}

auto operator*(const F128 a, const F128 b) -> F128
{
    return f128_mul(a, b);
}

auto operator/(const F128 a, const F128 b) -> F128
{
    return f128_div(a, b);
}

auto operator<(const F128 a, const F128 b) -> bool
{
    return f128_lt(a, b);
}

auto operator==(const F128 a, const F128 b) -> bool
{
    return a.v[0] == b.v[0] && a.v[1] == b.v[1];
}

auto negated(F128 a) -> F128
{
    a.v[1] ^= 0x8000000000000000ULL;
    return a;
}

auto integer(const std::int64_t value) -> F128
{
    return i64_to_f128(value);
}

const F128 kOne = integer(1);
const F128 kTwo = integer(2);

void internalRounding()
{
    softfloat_roundingMode = softfloat_round_near_even;
}

// |a| as a 128-bit value in [1, 2) and its unbiased binary exponent; a finite and nonzero
struct Split
{
    F128         m;
    std::int32_t exponent;
};

auto split(const F80 a) -> Split
{
    const auto n = normalized(a);
    return Split{ extF80_to_f128(extFloat80_t{ n.significand, 0x3FFF }), n.exponent - 0x3FFF };
}

auto exponent128(const F128 a) -> std::int32_t
{
    return static_cast<std::int32_t>((a.v[1] >> 48) & 0x7FFF) - 0x3FFF;
}

// a * 2^n for a normal a whose result stays normal
auto scaled(F128 a, const std::int32_t n) -> F128
{
    a.v[1] += static_cast<std::uint64_t>(static_cast<std::int64_t>(n)) << 48;
    return a;
}

// Horner's rule over coefficients c[0] + c[1] z + c[2] z^2 + ...
template <std::size_t N>
auto polynomial(const std::array<F128, N>& c, const F128 z) -> F128
{
    auto sum = c[N - 1];
    for (auto i = N - 1; i-- > 0;)
    {
        sum = sum * z + c[i];
    }
    return sum;
}

// Taylor coefficients, built once in 128-bit arithmetic
template <std::size_t N, class Term>
auto coefficients(Term term) -> std::array<F128, N>
{
    internalRounding();
    std::array<F128, N> c{};
    for (std::size_t i = 0; i < N; ++i)
    {
        c[i] = term(static_cast<std::int64_t>(i));
    }
    return c;
}

auto reciprocalFactorial(const std::int64_t n) -> F128
{
    auto value = kOne;
    for (std::int64_t i = 2; i <= n; ++i)
    {
        value = value / integer(i);
    }
    return value;
}

// sin r = r * S(r^2), cos r = C(r^2), for |r| <= pi/4: the first neglected terms are below 2^-120
const auto kSin = coefficients<17>([](const std::int64_t i)
                                   {
                                       const auto c = reciprocalFactorial(2 * i + 1);
                                       return i % 2 ? negated(c) : c;
                                   });
const auto kCos = coefficients<17>([](const std::int64_t i)
                                   {
                                       const auto c = reciprocalFactorial(2 * i);
                                       return i % 2 ? negated(c) : c;
                                   });
// atan u = u * A(u^2) for |u| <= 0.1, after two argument halvings
const auto kAtan = coefficients<19>([](const std::int64_t i)
                                    {
                                        const auto c = kOne / integer(2 * i + 1);
                                        return i % 2 ? negated(c) : c;
                                    });
// atanh s = s * H(s^2) for |s| <= 0.172, which ln(m) = 2 atanh((m - 1) / (m + 1)) needs for m in [sqrt(1/2), sqrt(2)]
const auto kAtanh = coefficients<25>([](const std::int64_t i)
                                     {
                                         return kOne / integer(2 * i + 1);
                                     });
// expm1 z = z * E(z) for |z| <= ln 2
const auto kExpm1 = coefficients<31>([](const std::int64_t i)
                                     {
                                         return reciprocalFactorial(i + 1);
                                     });

const F128 kSqrt2       = f128_sqrt(kTwo);
const F128 kSqrtHalf    = kSqrt2 / kTwo;
const F128 kTanEighthPi = kSqrt2 - kOne;
const F128 kTwoLog2E    = kLog2E * kTwo;
const F128 kPrem1Limit  = kOne - kSqrtHalf; // FYL2XP1's documented range

// Rounds value * 2^scale to 64 bits by env.rounding with the x87's flags and C1. tail is the sign of what the
// 128-bit value leaves out, relative to its magnitude: -1 when the true result is slightly smaller, +1 when larger,
// 0 when value is exact or its own rounding error already decides.
auto finish(const F128 value, const std::int32_t scale, const int tail, Env& env) -> F80
{
    const bool negative = (value.v[1] >> 63) != 0;
    auto       exponent = static_cast<std::int32_t>((value.v[1] >> 48) & 0x7FFF);
    auto       high     = value.v[1] & 0x0000FFFFFFFFFFFFULL;
    auto       low      = value.v[0];
    if (exponent == 0 && high == 0 && low == 0)
    {
        return zero(negative);
    }
    if (exponent == 0)
    {
        exponent = 1;
        while (!(high & 0x0001000000000000ULL))
        {
            high = (high << 1) | (low >> 63);
            low <<= 1;
            --exponent;
        }
    }
    high |= 0x0001000000000000ULL;
    auto significand = (high << 15) | (low >> 49);
    auto extra       = low << 15;
    exponent += scale;
    if (extra == 0 && tail > 0)
    {
        extra = 1;
    }
    else if (extra == 0 && tail < 0)
    {
        extra = ~0ULL;
        if (significand == kIntegerBit)
        {
            significand = ~0ULL;
            --exponent;
        }
        else
        {
            --significand;
        }
    }
    return fromSoft(rounded(env, 80, [&]
                            {
                                return softfloat_roundPackToExtF80(negative, exponent, significand, extra, 80);
                            }));
}

auto finish(const F128 value, Env& env) -> F80
{
    return finish(value, 0, 0, env);
}

// x - k pi66/2 for the k nearest x * 2/pi, x = |a| below 2^63, computed exactly and then rounded to 128 bits
struct Reduced
{
    F128          r;
    std::uint32_t quadrant;
};

auto reduce(const F80 a) -> Reduced
{
    const auto n = normalized(a);
    if (n.exponent < 0x3FFE) // |a| < 1/2
    {
        return Reduced{ extF80_to_f128(toSoft(abs(a))), 0 };
    }
    const auto k = static_cast<std::uint64_t>(
        f128_to_i64(extF80_to_f128(toSoft(abs(a))) * kTwoOverPi, softfloat_round_near_even, false));

    // Fixed point with 383 fraction bits, 15 limbs, two's complement
    std::array<std::uint32_t, 15> acc{};
    const auto                    shift = n.exponent - 0x3FFF - 63 + 383;
    std::uint64_t                 carry = 0;
    for (int j = 0; j < 3; ++j)
    {
        const std::uint64_t piece = j == 0 ? n.significand & 0xFFFFFFFF : j == 1 ? n.significand >> 32
                                                                                 : 0;
        const auto          v     = (piece << (shift % 32)) | carry;
        acc[shift / 32 + j]       = static_cast<std::uint32_t>(v);
        carry                     = v >> 32;
    }
    std::array<std::uint32_t, 15> product{};
    for (int i = 0; i < 2; ++i)
    {
        const std::uint64_t ki = i == 0 ? k & 0xFFFFFFFF : k >> 32;
        std::uint64_t       c  = 0;
        for (std::size_t j = 0; j < kHalfPiFixed.size(); ++j)
        {
            const auto v   = ki * kHalfPiFixed[j] + product[i + j] + c;
            product[i + j] = static_cast<std::uint32_t>(v);
            c              = v >> 32;
        }
        for (std::size_t j = i + kHalfPiFixed.size(); c != 0 && j < product.size(); ++j)
        {
            const auto v = static_cast<std::uint64_t>(product[j]) + c;
            product[j]   = static_cast<std::uint32_t>(v);
            c            = v >> 32;
        }
    }
    std::uint64_t borrow = 0;
    for (std::size_t j = 0; j < acc.size(); ++j)
    {
        const auto v = static_cast<std::uint64_t>(acc[j]) - product[j] - borrow;
        acc[j]       = static_cast<std::uint32_t>(v);
        borrow       = (v >> 32) & 1;
    }
    const bool negative = borrow != 0;
    if (negative)
    {
        std::uint64_t c = 1;
        for (auto& limb : acc)
        {
            const auto v = static_cast<std::uint64_t>(static_cast<std::uint32_t>(~limb)) + c;
            limb         = static_cast<std::uint32_t>(v);
            c            = v >> 32;
        }
    }
    // The top 128 bits, as a 128-bit float
    int top = static_cast<int>(acc.size()) * 32 - 1;
    while (top > 0 && !(acc[top / 32] >> (top % 32) & 1))
    {
        --top;
    }
    auto bit = [&](const int i) -> std::uint64_t
    {
        return i < 0 ? 0 : (acc[i / 32] >> (i % 32)) & 1;
    };
    std::uint64_t high = 0;
    std::uint64_t low  = 0;
    for (int i = 0; i < 64; ++i)
    {
        high = (high << 1) | bit(top - i);
        low  = (low << 1) | bit(top - 64 - i);
    }
    const auto r = softfloat_normRoundPackToF128(negative, top - 127 - 383 + 0x3FFF + 111, high, low);
    return Reduced{ r, static_cast<std::uint32_t>(k & 3) };
}

auto sinOf(const F128 r, int& tail) -> F128
{
    const auto s = r * polynomial(kSin, r * r);
    tail         = s == r ? -1 : 0; // sin r = r - r^3/6 + ...
    return s;
}

auto cosOf(const F128 r, int& tail) -> F128
{
    const auto c = polynomial(kCos, r * r);
    tail         = c == kOne ? -1 : 0;
    return c;
}

// The checks the trigonometric instructions share. Returns true with result set when it is decided here.
auto trigSpecial(const F80 a, Env& env, F80& result, bool& inRange) -> bool
{
    inRange = true;
    if (isUnsupported(a) || isInf(a))
    {
        result = invalid(env);
        return true;
    }
    if (isNaN(a))
    {
        result = propagate(a, a, env);
        return true;
    }
    if (exponentOf(a) >= 0x3FFF + 63)
    {
        inRange = false;
        result  = a;
        return true;
    }
    return false;
}

// sin and cos of a, each with its tail, by quadrant
struct SinCos
{
    F128 sin;
    int  sinTail;
    F128 cos;
    int  cosTail;
};

auto sinCosOf(const F80 a) -> SinCos
{
    internalRounding();
    const auto red = reduce(a);
    int        st  = 0;
    int        ct  = 0;
    const auto s   = sinOf(red.r, st);
    const auto c   = cosOf(red.r, ct);
    SinCos     out{};
    switch (red.quadrant)
    {
        case 0:
            out = { s, st, c, ct };
            break;
        case 1:
            out = { c, ct, negated(s), st };
            break;
        case 2:
            out = { negated(s), st, negated(c), ct };
            break;
        default:
            out = { negated(c), ct, s, st };
            break;
    }
    if (isNegative(a))
    {
        out.sin = negated(out.sin);
    }
    return out;
}

auto atanUnit(const F128 t, int& tail) -> F128
{
    // t in (0, 1]
    tail = 0;
    if (exponent128(t) < -60)
    {
        tail = -1;
        return t;
    }
    auto base = F128{};
    auto u    = t;
    if (kTanEighthPi < t)
    {
        base = kQuarterPi;
        u    = (t - kOne) / (t + kOne);
    }
    for (int i = 0; i < 2; ++i)
    {
        u = u / (kOne + f128_sqrt(kOne + u * u));
    }
    const auto a = u * polynomial(kAtan, u * u) * integer(4);
    return base.v[1] == 0 && base.v[0] == 0 ? a : base + a;
}

// log2(m) for m in [sqrt(1/2), sqrt(2)]
auto log2Near1(const F128 m) -> F128
{
    const auto s = (m - kOne) / (m + kOne);
    return s * polynomial(kAtanh, s * s) * kTwoLog2E;
}

// log2(m * 2^exponent) for m in [1, 2)
auto log2Of(const Split v) -> F128
{
    auto e = v.exponent;
    auto m = v.m;
    if (kSqrt2 < m)
    {
        m = scaled(m, -1);
        ++e;
    }
    const auto l = log2Near1(m);
    return e == 0 ? l : integer(e) + l;
}

// y * l with y's exponent kept out of the 128-bit range until the final rounding. The measured x87 reports a
// denormal result as underflowed and inexact even when it is exact (y denormal and x a power of two).
auto timesY(const F80 y, const F128 l, Env& env) -> F80
{
    const auto ys     = split(y);
    const auto p      = isNegative(y) ? negated(ys.m * l) : ys.m * l;
    const auto result = finish(p, ys.exponent, 0, env);
    if (exponentOf(result) == 0)
    {
        env.flags |= kUnderflow | kInexact;
    }
    return result;
}

auto signedValue(const F128 v, const bool negative, Env& env) -> F80
{
    return finish(negative ? negated(v) : v, env);
}

auto sinImpl(const F80 a, F80& result, Env& env) -> bool
{
    bool inRange = true;
    if (trigSpecial(a, env, result, inRange))
    {
        return inRange;
    }
    if (isZero(a))
    {
        result = a;
        return true;
    }
    const auto sc = sinCosOf(a);
    result        = finish(sc.sin, 0, sc.sinTail, env);
    return true;
}

auto cosImpl(const F80 a, F80& result, Env& env) -> bool
{
    bool inRange = true;
    if (trigSpecial(a, env, result, inRange))
    {
        return inRange;
    }
    if (isZero(a))
    {
        result = one();
        return true;
    }
    const auto sc = sinCosOf(a);
    result        = finish(sc.cos, 0, sc.cosTail, env);
    return true;
}

auto sinCosImpl(const F80 a, F80& sine, F80& cosine, Env& env) -> bool
{
    bool inRange = true;
    F80  special;
    if (trigSpecial(a, env, special, inRange))
    {
        sine   = special;
        cosine = special;
        return inRange;
    }
    if (isZero(a))
    {
        sine   = a;
        cosine = one();
        return true;
    }
    const auto sc = sinCosOf(a);
    sine          = finish(sc.sin, 0, sc.sinTail, env);
    cosine        = finish(sc.cos, 0, sc.cosTail, env);
    return true;
}

auto tanImpl(const F80 a, F80& result, Env& env) -> bool
{
    bool inRange = true;
    if (trigSpecial(a, env, result, inRange))
    {
        return inRange;
    }
    if (isZero(a))
    {
        result = a;
        return true;
    }
    internalRounding();
    const auto red  = reduce(a);
    int        st   = 0;
    int        ct   = 0;
    const auto s    = sinOf(red.r, st);
    const auto c    = cosOf(red.r, ct);
    const bool odd  = (red.quadrant & 1) != 0;
    auto       t    = odd ? negated(c / s) : s / c;
    const auto tail = !odd && st < 0 ? 1 : 0; // tan r = r + r^3/3 + ...
    if (tail)
    {
        t = red.r;
    }
    result = finish(isNegative(a) ? negated(t) : t, 0, tail, env);
    return true;
}

auto atan2Impl(const F80 y, const F80 x, Env& env) -> F80
{
    if (isUnsupported(x) || isUnsupported(y))
    {
        return invalid(env);
    }
    if (isNaN(x) || isNaN(y))
    {
        return propagate(x, y, env);
    }
    internalRounding();
    const bool ny = isNegative(y);
    const bool nx = isNegative(x);
    if (isZero(y))
    {
        return nx ? signedValue(kPi, ny, env) : zero(ny);
    }
    if (isInf(y))
    {
        return signedValue(isInf(x) ? (nx ? kThreeFourthPi : kQuarterPi) : kHalfPi, ny, env);
    }
    if (isInf(x))
    {
        return nx ? signedValue(kPi, ny, env) : zero(ny);
    }
    if (isZero(x))
    {
        return signedValue(kHalfPi, ny, env);
    }
    const auto sy   = split(y);
    const auto sx   = split(x);
    const bool swap = sx.exponent < sy.exponent || (sx.exponent == sy.exponent && sx.m < sy.m);
    // t = min(|x|, |y|) / max(|x|, |y|), in (0, 1]
    const auto q = swap ? sx.m / sy.m : sy.m / sx.m;
    const auto d = swap ? sx.exponent - sy.exponent : sy.exponent - sx.exponent;
    if (!swap && !nx && d + exponent128(q) < -60)
    {
        // atan t = t - t^3/3 + ...: t itself, slightly large, possibly below the 128-bit range
        return finish(ny ? negated(q) : q, d, -1, env);
    }
    int  tail = 0;
    auto a    = d + exponent128(q) < -16000 ? F128{} : atanUnit(scaled(q, d), tail);
    if (swap)
    {
        a    = kHalfPi - a;
        tail = 0;
    }
    if (nx)
    {
        a    = kPi - a;
        tail = 0;
    }
    return finish(ny ? negated(a) : a, 0, tail, env);
}

auto exp2m1Impl(const F80 a, Env& env) -> F80
{
    if (isUnsupported(a))
    {
        return invalid(env);
    }
    if (isNaN(a))
    {
        return propagate(a, a, env);
    }
    if (isZero(a))
    {
        return a;
    }
    if (isInf(a))
    {
        return isNegative(a) ? negate(one()) : a;
    }
    if (exponentOf(a) >= 0x3FFF && !(exponentOf(a) == 0x3FFF && a.significand == kIntegerBit))
    {
        // Outside F2XM1's documented range of [-1, 1] the x87 is undefined; the measured one returns a, inexact
        env.flags |= kInexact;
        return a;
    }
    internalRounding();
    const auto z = extF80_to_f128(toSoft(a)) * kLn2;
    return finish(z * polynomial(kExpm1, z), env);
}

auto yLog2XImpl(const F80 x, const F80 y, Env& env) -> F80
{
    if (isUnsupported(x) || isUnsupported(y))
    {
        return invalid(env);
    }
    if (isNaN(x) || isNaN(y))
    {
        return propagate(x, y, env);
    }
    if (isNegative(x) && !isZero(x))
    {
        return invalid(env);
    }
    const bool ny = isNegative(y);
    if (isZero(x))
    {
        if (isZero(y))
        {
            return invalid(env);
        }
        env.flags |= isInf(y) ? 0 : kZeroDiv;
        return infinity(!ny);
    }
    if (isInf(x))
    {
        return isZero(y) ? invalid(env) : infinity(ny);
    }
    internalRounding();
    const auto sx       = split(x);
    const bool isOne    = sx.exponent == 0 && sx.m == kOne;
    const bool belowOne = sx.exponent < 0;
    if (isInf(y))
    {
        return isOne ? invalid(env) : infinity(belowOne != ny);
    }
    if (isZero(y) || isOne)
    {
        return zero(belowOne != ny);
    }
    return timesY(y, log2Of(sx), env);
}

auto yLog2Xp1Impl(const F80 x, const F80 y, Env& env) -> F80
{
    if (isUnsupported(x) || isUnsupported(y))
    {
        return invalid(env);
    }
    if (isNaN(x) || isNaN(y))
    {
        return propagate(x, y, env);
    }
    const bool nx = isNegative(x);
    const bool ny = isNegative(y);
    if (isZero(x))
    {
        return isInf(y) ? invalid(env) : zero(nx != ny);
    }
    if (isInf(x))
    {
        return nx || isZero(y) ? invalid(env) : infinity(ny);
    }
    if (isInf(y))
    {
        return infinity(nx != ny);
    }
    if (isZero(y))
    {
        return zero(nx != ny);
    }
    internalRounding();
    const auto xv = extF80_to_f128(toSoft(x));
    if (nx && !(negated(kOne) < xv))
    {
        // x <= -1 has no logarithm; the measured x87 returns x, inexact
        env.flags |= kInexact;
        return x;
    }
    if (isDenormal(x))
    {
        // log2(1 + x) = x log2(e) to far beyond 128 bits; x itself is below the 128-bit normal range, so keep its
        // exponent apart
        const auto sx = split(x);
        const auto sy = split(y);
        const auto p  = sx.m * sy.m * kLog2E;
        return finish(nx != ny ? negated(p) : p, sx.exponent + sy.exponent, 0, env);
    }
    const auto l = f128_lt(f128_mul(xv, xv), kPrem1Limit * kPrem1Limit)
                       ? (xv / (kTwo + xv)) * polynomial(kAtanh, (xv / (kTwo + xv)) * (xv / (kTwo + xv))) * kTwoLog2E
                       : log2Of(Split{ scaled(kOne + xv, -exponent128(kOne + xv)), exponent128(kOne + xv) });
    return timesY(y, l, env);
}

} // namespace

auto sin(const F80 a, F80& result, Env& env) -> bool
{
    return checked(a, a, env, [&](Env& local)
                   {
                       return sinImpl(a, result, local);
                   });
}

auto cos(const F80 a, F80& result, Env& env) -> bool
{
    return checked(a, a, env, [&](Env& local)
                   {
                       return cosImpl(a, result, local);
                   });
}

auto sinCos(const F80 a, F80& sine, F80& cosine, Env& env) -> bool
{
    return checked(a, a, env, [&](Env& local)
                   {
                       return sinCosImpl(a, sine, cosine, local);
                   });
}

auto tan(const F80 a, F80& result, Env& env) -> bool
{
    return checked(a, a, env, [&](Env& local)
                   {
                       return tanImpl(a, result, local);
                   });
}

auto atan2(const F80 y, const F80 x, Env& env) -> F80
{
    return checked(x, y, env, [&](Env& local)
                   {
                       return atan2Impl(y, x, local);
                   });
}

auto exp2m1(const F80 a, Env& env) -> F80
{
    return checked(a, a, env, [&](Env& local)
                   {
                       return exp2m1Impl(a, local);
                   });
}

auto yLog2X(const F80 x, const F80 y, Env& env) -> F80
{
    return checked(x, y, env, [&](Env& local)
                   {
                       return yLog2XImpl(x, y, local);
                   });
}

auto yLog2Xp1(const F80 x, const F80 y, Env& env) -> F80
{
    return checked(x, y, env, [&](Env& local)
                   {
                       return yLog2Xp1Impl(x, y, local);
                   });
}

} // namespace tmweb::f80
