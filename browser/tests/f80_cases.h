#pragma once

// Random x87 operands for the f80 tests, biased toward the encodings and magnitudes with corner cases. The same
// seed gives the same operands on every build, which the native-versus-Wasm digest relies on.

#include "f80/f80.h"

#include <cstdint>
#include <iterator>

namespace tmweb::f80::cases
{

struct Random
{
    std::uint64_t state;

    auto next() -> std::uint64_t
    {
        auto z = (state += 0x9E3779B97F4A7C15ULL);
        z      = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z      = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    auto below(const std::uint64_t n) -> std::uint64_t
    {
        return next() % n;
    }
};

inline auto exponentOf(const F80 a) -> std::int32_t
{
    return a.signExponent & 0x7FFF;
}

inline auto significandPattern(Random& rng) -> std::uint64_t
{
    // One draw per statement: the order of operands in an expression is unspecified, and every compiler must
    // produce the same operands
    const auto kind = rng.below(8);
    const auto bits = rng.next();
    const auto wide = rng.below(2) != 0;
    const auto mask = wide ? 0xFFFFFFFFFFFFF800ULL : 0xFFFFFF0000000000ULL;
    const auto tie  = (wide ? 0x400ULL : 0x0000008000000000ULL) + rng.below(3) - 1;
    switch (kind)
    {
        case 0:
            return ~0ULL;
        case 1:
            return 0x8000000000000000ULL | (bits & 3);
        case 2: // a float or double value: low bits clear, so precision-control ties happen
            return bits & mask;
        case 3: // just above or below a 24- or 53-bit tie
            return (bits & mask) | tie;
        default:
            return bits;
    }
}

// A random operand, biased toward the encodings and magnitudes where x87 behaviour has corner cases
inline auto operand(Random& rng, const F80* near) -> F80
{
    const auto sign = static_cast<std::uint16_t>(rng.below(2) ? 0x8000 : 0);
    const auto sig  = significandPattern(rng);
    switch (rng.below(20))
    {
        case 0:
            return F80{ 0, sign };
        case 1: // denormal
            return F80{ (sig & 0x7FFFFFFFFFFFFFFFULL) >> rng.below(64), sign };
        case 2: // pseudo-denormal
            return F80{ sig | 0x8000000000000000ULL, sign };
        case 3:
            return F80{ 0x8000000000000000ULL, static_cast<std::uint16_t>(sign | 0x7FFF) };
        case 4: // quiet NaN
            return F80{ sig | 0xC000000000000000ULL, static_cast<std::uint16_t>(sign | 0x7FFF) };
        case 5: // signaling NaN
        {
            auto s = (sig & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
            if (s == 0x8000000000000000ULL)
            {
                s |= 1;
            }
            return F80{ s, static_cast<std::uint16_t>(sign | 0x7FFF) };
        }
        case 6: // unnormal, pseudo-infinity or pseudo-NaN
        {
            const auto e = rng.below(4) ? 1 + rng.below(0x7FFE) : 0x7FFF;
            return F80{ sig & 0x7FFFFFFFFFFFFFFFULL, static_cast<std::uint16_t>(sign | e) };
        }
        case 7: // near the bottom or the top of the exponent range
        {
            const auto e = rng.below(2) ? 1 + rng.below(80) : 0x7FFE - rng.below(80);
            return F80{ sig | 0x8000000000000000ULL, static_cast<std::uint16_t>(sign | e) };
        }
        case 8:
        case 9: // integers and halves near FIST's limits and small values
        {
            static constexpr std::int64_t kBase[] = { 0, 1, 2, 3, 32767, 32768, 65536, 2147483647, 2147483648LL, 4294967296LL, 9223372036854775807LL };
            const auto                    base    = kBase[rng.below(std::size(kBase))];
            auto                          env     = Env{};
            auto                          v       = add(fromInt64(base), fromInt64(static_cast<std::int64_t>(rng.below(5)) - 2), env);
            if (rng.below(2))
            {
                v = add(v, F80{ 0x8000000000000000ULL, static_cast<std::uint16_t>(0x3FFE - rng.below(3)) }, env);
            }
            if (rng.below(4) == 0)
            {
                v = add(fromInt64(9223372036854775807LL), fromInt64(1), env); // 2^63
                v.significand += rng.below(3);
                v.significand -= rng.below(3);
            }
            v.signExponent |= sign;
            return v;
        }
        case 10:
        case 11:
        case 12: // close to the other operand's exponent: cancellation, ties and FPREM's short reductions
            if (near && exponentOf(*near) > 0 && exponentOf(*near) < 0x7FFF)
            {
                auto e = exponentOf(*near) + static_cast<std::int32_t>(rng.below(140)) - 70;
                if (rng.below(2))
                {
                    e = exponentOf(*near) + static_cast<std::int32_t>(rng.below(5)) - 2;
                }
                if (e > 0 && e < 0x7FFF)
                {
                    auto s = sig | 0x8000000000000000ULL;
                    if (rng.below(3) == 0)
                    {
                        s = (near->significand ^ (1ULL << rng.below(64))) | 0x8000000000000000ULL;
                    }
                    return F80{ s, static_cast<std::uint16_t>(sign | e) };
                }
            }
            [[fallthrough]];
        case 13:
        case 14: // around 1
            return F80{ sig | 0x8000000000000000ULL, static_cast<std::uint16_t>(sign | (0x3FFF + rng.below(130) - 65)) };
        default:
            return F80{ sig | 0x8000000000000000ULL, static_cast<std::uint16_t>(sign | (1 + rng.below(0x7FFE))) };
    }
}

inline auto memoryOperand(Random& rng, const bool isDouble) -> std::uint64_t
{
    const auto bits = rng.next();
    switch (rng.below(6))
    {
        case 0: // denormal or zero
            return isDouble ? bits & 0x800FFFFFFFFFFFFFULL >> rng.below(52) : bits & (0x807FFFFFULL >> rng.below(23));
        case 1: // infinity or NaN
            return isDouble ? (bits & 0x800FFFFFFFFFFFFFULL >> rng.below(52)) | 0x7FF0000000000000ULL
                            : (bits & (0x807FFFFFULL >> rng.below(23))) | 0x7F800000ULL;
        default:
            return isDouble ? bits : bits & 0xFFFFFFFFULL;
    }
}

} // namespace tmweb::f80::cases
