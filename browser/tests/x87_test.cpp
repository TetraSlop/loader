// x87 instructions through the interpreter, from hand-assembled bytes: the operand-order encodings (D8 E0+i is
// ST0 = ST0 - ST(i), DC E0+i is ST(i) = ST0 - ST(i)), precision and rounding control, compares, and the stack.
#include "core/cpu.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <initializer_list>
#include <vector>

using namespace tmweb;

namespace
{

constexpr std::uint32_t kCode  = layout::kHeap;
constexpr std::uint32_t kData  = layout::kHeap + 0x10000;
constexpr std::uint32_t kStack = layout::kHeap + 0x30000;

// Data slots, by EBX offset
enum Slot : std::uint32_t
{
    kA       = 0x00, // double 10
    kB       = 0x08, // double 4
    kOut     = 0x10, // double result
    kFloat   = 0x18, // float 1.5
    kControl = 0x20, // control word
    kThree   = 0x28, // double 3
    kInt     = 0x30, // int32 result
    kValue   = 0x38, // double, per test
};

struct Rig
{
    Memory  memory;
    Cpu     cpu{ memory };
    Context context;

    Rig()
    {
        memory.map(kCode, 0x10000);
        memory.map(kData, 0x10000);
        memory.map(kStack - 0x10000, 0x10000);
        context.r[kEsp] = kStack;
        context.r[kEbx] = kData;
        setDouble(kA, 10.0);
        setDouble(kB, 4.0);
        setDouble(kThree, 3.0);
        memory.write<std::uint32_t>(kData + kFloat, std::bit_cast<std::uint32_t>(1.5f));
    }

    void setDouble(const std::uint32_t slot, const double value)
    {
        memory.write<std::uint64_t>(kData + slot, std::bit_cast<std::uint64_t>(value));
    }

    auto getDouble(const std::uint32_t slot) const -> double
    {
        return std::bit_cast<double>(memory.read<std::uint64_t>(kData + slot));
    }

    // Load A then B (ST0 = 4, ST1 = 10), run the middle, store ST0 to kOut
    auto withAb(std::initializer_list<std::uint8_t> middle) -> double
    {
        auto code = std::vector<std::uint8_t>{ 0xDD, 0x03, 0xDD, 0x43, kB };
        code.insert(code.end(), middle);
        code.insert(code.end(), { 0xDD, 0x5B, kOut });
        return run(code);
    }

    auto run(const std::vector<std::uint8_t>& code) -> double
    {
        memory.write(kCode, code);
        context.eip    = kCode;
        const auto end = kCode + static_cast<std::uint32_t>(code.size());
        while (context.eip != end)
        {
            const auto event = cpu.step(context);
            REQUIRE(event.stop == Stop::Budget);
        }
        return getDouble(kOut);
    }
};

} // namespace

TEST_CASE("D8 E0+i is ST0 = ST0 - ST(i); D8 E8+i is ST0 = ST(i) - ST0")
{
    CHECK(Rig{}.withAb({ 0xD8, 0xE1 }) == -6.0);
    CHECK(Rig{}.withAb({ 0xD8, 0xE9 }) == 6.0);
    CHECK(Rig{}.withAb({ 0xD8, 0xF1 }) == 0.4); // FDIV ST0, ST1: 4 / 10
    CHECK(Rig{}.withAb({ 0xD8, 0xF9 }) == 2.5); // FDIVR ST0, ST1: 10 / 4
}

TEST_CASE("DC E8+i is ST(i) = ST(i) - ST0; DC E0+i is ST(i) = ST0 - ST(i)")
{
    // After the operation, FSTP pops ST0 (4) and the next FSTP reads ST1
    CHECK(Rig{}.withAb({ 0xDC, 0xE9, 0xDD, 0xD8 }) == 6.0);  // fsub st(1), st0; fstp st(0)
    CHECK(Rig{}.withAb({ 0xDC, 0xE1, 0xDD, 0xD8 }) == -6.0); // fsubr st(1), st0
    CHECK(Rig{}.withAb({ 0xDC, 0xF9, 0xDD, 0xD8 }) == 2.5);  // fdiv st(1), st0: 10 / 4
    CHECK(Rig{}.withAb({ 0xDC, 0xF1, 0xDD, 0xD8 }) == 0.4);  // fdivr st(1), st0: 4 / 10
}

TEST_CASE("The popping forms DE E8+i (FSUBP) and DE E0+i (FSUBRP) follow DC")
{
    CHECK(Rig{}.withAb({ 0xDE, 0xE9 }) == 6.0);
    CHECK(Rig{}.withAb({ 0xDE, 0xE1 }) == -6.0);
    CHECK(Rig{}.withAb({ 0xDE, 0xF9 }) == 2.5);
    CHECK(Rig{}.withAb({ 0xDE, 0xF1 }) == 0.4);
    CHECK(Rig{}.withAb({ 0xDE, 0xC1 }) == 14.0); // faddp
    CHECK(Rig{}.withAb({ 0xDE, 0xC9 }) == 40.0); // fmulp
}

TEST_CASE("Memory operands: D8 /4 is ST0 - m32, D8 /5 is m32 - ST0")
{
    CHECK(Rig{}.withAb({ 0xD8, 0x63, kFloat }) == 2.5);
    CHECK(Rig{}.withAb({ 0xD8, 0x6B, kFloat }) == -2.5);
    CHECK(Rig{}.withAb({ 0xDC, 0x73, kThree }) == 4.0 / 3.0); // fdiv qword
}

TEST_CASE("FXCH, FLD ST(i) and FSTP ST(i) move the stack as documented")
{
    CHECK(Rig{}.withAb({ 0xD9, 0xC9 }) == 10.0);                        // fxch st1
    CHECK(Rig{}.withAb({ 0xD9, 0xC1 }) == 10.0);                        // fld st1
    CHECK(Rig{}.withAb({ 0xDD, 0xD9, 0xD9, 0xEE, 0xDE, 0xC1 }) == 4.0); // fstp st1 (ST1 = 4, pop); fldz; faddp
}

TEST_CASE("Precision control rounds each result to 24, 53 or 64 bits")
{
    const auto third = [](const std::uint16_t control)
    {
        auto rig = Rig{};
        rig.memory.write<std::uint16_t>(kData + kControl, control);
        // fldcw [ebx+20h]; fld1; fdiv qword [ebx+28h]; fstp qword [ebx+10h]
        return rig.run({ 0xD9, 0x6B, kControl, 0xD9, 0xE8, 0xDC, 0x73, kThree, 0xDD, 0x5B, kOut });
    };
    CHECK(third(0x007F) == static_cast<double>(1.0f / 3.0f));
    CHECK(third(0x027F) == 1.0 / 3.0);
    CHECK(third(0x037F) == 1.0 / 3.0);
    CHECK(third(0x007F) != third(0x027F));
}

TEST_CASE("FISTP rounds by the rounding control: nearest even, or chop as _ftol sets")
{
    const auto store = [](const double value, const std::uint16_t control)
    {
        auto rig = Rig{};
        rig.setDouble(kValue, value);
        rig.memory.write<std::uint16_t>(kData + kControl, control);
        // fldcw; fld qword [ebx+38h]; fistp dword [ebx+30h]
        rig.run({ 0xD9, 0x6B, kControl, 0xDD, 0x43, kValue, 0xDB, 0x5B, kInt });
        return rig.memory.read<std::int32_t>(kData + kInt);
    };
    CHECK(store(2.75, 0x037F) == 3);
    CHECK(store(2.75, 0x0F7F) == 2);
    CHECK(store(-2.5, 0x037F) == -2);
    CHECK(store(-2.75, 0x0F7F) == -2);
    CHECK(store(-2.25, 0x077F) == -3); // round down
    CHECK(store(2.25, 0x0B7F) == 3);   // round up
}

TEST_CASE("FCOM sets C3 C2 C0; FNSTSW AX and SAHF carry them to the flags; FCOMIP sets them directly")
{
    auto rig = Rig{};
    // fld [B] (4); fld [A] (10): ST0 = 10, ST1 = 4. fcom st1; fnstsw ax
    rig.run({ 0xDD, 0x43, kB, 0xDD, 0x03, 0xD8, 0xD1, 0xDF, 0xE0 });
    CHECK((rig.context.r[kEax] & 0x4500) == 0); // greater
    // fxch; fcom st1 (4 vs 10: less); fnstsw ax
    rig.run({ 0xD9, 0xC9, 0xD8, 0xD1, 0xDF, 0xE0, 0x9E });
    CHECK((rig.context.r[kEax] & 0x4500) == 0x0100);
    CHECK((rig.context.eflags & kCF) == kCF);
    // fcomip st1: 4 vs 10, CF set, ZF clear, then the stack has one value
    rig.run({ 0xDF, 0xF1 });
    CHECK((rig.context.eflags & (kCF | kZF | kPF)) == kCF);
    CHECK(rig.context.fpu.top() == 7);
}

TEST_CASE("FTST and FXAM classify ST0")
{
    auto rig = Rig{};
    // fldz; ftst; fnstsw ax
    rig.run({ 0xD9, 0xEE, 0xD9, 0xE4, 0xDF, 0xE0 });
    CHECK((rig.context.r[kEax] & 0x4500) == 0x4000);
    // fchs (-0); fxam; fnstsw ax: zero class (C3) with the sign in C1
    rig.run({ 0xD9, 0xE0, 0xD9, 0xE5, 0xDF, 0xE0 });
    CHECK((rig.context.r[kEax] & 0x4700) == 0x4200);
}

TEST_CASE("FNSTENV reports the last FPU instruction's address")
{
    auto rig = Rig{};
    // The shellcode way to find eip: fldz; fnstenv [esp-1Ch]; mov eax, [esp-10h]
    rig.run({ 0xD9, 0xEE, 0xD9, 0x74, 0x24, 0xE4, 0x8B, 0x44, 0x24, 0xF0 });
    CHECK(rig.context.r[kEax] == kCode);
}

TEST_CASE("A ninth push overflows the stack: invalid, stack fault, C1 and the indefinite NaN")
{
    auto rig  = Rig{};
    auto code = std::vector<std::uint8_t>{};
    for (auto index = 0; index < 9; ++index)
    {
        code.insert(code.end(), { 0xD9, 0xE8 }); // fld1
    }
    code.insert(code.end(), { 0xDF, 0xE0 }); // fnstsw ax
    rig.run(code);
    CHECK((rig.context.r[kEax] & 0x0241) == 0x0241);
    const auto& st0 = rig.context.fpu.st[rig.context.fpu.top()];
    CHECK(st0.signExponent == 0xFFFF);
    CHECK(st0.significand == 0xC000000000000000ULL);
}

TEST_CASE("FILD and FISTP round-trip 16, 32 and 64-bit integers")
{
    auto rig = Rig{};
    rig.memory.write<std::int64_t>(kData + kValue, -1234567890123LL);
    // fild qword [ebx+38h]; fistp qword [ebx+10h]
    rig.run({ 0xDF, 0x6B, kValue, 0xDF, 0x7B, kOut });
    CHECK(rig.memory.read<std::int64_t>(kData + kOut) == -1234567890123LL);
    rig.memory.write<std::int16_t>(kData + kValue, -300);
    // fild word [ebx+38h]; fistp dword [ebx+30h]
    rig.run({ 0xDF, 0x43, kValue, 0xDB, 0x5B, kInt });
    CHECK(rig.memory.read<std::int32_t>(kData + kInt) == -300);
}

TEST_CASE("FSQRT and FSIN run on the soft float")
{
    auto rig = Rig{};
    rig.setDouble(kValue, 2.0);
    // fld qword [ebx+38h]; fsqrt; fstp qword [ebx+10h]
    CHECK(rig.run({ 0xDD, 0x43, kValue, 0xD9, 0xFA, 0xDD, 0x5B, kOut }) == std::sqrt(2.0));
    rig.setDouble(kValue, 0.5);
    const auto sine = rig.run({ 0xDD, 0x43, kValue, 0xD9, 0xFE, 0xDD, 0x5B, kOut });
    CHECK(std::fabs(sine - std::sin(0.5)) < 1e-15);
}
