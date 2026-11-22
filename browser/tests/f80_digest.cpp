// Hashes every f80 operation's results and flags over fixed random operands at each precision and rounding mode.
// The native and the Wasm build must print the same digest; with --check it must also equal kExpected, which is
// updated deliberately whenever f80's results change (and then checked again with f80_hostdiff).
#include "f80/f80.h"
#include "tests/f80_cases.h"

#include <cstdio>
#include <cstring>

using namespace tmweb::f80;
using namespace tmweb::f80::cases;

namespace
{

constexpr std::uint64_t kExpected = 0xC327A8B681C1EDF6ULL;

struct Digest
{
    std::uint64_t value = 0xCBF29CE484222325ULL; // FNV-1a

    void add(std::uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
        {
            value = (value ^ (v & 0xFF)) * 0x100000001B3ULL;
            v >>= 8;
        }
    }

    void add(const F80 v)
    {
        add(v.significand);
        add(v.signExponent);
    }
};

} // namespace

int main(int argc, char** argv)
{
    constexpr int kCases = 1500;
    auto          digest = Digest{};
    for (int pc = 0; pc < 4; ++pc)
    {
        for (int rc = 0; rc < 4; ++rc)
        {
            const auto control = static_cast<std::uint16_t>((0x037F & ~0x0F00) | (pc << 8) | (rc << 10));
            auto       rng     = Random{ 0x5EED0000ULL + pc * 4 + rc };
            for (int i = 0; i < kCases; ++i)
            {
                const auto a   = operand(rng, nullptr);
                const auto b   = operand(rng, rng.below(2) ? &a : nullptr);
                const auto f32 = static_cast<std::uint32_t>(memoryOperand(rng, false));
                const auto f64 = memoryOperand(rng, true);
                auto       run = [&](auto op)
                {
                    auto env = envFromControl(control);
                    op(env);
                    digest.add(env.flags);
                };
                run([&](Env& env)
                    {
                        digest.add(add(a, b, env));
                    });
                run([&](Env& env)
                    {
                        digest.add(sub(a, b, env));
                    });
                run([&](Env& env)
                    {
                        digest.add(mul(a, b, env));
                    });
                run([&](Env& env)
                    {
                        digest.add(div(a, b, env));
                    });
                run([&](Env& env)
                    {
                        digest.add(sqrt(a, env));
                    });
                run([&](Env& env)
                    {
                        digest.add(roundToInt(a, env));
                    });
                run([&](Env& env)
                    {
                        digest.add(scale(a, b, env));
                    });
                run([&](Env& env)
                    {
                        std::uint16_t cc = 0;
                        digest.add(partialRemainder(a, b, false, env, cc));
                        digest.add(cc);
                    });
                run([&](Env& env)
                    {
                        std::uint16_t cc = 0;
                        digest.add(partialRemainder(a, b, true, env, cc));
                        digest.add(cc);
                    });
                run([&](Env& env)
                    {
                        F80 significand;
                        digest.add(extract(a, significand, env));
                        digest.add(significand);
                    });
                run([&](Env& env)
                    {
                        digest.add(static_cast<std::uint64_t>(compare(a, b, false, env)));
                    });
                run([&](Env& env)
                    {
                        digest.add(static_cast<std::uint64_t>(compare(a, b, true, env)));
                    });
                digest.add(static_cast<std::uint64_t>(classify(a)));
                run([&](Env& env)
                    {
                        digest.add(toFloat(a, env));
                    });
                run([&](Env& env)
                    {
                        digest.add(toDouble(a, env));
                    });
                run([&](Env& env)
                    {
                        digest.add(static_cast<std::uint64_t>(toInt16(a, env)));
                    });
                run([&](Env& env)
                    {
                        digest.add(static_cast<std::uint64_t>(toInt32(a, env)));
                    });
                run([&](Env& env)
                    {
                        digest.add(static_cast<std::uint64_t>(toInt64(a, env)));
                    });
                run([&](Env& env)
                    {
                        digest.add(fromFloat(f32, env));
                    });
                run([&](Env& env)
                    {
                        digest.add(fromDouble(f64, env));
                    });
                run([&](Env& env)
                    {
                        bool       denormal = false;
                        const auto m        = floatOperand(f32, denormal);
                        digest.add(add(a, m, env));
                        if (denormal)
                        {
                            denormalOperand(a, m, env);
                        }
                    });
                run([&](Env& env)
                    {
                        bool       denormal = false;
                        const auto m        = doubleOperand(f64, denormal);
                        digest.add(mul(a, m, env));
                        if (denormal)
                        {
                            denormalOperand(a, m, env);
                        }
                    });
                run([&](Env& env)
                    {
                        digest.add(constant(static_cast<std::uint8_t>(i % 7), env));
                    });
                run([&](Env& env)
                    {
                        F80 s;
                        F80 c;
                        digest.add(sinCos(a, s, c, env));
                        digest.add(s);
                        digest.add(c);
                    });
                run([&](Env& env)
                    {
                        F80 r;
                        digest.add(sin(a, r, env));
                        digest.add(r);
                    });
                run([&](Env& env)
                    {
                        F80 r;
                        digest.add(cos(a, r, env));
                        digest.add(r);
                    });
                run([&](Env& env)
                    {
                        F80 r;
                        digest.add(tan(a, r, env));
                        digest.add(r);
                    });
                run([&](Env& env)
                    {
                        digest.add(atan2(b, a, env));
                    });
                run([&](Env& env)
                    {
                        digest.add(exp2m1(a, env));
                    });
                run([&](Env& env)
                    {
                        digest.add(yLog2X(a, b, env));
                    });
                run([&](Env& env)
                    {
                        digest.add(yLog2Xp1(a, b, env));
                    });
            }
        }
    }
    std::printf("f80 digest %016llX\n", static_cast<unsigned long long>(digest.value));
    if (argc > 1 && std::strcmp(argv[1], "--check") == 0 && digest.value != kExpected)
    {
        std::printf("expected   %016llX\n", static_cast<unsigned long long>(kExpected));
        return 1;
    }
    return 0;
}
