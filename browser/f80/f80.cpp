// Placeholder bodies so the skeleton links; the soft-float implementation replaces this file.
#include "f80/f80.h"

#include <cstdio>
#include <cstdlib>

namespace tmweb::f80
{
namespace
{

[[noreturn]] void missing(const char* name)
{
    std::fprintf(stderr, "f80::%s is not implemented yet\n", name);
    std::abort();
}

} // namespace

auto envFromControl(const std::uint16_t controlWord) -> Env
{
    return Env{ static_cast<Precision>((controlWord >> 8) & 3), static_cast<Rounding>((controlWord >> 10) & 3), 0 };
}

auto zero(const bool negative) -> F80
{
    auto value = F80{};
    if (negative)
    {
        value.signExponent = 0x8000;
    }
    return value;
}

auto one() -> F80
{
    return F80{ 0x8000000000000000ULL, 0x3FFF };
}

auto infinity(const bool negative) -> F80
{
    auto value = F80{ 0x8000000000000000ULL, 0x7FFF };
    if (negative)
    {
        value.signExponent = 0xFFFF;
    }
    return value;
}

auto indefinite() -> F80
{
    return F80{ 0xC000000000000000ULL, 0xFFFF };
}

auto pi() -> F80
{
    return F80{ 0xC90FDAA22168C235ULL, 0x4000 };
}

auto constant(std::uint8_t, Env&) -> F80
{
    missing("constant");
}

auto fromInt64(std::int64_t) -> F80
{
    missing("fromInt64");
}

auto fromFloat(std::uint32_t, Env&) -> F80
{
    missing("fromFloat");
}

auto fromDouble(std::uint64_t, Env&) -> F80
{
    missing("fromDouble");
}

auto toFloat(F80, Env&) -> std::uint32_t
{
    missing("toFloat");
}

auto toDouble(F80, Env&) -> std::uint64_t
{
    missing("toDouble");
}

auto toInt16(F80, Env&) -> std::int16_t
{
    missing("toInt16");
}

auto toInt32(F80, Env&) -> std::int32_t
{
    missing("toInt32");
}

auto toInt64(F80, Env&) -> std::int64_t
{
    missing("toInt64");
}

auto add(F80, F80, Env&) -> F80
{
    missing("add");
}

auto sub(F80, F80, Env&) -> F80
{
    missing("sub");
}

auto mul(F80, F80, Env&) -> F80
{
    missing("mul");
}

auto div(F80, F80, Env&) -> F80
{
    missing("div");
}

auto sqrt(F80, Env&) -> F80
{
    missing("sqrt");
}

auto roundToInt(F80, Env&) -> F80
{
    missing("roundToInt");
}

auto scale(F80, F80, Env&) -> F80
{
    missing("scale");
}

auto partialRemainder(F80, F80, bool, Env&, std::uint16_t&) -> F80
{
    missing("partialRemainder");
}

auto extract(F80, F80&, Env&) -> F80
{
    missing("extract");
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

auto compare(F80, F80, bool, Env&) -> Order
{
    missing("compare");
}

auto classify(F80) -> Class
{
    missing("classify");
}

auto isNegative(const F80 a) -> bool
{
    return (a.signExponent & 0x8000) != 0;
}

auto toHostDouble(F80) -> double
{
    missing("toHostDouble");
}

auto fromHostDouble(double) -> F80
{
    missing("fromHostDouble");
}

} // namespace tmweb::f80
