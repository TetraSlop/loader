// Differential test of f80 against this machine's x87, on random operands at every precision and rounding mode.
// Windows x86 with MSVC only (inline assembly); run by hand: f80_hostdiff [cases per op and mode] [seed] [op].
// Exits nonzero when an exactly specified operation differs. Transcendentals are reported as ulp statistics.
#include "f80/f80.h"
#include "tests/f80_cases.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace tmweb::f80;
using namespace tmweb::f80::cases;

namespace
{

// Operands and results the assembly reads and writes; the 16-byte F80 keeps the 10-byte memory layout at offset 0
struct Io
{
    F80           a;
    F80           b;
    F80           r1;
    F80           r2;
    std::uint64_t mem;
    std::uint16_t cw;
    std::uint16_t sw;
};

// Loads b then a (ST0 = a, ST1 = b), runs the instruction, saves the status word, stores ST0 and ST1, resets.
#define HOST_OP(name, ...)                                                                                                                                    \
    void name(Io* io)                                                                                                                                         \
    {                                                                                                                                                         \
        std::uint16_t saved;                                                                                                                                  \
        __asm mov ecx, io __asm fnstcw saved __asm fninit __asm fldcw[ecx] Io.cw __asm fld tbyte ptr[ecx] Io.b __asm fld tbyte ptr[ecx] Io.a                  \
                           __VA_ARGS__ __asm fnstsw[ecx] Io.sw __asm fstp tbyte ptr[ecx] Io.r1 __asm fstp tbyte ptr[ecx] Io.r2 __asm fninit __asm fldcw saved \
    }

HOST_OP(hostAdd, __asm fadd st, st(1))
HOST_OP(hostSub, __asm fsub st, st(1))
HOST_OP(hostMul, __asm fmul st, st(1))
HOST_OP(hostDiv, __asm fdiv st, st(1))
HOST_OP(hostSqrt, __asm fsqrt)
HOST_OP(hostRndint, __asm frndint)
HOST_OP(hostScale, __asm fscale)
HOST_OP(hostPrem, __asm fprem)
HOST_OP(hostPrem1, __asm fprem1)
HOST_OP(hostXtract, __asm fxtract)
HOST_OP(hostCom, __asm fcom st(1))
HOST_OP(hostUcom, __asm fucom st(1))
HOST_OP(hostXam, __asm fxam)
HOST_OP(hostStF32, __asm fst dword ptr[ecx] Io.mem)
HOST_OP(hostStF64, __asm fst qword ptr[ecx] Io.mem)
HOST_OP(hostStI16, __asm fist word ptr[ecx] Io.mem)
HOST_OP(hostStI32, __asm fist dword ptr[ecx] Io.mem)
HOST_OP(hostStI64, __asm fld st(0) __asm fistp qword ptr[ecx] Io.mem)
HOST_OP(hostLdF32, __asm fld dword ptr[ecx] Io.mem)
HOST_OP(hostLdF64, __asm fld qword ptr[ecx] Io.mem)
HOST_OP(hostAddF32, __asm fadd dword ptr[ecx] Io.mem)
HOST_OP(hostMulF64, __asm fmul qword ptr[ecx] Io.mem)
HOST_OP(hostDivI32, __asm fidiv dword ptr[ecx] Io.mem)
HOST_OP(hostComI32, __asm ficom dword ptr[ecx] Io.mem)
HOST_OP(hostLd1, __asm fld1)
HOST_OP(hostLdl2t, __asm fldl2t)
HOST_OP(hostLdl2e, __asm fldl2e)
HOST_OP(hostLdpi, __asm fldpi)
HOST_OP(hostLdlg2, __asm fldlg2)
HOST_OP(hostLdln2, __asm fldln2)
HOST_OP(hostSin, __asm fsin)
HOST_OP(hostCos, __asm fcos)
HOST_OP(hostSinCos, __asm fsincos)
HOST_OP(hostTan, __asm fptan)
HOST_OP(hostAtan, __asm fpatan)
HOST_OP(hostF2xm1, __asm f2xm1)
HOST_OP(hostYl2x, __asm fyl2x)
HOST_OP(hostYl2xp1, __asm fyl2xp1)

constexpr std::uint16_t kC0 = 0x0100;
constexpr std::uint16_t kC1 = 0x0200;
constexpr std::uint16_t kC2 = 0x0400;
constexpr std::uint16_t kC3 = 0x4000;

auto withC1(const Env& env) -> std::uint16_t
{
    return static_cast<std::uint16_t>((env.flags & 0x3F) | ((env.flags & kRoundedUp) ? kC1 : 0));
}

auto orderCodes(const Order order) -> std::uint16_t
{
    switch (order)
    {
        case Order::Less:
            return kC0;
        case Order::Equal:
            return kC3;
        case Order::Greater:
            return 0;
        default:
            return kC0 | kC2 | kC3;
    }
}

auto classCodes(const F80 a) -> std::uint16_t
{
    static constexpr std::uint16_t kCodes[] = { 0, kC0, kC2, kC2 | kC0, kC3, kC3 | kC0, kC3 | kC2 };
    return static_cast<std::uint16_t>(kCodes[static_cast<int>(classify(a))] | (isNegative(a) ? kC1 : 0));
}

enum class Kind
{
    Exact,         // bit-exact result, exception flags and C1
    Transcendental // ulp statistics
};

// What the soft side computes for one case: the comparable status bits and results
struct Soft
{
    std::uint16_t sw;
    F80           r1;
    F80           r2;
    std::uint64_t mem;
};

struct Op
{
    const char* name;
    void (*host)(Io*);
    Soft (*soft)(const Io&);
    std::uint16_t compared; // status bits compared besides the exception flags
    int           results;  // 0: memory, 1: r1, 2: r1 and r2
    Kind          kind;
    int           operands; // how operands are generated: 1 unary, 2 binary, 3 memory load
};

template <F80 (*Fn)(F80, F80, Env&)>
auto softBinary(const Io& io) -> Soft
{
    auto       env = envFromControl(io.cw);
    const auto r   = Fn(io.a, io.b, env);
    return Soft{ withC1(env), r, io.b, 0 };
}

template <F80 (*Fn)(F80, Env&)>
auto softUnary(const Io& io) -> Soft
{
    auto       env = envFromControl(io.cw);
    const auto r   = Fn(io.a, env);
    return Soft{ withC1(env), r, io.b, 0 };
}

auto softPrem(const Io& io, const bool ieee) -> Soft
{
    auto          env = envFromControl(io.cw);
    std::uint16_t cc  = 0;
    const auto    r   = partialRemainder(io.a, io.b, ieee, env, cc);
    return Soft{ static_cast<std::uint16_t>((env.flags & 0x3F) | cc), r, io.b, 0 };
}

auto softXtract(const Io& io) -> Soft
{
    auto       env = envFromControl(io.cw);
    F80        significand;
    const auto exponent = extract(io.a, significand, env);
    return Soft{ withC1(env), significand, exponent, 0 };
}

auto softCompare(const Io& io, const bool quiet) -> Soft
{
    auto       env   = envFromControl(io.cw);
    const auto order = compare(io.a, io.b, quiet, env);
    return Soft{ static_cast<std::uint16_t>((env.flags & 0x3F) | orderCodes(order)), io.a, io.b, 0 };
}

auto softStore(const Io& io, const int which) -> Soft
{
    auto          env = envFromControl(io.cw);
    std::uint64_t mem = io.mem;
    switch (which)
    {
        case 0:
            mem = (mem & ~0xFFFFFFFFULL) | toFloat(io.a, env);
            break;
        case 1:
            mem = toDouble(io.a, env);
            break;
        case 2:
            mem = (mem & ~0xFFFFULL) | static_cast<std::uint16_t>(toInt16(io.a, env));
            break;
        case 3:
            mem = (mem & ~0xFFFFFFFFULL) | static_cast<std::uint32_t>(toInt32(io.a, env));
            break;
        default:
            mem = static_cast<std::uint64_t>(toInt64(io.a, env));
            break;
    }
    return Soft{ withC1(env), io.a, io.b, mem };
}

auto softLoad(const Io& io, const bool isDouble) -> Soft
{
    auto       env = envFromControl(io.cw);
    const auto r   = isDouble ? fromDouble(io.mem, env) : fromFloat(static_cast<std::uint32_t>(io.mem), env);
    return Soft{ withC1(env), r, io.a, 0 };
}

// The interpreter's way to run an arithmetic instruction with a memory operand: convert, then operate, in one Env
auto softMemory(const Io& io, const int which) -> Soft
{
    auto          env = envFromControl(io.cw);
    F80           r   = io.a;
    std::uint16_t cc  = 0;
    switch (which)
    {
        case 0:
        {
            bool       denormal = false;
            const auto m        = floatOperand(static_cast<std::uint32_t>(io.mem), denormal);
            r                   = add(io.a, m, env);
            if (denormal)
            {
                denormalOperand(io.a, m, env);
            }
            break;
        }
        case 1:
        {
            bool       denormal = false;
            const auto m        = doubleOperand(io.mem, denormal);
            r                   = mul(io.a, m, env);
            if (denormal)
            {
                denormalOperand(io.a, m, env);
            }
            break;
        }
        case 2:
            r = div(io.a, fromInt64(static_cast<std::int32_t>(io.mem)), env);
            break;
        default:
            cc = orderCodes(compare(io.a, fromInt64(static_cast<std::int32_t>(io.mem)), false, env));
            break;
    }
    return Soft{ static_cast<std::uint16_t>(withC1(env) | cc), r, io.b, 0 };
}

auto softConstant(const Io& io, const std::uint8_t which) -> Soft
{
    auto       env = envFromControl(io.cw);
    const auto r   = constant(which, env);
    return Soft{ withC1(env), r, io.a, 0 };
}

auto softTrig(const Io& io, const int which) -> Soft
{
    auto env = envFromControl(io.cw);
    F80  r1  = io.a;
    F80  r2  = io.b;
    bool ok  = true;
    switch (which)
    {
        case 0:
            ok = sin(io.a, r1, env);
            break;
        case 1:
            ok = cos(io.a, r1, env);
            break;
        case 2: // FSINCOS leaves ST0 = cos, ST1 = sin
            ok = sinCos(io.a, r2, r1, env);
            if (!ok)
            {
                r1 = io.a;
                r2 = io.b;
            }
            break;
        default: // FPTAN leaves ST0 = 1, ST1 = tan
            ok = tan(io.a, r2, env);
            r1 = ok ? one() : io.a;
            if (!ok)
            {
                r2 = io.b;
            }
            break;
    }
    return Soft{ static_cast<std::uint16_t>(withC1(env) | (ok ? 0 : kC2)), r1, r2, 0 };
}

template <int Which>
auto softLog(const Io& io) -> Soft
{
    auto env = envFromControl(io.cw);
    F80  r;
    switch (Which)
    {
        case 0:
            r = atan2(io.b, io.a, env);
            break;
        case 1:
            r = exp2m1(io.a, env);
            break;
        case 2:
            r = yLog2X(io.a, io.b, env);
            break;
        default:
            r = yLog2Xp1(io.a, io.b, env);
            break;
    }
    return Soft{ withC1(env), r, io.b, 0 };
}

const Op kOps[] = {
    { "fadd", hostAdd, softBinary<add>, kC1, 1, Kind::Exact, 2 },
    { "fsub", hostSub, softBinary<sub>, kC1, 1, Kind::Exact, 2 },
    { "fmul", hostMul, softBinary<mul>, kC1, 1, Kind::Exact, 2 },
    { "fdiv", hostDiv, softBinary<div>, kC1, 1, Kind::Exact, 2 },
    { "fsqrt", hostSqrt, softUnary<sqrt>, kC1, 1, Kind::Exact, 1 },
    { "frndint", hostRndint, softUnary<roundToInt>, kC1, 1, Kind::Exact, 1 },
    { "fscale", hostScale, softBinary<scale>, kC1, 1, Kind::Exact, 2 },
    { "fprem", hostPrem, [](const Io& io)
      {
          return softPrem(io, false);
      },
      kC0 | kC1 | kC2 | kC3,
      1,
      Kind::Exact,
      2 },
    { "fprem1", hostPrem1, [](const Io& io)
      {
          return softPrem(io, true);
      },
      kC0 | kC1 | kC2 | kC3,
      1,
      Kind::Exact,
      2 },
    { "fxtract", hostXtract, softXtract, kC1, 2, Kind::Exact, 1 },
    { "fcom", hostCom, [](const Io& io)
      {
          return softCompare(io, false);
      },
      kC0 | kC1 | kC2 | kC3,
      1,
      Kind::Exact,
      2 },
    { "fucom", hostUcom, [](const Io& io)
      {
          return softCompare(io, true);
      },
      kC0 | kC1 | kC2 | kC3,
      1,
      Kind::Exact,
      2 },
    { "fxam", hostXam, [](const Io& io)
      {
          return Soft{ classCodes(io.a), io.a, io.b, 0 };
      },
      kC0 | kC1 | kC2 | kC3,
      1,
      Kind::Exact,
      1 },
    { "fst32", hostStF32, [](const Io& io)
      {
          return softStore(io, 0);
      },
      kC1,
      0,
      Kind::Exact,
      1 },
    { "fst64", hostStF64, [](const Io& io)
      {
          return softStore(io, 1);
      },
      kC1,
      0,
      Kind::Exact,
      1 },
    { "fist16", hostStI16, [](const Io& io)
      {
          return softStore(io, 2);
      },
      kC1,
      0,
      Kind::Exact,
      1 },
    { "fist32", hostStI32, [](const Io& io)
      {
          return softStore(io, 3);
      },
      kC1,
      0,
      Kind::Exact,
      1 },
    { "fist64", hostStI64, [](const Io& io)
      {
          return softStore(io, 4);
      },
      kC1,
      0,
      Kind::Exact,
      1 },
    { "fld32", hostLdF32, [](const Io& io)
      {
          return softLoad(io, false);
      },
      kC1,
      1,
      Kind::Exact,
      3 },
    { "fld64", hostLdF64, [](const Io& io)
      {
          return softLoad(io, true);
      },
      kC1,
      1,
      Kind::Exact,
      3 },
    { "fadd32", hostAddF32, [](const Io& io)
      {
          return softMemory(io, 0);
      },
      kC1,
      1,
      Kind::Exact,
      3 },
    { "fmul64", hostMulF64, [](const Io& io)
      {
          return softMemory(io, 1);
      },
      kC1,
      1,
      Kind::Exact,
      3 },
    { "fidiv32", hostDivI32, [](const Io& io)
      {
          return softMemory(io, 2);
      },
      kC1,
      1,
      Kind::Exact,
      3 },
    { "ficom32", hostComI32, [](const Io& io)
      {
          return softMemory(io, 3);
      },
      kC0 | kC1 | kC2 | kC3,
      1,
      Kind::Exact,
      3 },
    { "fld1", hostLd1, [](const Io& io)
      {
          return softConstant(io, 0);
      },
      kC1,
      1,
      Kind::Exact,
      1 },
    { "fldl2t", hostLdl2t, [](const Io& io)
      {
          return softConstant(io, 1);
      },
      kC1,
      1,
      Kind::Exact,
      1 },
    { "fldl2e", hostLdl2e, [](const Io& io)
      {
          return softConstant(io, 2);
      },
      kC1,
      1,
      Kind::Exact,
      1 },
    { "fldpi", hostLdpi, [](const Io& io)
      {
          return softConstant(io, 3);
      },
      kC1,
      1,
      Kind::Exact,
      1 },
    { "fldlg2", hostLdlg2, [](const Io& io)
      {
          return softConstant(io, 4);
      },
      kC1,
      1,
      Kind::Exact,
      1 },
    { "fldln2", hostLdln2, [](const Io& io)
      {
          return softConstant(io, 5);
      },
      kC1,
      1,
      Kind::Exact,
      1 },
    { "fsin", hostSin, [](const Io& io)
      {
          return softTrig(io, 0);
      },
      kC2,
      1,
      Kind::Transcendental,
      1 },
    { "fcos", hostCos, [](const Io& io)
      {
          return softTrig(io, 1);
      },
      kC2,
      1,
      Kind::Transcendental,
      1 },
    { "fsincos", hostSinCos, [](const Io& io)
      {
          return softTrig(io, 2);
      },
      kC2,
      2,
      Kind::Transcendental,
      1 },
    { "fptan", hostTan, [](const Io& io)
      {
          return softTrig(io, 3);
      },
      kC2,
      2,
      Kind::Transcendental,
      1 },
    { "fpatan", hostAtan, softLog<0>, 0, 1, Kind::Transcendental, 2 },
    { "f2xm1", hostF2xm1, softLog<1>, 0, 1, Kind::Transcendental, 1 },
    { "fyl2x", hostYl2x, softLog<2>, 0, 1, Kind::Transcendental, 2 },
    { "fyl2xp1", hostYl2xp1, softLog<3>, 0, 1, Kind::Transcendental, 2 },
};

auto same(const F80 a, const F80 b) -> bool
{
    return a.significand == b.significand && a.signExponent == b.signExponent;
}

// Distance in units in the last place of b; huge when the classes or signs differ
auto ulps(const F80 a, const F80 b) -> double
{
    if (same(a, b) || (classify(a) == Class::NaN && classify(b) == Class::NaN))
    {
        return 0;
    }
    const auto finite = [](const F80 v)
    {
        const auto c = classify(v);
        return c == Class::Normal || c == Class::Denormal || c == Class::Zero;
    };
    if (!finite(a) || !finite(b) || isNegative(a) != isNegative(b))
    {
        return 1e30;
    }
    auto       env      = Env{};
    const auto diff     = abs(sub(a, b, env));
    const auto exponent = std::max(exponentOf(b), 1);
    return toHostDouble(scale(diff, fromInt64(-(exponent - 0x3FFF - 63)), env));
}

void print(const char* label, const F80 v)
{
    std::printf(" %s=%04X:%016llX", label, v.signExponent, static_cast<unsigned long long>(v.significand));
}

} // namespace

int main(int argc, char** argv)
{
    const int         cases         = argc > 1 ? std::atoi(argv[1]) : 20000;
    const auto        seed          = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 1;
    const std::string only          = argc > 3 ? argv[3] : "";
    int               exactFailures = 0;
    for (const auto& op : kOps)
    {
        if (!only.empty() && only != op.name)
        {
            continue;
        }
        for (int pc = 0; pc < 4; ++pc)
        {
            if (pc == kReserved)
            {
                continue;
            }
            for (int rc = 0; rc < 4; ++rc)
            {
                auto      rng       = Random{ seed * 0x100 + pc * 4 + rc + std::hash<std::string>{}(op.name) };
                int       mismatch  = 0;
                int       flagDiffs = 0;
                int       exact = 0, oneUlp = 0, worse = 0;
                double    maxUlps = 0;
                const int shown   = 6;
                for (int i = 0; i < cases; ++i)
                {
                    Io io{};
                    io.cw = static_cast<std::uint16_t>(0x3F | 0x40 | (pc << 8) | (rc << 10));
                    io.a  = operand(rng, nullptr);
                    io.b  = operand(rng, rng.below(2) ? &io.a : nullptr);
                    if (op.operands == 3)
                    {
                        io.mem = memoryOperand(rng, std::strstr(op.name, "64") != nullptr);
                    }
                    else
                    {
                        io.mem = rng.next();
                    }
                    auto host = io;
                    op.host(&host);
                    const auto soft     = op.soft(io);
                    const auto mask     = static_cast<std::uint16_t>(0x3F | op.compared);
                    const auto hostSw   = static_cast<std::uint16_t>(host.sw & mask);
                    const auto softSw   = static_cast<std::uint16_t>(soft.sw & mask);
                    bool       resultOk = true;
                    if (op.results == 0)
                    {
                        resultOk = host.mem == soft.mem;
                    }
                    else
                    {
                        resultOk = same(host.r1, soft.r1) && (op.results < 2 || same(host.r2, soft.r2));
                    }
                    if (op.kind == Kind::Transcendental)
                    {
                        auto d = ulps(soft.r1, host.r1);
                        if (op.results == 2 && std::strcmp(op.name, "fptan") == 0)
                        {
                            d = ulps(soft.r2, host.r2);
                        }
                        else if (op.results == 2)
                        {
                            d = std::max(d, ulps(soft.r2, host.r2));
                        }
                        exact += d == 0;
                        oneUlp += d > 0 && d <= 1;
                        worse += d > 1;
                        if (d < 1e29)
                        {
                            maxUlps = std::max(maxUlps, d);
                        }
                        // Flags other than inexact and C1 must agree
                        if ((hostSw & ~(0x20 | kC1)) != (softSw & ~(0x20 | kC1)))
                        {
                            ++flagDiffs;
                        }
                        if ((d > 1 || (hostSw & ~(0x20 | kC1)) != (softSw & ~(0x20 | kC1))) && worse + flagDiffs <= shown)
                        {
                            std::printf("%s pc%d rc%d ulps=%g", op.name, pc, rc, d);
                            print("a", io.a);
                            print("b", io.b);
                            std::printf(" | host sw=%04X", hostSw);
                            print("r1", host.r1);
                            print("r2", host.r2);
                            std::printf(" | soft sw=%04X", softSw);
                            print("r1", soft.r1);
                            print("r2", soft.r2);
                            std::printf("\n");
                        }
                        continue;
                    }
                    if (resultOk && hostSw == softSw)
                    {
                        continue;
                    }
                    if (++mismatch <= shown)
                    {
                        std::printf("%s pc%d rc%d", op.name, pc, rc);
                        print("a", io.a);
                        print("b", io.b);
                        std::printf(" mem=%016llX | host sw=%04X", static_cast<unsigned long long>(io.mem), hostSw);
                        print("r1", host.r1);
                        print("r2", host.r2);
                        std::printf(" mem=%016llX | soft sw=%04X", static_cast<unsigned long long>(host.mem), softSw);
                        print("r1", soft.r1);
                        print("r2", soft.r2);
                        std::printf(" mem=%016llX\n", static_cast<unsigned long long>(soft.mem));
                    }
                }
                if (op.kind == Kind::Transcendental)
                {
                    std::printf("%-8s pc%d rc%d: %d exact, %d within 1 ulp, %d worse (max %.3g finite), %d flag diffs\n",
                                op.name,
                                pc,
                                rc,
                                exact,
                                oneUlp,
                                worse,
                                maxUlps,
                                flagDiffs);
                }
                else
                {
                    exactFailures += mismatch;
                    std::printf("%-8s pc%d rc%d: %d / %d differ\n", op.name, pc, rc, mismatch, cases);
                }
            }
        }
    }
    std::printf("%d exact-operation mismatches\n", exactFailures);
    return exactFailures == 0 ? 0 : 1;
}
