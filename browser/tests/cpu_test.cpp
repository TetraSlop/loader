// Instruction-level tests of the interpreter, from hand-assembled bytes. Expected values follow the Intel manual,
// and QEMU's choices where the manual leaves a flag undefined.
#include "core/cpu.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <vector>

using namespace tmweb;

namespace
{

constexpr std::uint32_t kCode  = layout::kHeap;
constexpr std::uint32_t kData  = layout::kHeap + 0x10000;
constexpr std::uint32_t kStack = layout::kHeap + 0x30000;

struct Rig
{
    Memory        memory;
    Cpu           cpu{ memory };
    Context       context;
    std::uint32_t end = 0;

    Rig()
    {
        memory.map(kCode, 0x10000);
        memory.map(kData, 0x10000);
        memory.map(kStack - 0x10000, 0x10000);
        context.r[kEsp] = kStack;
        context.eip     = kCode;
    }

    void code(std::initializer_list<std::uint8_t> bytes)
    {
        auto data = std::vector<std::uint8_t>(bytes);
        memory.write(kCode, data);
        end = kCode + static_cast<std::uint32_t>(data.size());
    }

    // Run until eip leaves the code or something stops the CPU
    auto run(const std::uint64_t limit = 10000) -> Event
    {
        for (auto count = std::uint64_t{}; count < limit; ++count)
        {
            if (context.eip == end)
            {
                return Event{ Stop::Budget, context.eip };
            }
            const auto event = cpu.step(context);
            if (event.stop != Stop::Budget)
            {
                return event;
            }
        }
        if (context.eip != end)
        {
            FAIL("the code did not finish");
        }
        return Event{ Stop::Budget, context.eip };
    }

    auto flags(const std::uint32_t mask) const -> std::uint32_t
    {
        return context.eflags & mask;
    }
};

} // namespace

TEST_CASE("ADD sets carry, overflow, sign, zero, parity and auxiliary carry")
{
    auto rig = Rig{};
    rig.code({ 0x01, 0xD8 }); // add eax, ebx
    rig.context.r[kEax] = 0x7FFFFFFF;
    rig.context.r[kEbx] = 1;
    rig.run();
    CHECK(rig.context.r[kEax] == 0x80000000);
    CHECK(rig.flags(kCF | kOF | kSF | kZF | kAF | kPF) == (kOF | kSF | kAF | kPF));

    rig.context.eip     = kCode;
    rig.context.r[kEax] = 0xFFFFFFFF;
    rig.context.r[kEbx] = 1;
    rig.run();
    CHECK(rig.context.r[kEax] == 0);
    CHECK(rig.flags(kCF | kOF | kSF | kZF) == (kCF | kZF));
}

TEST_CASE("SUB and CMP borrow; SBB and ADC take the carry")
{
    auto rig = Rig{};
    rig.code({
        0x29,
        0xD8, // sub eax, ebx      1 - 2 = -1, borrow
        0x19,
        0xC9, // sbb ecx, ecx      0 - 0 - 1 = -1, borrow
        0x11,
        0xD2, // adc edx, edx      CF still set: 5 + 5 + 1
        0x83,
        0xFE,
        0x05, // cmp esi, 5
    });
    rig.context.r[kEax] = 1;
    rig.context.r[kEbx] = 2;
    rig.context.r[kEdx] = 5;
    rig.context.r[kEsi] = 5;
    rig.run();
    CHECK(rig.context.r[kEax] == 0xFFFFFFFF);
    CHECK(rig.context.r[kEcx] == 0xFFFFFFFF);
    CHECK(rig.context.r[kEdx] == 11);
    CHECK(rig.flags(kZF | kCF) == kZF);
}

TEST_CASE("INC and DEC keep the carry flag")
{
    auto rig = Rig{};
    rig.code({ 0xF9, 0x40, 0x4B }); // stc; inc eax; dec ebx
    rig.context.r[kEax] = 0x7FFFFFFF;
    rig.context.r[kEbx] = 1;
    rig.run();
    CHECK(rig.context.r[kEax] == 0x80000000);
    CHECK(rig.context.r[kEbx] == 0);
    CHECK(rig.flags(kCF | kZF | kOF) == (kCF | kZF));
}

TEST_CASE("Byte registers address AH..BH")
{
    auto rig = Rig{};
    rig.code({ 0xB4, 0x12, 0x88, 0xE3, 0x00, 0xE7 }); // mov ah, 12h; mov bl, ah; add bh, ah
    rig.context.r[kEbx] = 0x00000100;
    rig.run();
    CHECK(rig.context.r[kEax] == 0x00001200);
    CHECK(rig.context.r[kEbx] == 0x00001312);
}

TEST_CASE("Shifts: carry is the last bit out, OF follows QEMU for every count")
{
    auto rig = Rig{};
    rig.code({
        0xD1,
        0xE0, // shl eax, 1
        0xC1,
        0xEB,
        0x04, // shr ebx, 4
        0xC1,
        0xF9,
        0x1F, // sar ecx, 31
    });
    rig.context.r[kEax] = 0xC0000001;
    rig.context.r[kEbx] = 0x0000001F;
    rig.context.r[kEcx] = 0x80000000;
    rig.run();
    CHECK(rig.context.r[kEax] == 0x80000002);
    CHECK(rig.context.r[kEbx] == 1);
    CHECK(rig.context.r[kEcx] == 0xFFFFFFFF);
    CHECK(rig.flags(kCF) == 0); // SAR by 31 of 0x80000000: the last bit out is 0
}

TEST_CASE("A shift by zero changes neither the operand nor the flags")
{
    auto rig = Rig{};
    rig.code({ 0xF9, 0xD3, 0xE0 }); // stc; shl eax, cl with cl = 0
    rig.context.r[kEax] = 5;
    rig.run();
    CHECK(rig.context.r[kEax] == 5);
    CHECK(rig.flags(kCF) == kCF);
}

TEST_CASE("Rotates through carry and without")
{
    auto rig = Rig{};
    rig.code({
        0xD0,
        0xC0, // rol al, 1
        0xD0,
        0xD3, // rcl bl, 1
        0xC0,
        0xC9,
        0x04, // ror cl, 4
        0xD0,
        0xDA, // rcr dl, 1
    });
    rig.context.r[kEax] = 0x81;
    rig.context.r[kEbx] = 0x80;
    rig.context.r[kEcx] = 0x12;
    rig.context.r[kEdx] = 0x01;
    rig.run();
    CHECK((rig.context.r[kEax] & 0xFF) == 0x03);
    CHECK((rig.context.r[kEbx] & 0xFF) == 0x01); // CF from ROL was 1, goes into bit 0; the old bit 7 goes to CF
    CHECK((rig.context.r[kEcx] & 0xFF) == 0x21);
    CHECK((rig.context.r[kEdx] & 0xFF) == 0x00); // ROR left CF = 0, which RCR shifts in; bit 0 goes to CF
    CHECK(rig.flags(kCF) == kCF);
}

TEST_CASE("MUL, IMUL, DIV and IDIV on EDX:EAX")
{
    auto rig = Rig{};
    rig.code({
        0xF7,
        0xE3, // mul ebx
        0x89,
        0xC6, // mov esi, eax
        0x89,
        0xD7, // mov edi, edx
        0xB8,
        0xF9,
        0xFF,
        0xFF,
        0xFF, // mov eax, -7
        0x99, // cdq
        0xB9,
        0x02,
        0x00,
        0x00,
        0x00, // mov ecx, 2
        0xF7,
        0xF9, // idiv ecx: -3 remainder -1
        0x6B,
        0xED,
        0xFD, // imul ebp, ebp, -3
    });
    rig.context.r[kEax] = 0x80000000;
    rig.context.r[kEbx] = 4;
    rig.context.r[kEbp] = 5;
    rig.run();
    CHECK(rig.context.r[kEsi] == 0);
    CHECK(rig.context.r[kEdi] == 2);
    CHECK(rig.context.r[kEax] == 0xFFFFFFFD);
    CHECK(rig.context.r[kEdx] == 0xFFFFFFFF);
    CHECK(rig.context.r[kEbp] == 0xFFFFFFF1);
}

TEST_CASE("Division by zero is a divide error with the state as before the instruction")
{
    auto rig = Rig{};
    rig.code({ 0x40, 0xF7, 0xF1 }); // inc eax; div ecx
    rig.context.r[kEax] = 9;
    const auto event    = rig.run();
    CHECK(event.stop == Stop::Interrupt);
    CHECK(event.vector == 0);
    CHECK(event.address == kCode + 1);
    CHECK(rig.context.eip == kCode + 1);
    CHECK(rig.context.r[kEax] == 10);
}

TEST_CASE("A faulting instruction leaves registers and eip unchanged")
{
    auto rig = Rig{};
    rig.code({ 0x31, 0xC0, 0x50, 0x31, 0x00 }); // xor eax, eax; push eax; xor [eax], eax
    const auto esp   = rig.context.r[kEsp];
    const auto event = rig.run();
    CHECK(event.stop == Stop::Fault);
    CHECK(event.address == kCode + 3);
    CHECK(event.fault.address == 0);
    CHECK(event.fault.access == Access::Read);
    CHECK(rig.context.eip == kCode + 3);
    CHECK(rig.context.r[kEsp] == esp - 4);
}

TEST_CASE("PUSH ESP pushes the old value; POP ESP and PUSHAD/POPAD")
{
    auto rig = Rig{};
    rig.code({ 0x54, 0x5B, 0x60, 0x31, 0xC0, 0x61 }); // push esp; pop ebx; pushad; xor eax, eax; popad
    rig.context.r[kEax] = 0x1234;
    const auto esp      = rig.context.r[kEsp];
    rig.run();
    CHECK(rig.context.r[kEbx] == esp);
    CHECK(rig.context.r[kEax] == 0x1234);
    CHECK(rig.context.r[kEsp] == esp);
}

TEST_CASE("CALL, RET imm16 and indirect calls through memory")
{
    auto rig = Rig{};
    // call +5; jmp end; (sub:) mov eax, [esp+4]; ret 4 -- after pushing an argument
    rig.code({
        0x6A,
        0x2A, // push 42
        0xE8,
        0x02,
        0x00,
        0x00,
        0x00, // call sub
        0xEB,
        0x07, // jmp end
        0x8B,
        0x44,
        0x24,
        0x04, // sub: mov eax, [esp+4]
        0xC2,
        0x04,
        0x00, // ret 4
    });
    rig.end        = kCode + 16;
    const auto esp = rig.context.r[kEsp];
    rig.run();
    CHECK(rig.context.r[kEax] == 42);
    CHECK(rig.context.r[kEsp] == esp);
}

TEST_CASE("SIB addressing with scale, index and displacement")
{
    auto rig = Rig{};
    rig.code({ 0x8D, 0x44, 0x8B, 0x10 }); // lea eax, [ebx + ecx*4 + 10h]
    rig.context.r[kEbx] = 0x1000;
    rig.context.r[kEcx] = 3;
    rig.run();
    CHECK(rig.context.r[kEax] == 0x101C);
}

TEST_CASE("FS-relative loads use the FS base")
{
    auto rig = Rig{};
    rig.memory.write<std::uint32_t>(kData + 0x30, 0xCAFEF00D);
    rig.code({ 0x64, 0xA1, 0x30, 0x00, 0x00, 0x00 }); // mov eax, fs:[30h]
    rig.context.fsBase = kData;
    rig.run();
    CHECK(rig.context.r[kEax] == 0xCAFEF00D);
}

TEST_CASE("REP MOVSD steps once per iteration and REPNE SCASB stops on a match")
{
    auto rig = Rig{};
    rig.memory.writeString(kData, "abcdefgh");
    rig.code({ 0xF3, 0xA5, 0xF2, 0xAE }); // rep movsd; repne scasb
    rig.context.r[kEsi] = kData;
    rig.context.r[kEdi] = kData + 0x100;
    rig.context.r[kEcx] = 2;
    auto steps          = 0;
    while (rig.context.eip == kCode)
    {
        rig.cpu.step(rig.context);
        ++steps;
    }
    CHECK(steps == 2);
    CHECK(rig.memory.readString(kData + 0x100) == "abcdefgh");
    rig.context.r[kEdi] = kData;
    rig.context.r[kEcx] = 100;
    rig.context.r[kEax] = 'd';
    rig.run();
    CHECK(rig.context.r[kEdi] == kData + 4);
    CHECK(rig.context.r[kEcx] == 96);
    CHECK(rig.flags(kZF) == kZF);
}

TEST_CASE("REP with ECX = 0 does nothing and moves on")
{
    auto rig = Rig{};
    rig.code({ 0xF3, 0xAB }); // rep stosd
    rig.context.r[kEdi] = kData;
    rig.run(1);
    CHECK(rig.context.eip == kCode + 2);
    CHECK(rig.memory.read<std::uint32_t>(kData) == 0);
}

TEST_CASE("LOOP, JECXZ and Jcc")
{
    auto rig = Rig{};
    rig.code({
        0x40, // l: inc eax
        0xE2,
        0xFD, // loop l
        0xE3,
        0x01, // jecxz +1
        0x40, // inc eax (skipped)
        0x3C,
        0x05, // cmp al, 5
        0x7C,
        0x01, // jl +1 (not taken)
        0x43, // inc ebx
    });
    rig.context.r[kEcx] = 5;
    rig.run();
    CHECK(rig.context.r[kEax] == 5);
    CHECK(rig.context.r[kEbx] == 1);
}

TEST_CASE("MOVZX, MOVSX, BSWAP, SETcc and CMOVcc")
{
    auto rig = Rig{};
    rig.code({
        0x0F,
        0xB6,
        0xC3, // movzx eax, bl
        0x0F,
        0xBF,
        0xCB, // movsx ecx, bx
        0x0F,
        0xCA, // bswap edx
        0x39,
        0xC0, // cmp eax, eax
        0x0F,
        0x94,
        0xC6, // sete dh
        0x0F,
        0x45,
        0xF1, // cmovne esi, ecx (not taken)
        0x0F,
        0x44,
        0xF9, // cmove edi, ecx
    });
    rig.context.r[kEbx] = 0x8081;
    rig.context.r[kEdx] = 0x11223344;
    rig.run();
    CHECK(rig.context.r[kEax] == 0x81);
    CHECK(rig.context.r[kEcx] == 0xFFFF8081);
    CHECK(rig.context.r[kEdx] == 0x44330111); // bswap, then SETE writes DH
    CHECK(rig.context.r[kEsi] == 0);
    CHECK(rig.context.r[kEdi] == 0xFFFF8081);
}

TEST_CASE("CMPXCHG, XADD and CMPXCHG8B")
{
    auto rig = Rig{};
    rig.memory.write<std::uint32_t>(kData, 7);
    rig.memory.write<std::uint64_t>(kData + 8, 0x1111111122222222);
    rig.code({
        0x0F,
        0xB1,
        0x0B, // cmpxchg [ebx], ecx: eax 7 matches
        0x0F,
        0xC1,
        0x13, // xadd [ebx], edx
        0x0F,
        0xC7,
        0x4B,
        0x08, // cmpxchg8b [ebx+8]: edx:eax mismatch, loads them
    });
    rig.context.r[kEbx] = kData;
    rig.context.r[kEax] = 7;
    rig.context.r[kEcx] = 100;
    rig.context.r[kEdx] = 5;
    rig.run();
    CHECK(rig.memory.read<std::uint32_t>(kData) == 105);
    CHECK(rig.context.r[kEdx] == 0x11111111);
    CHECK(rig.context.r[kEax] == 0x22222222);
    CHECK(rig.flags(kZF) == 0);
}

TEST_CASE("BT with a register offset reaches beyond the operand in memory")
{
    auto rig = Rig{};
    rig.memory.write<std::uint32_t>(kData + 4, 0x2);
    rig.code({ 0x0F, 0xAB, 0x03 }); // bts [ebx], eax with eax = 33
    rig.context.r[kEbx] = kData;
    rig.context.r[kEax] = 33;
    rig.run();
    CHECK(rig.flags(kCF) == kCF);
    rig.context.eip     = kCode;
    rig.context.r[kEax] = 32;
    rig.run();
    CHECK(rig.memory.read<std::uint32_t>(kData + 4) == 0x3);
    CHECK(rig.flags(kCF) == 0);
}

TEST_CASE("SHLD and SHRD")
{
    auto rig = Rig{};
    rig.code({ 0x0F, 0xA4, 0xD8, 0x08, 0x0F, 0xAD, 0xD1 }); // shld eax, ebx, 8; shrd ecx, edx, cl
    rig.context.r[kEax] = 0x11223344;
    rig.context.r[kEbx] = 0xAABBCCDD;
    rig.context.r[kEcx] = 4;
    rig.context.r[kEdx] = 0xF;
    rig.run();
    CHECK(rig.context.r[kEax] == 0x223344AA);
    CHECK(rig.context.r[kEcx] == 0xF0000000);
}

TEST_CASE("Operand-size prefix works on 16-bit halves")
{
    auto rig = Rig{};
    rig.code({ 0x66, 0x05, 0x01, 0x00, 0x66, 0x40 }); // add ax, 1; inc ax
    rig.context.r[kEax] = 0x1234FFFF;
    rig.run();
    CHECK(rig.context.r[kEax] == 0x12340001);
}

TEST_CASE("ENTER, LEAVE and XLAT")
{
    auto rig = Rig{};
    rig.memory.write<std::uint8_t>(kData + 3, 0x77);
    rig.code({ 0xC8, 0x10, 0x00, 0x00, 0xC9, 0xD7 }); // enter 10h, 0; leave; xlat
    rig.context.r[kEbp] = 0x55;
    rig.context.r[kEbx] = kData;
    rig.context.r[kEax] = 3;
    const auto esp      = rig.context.r[kEsp];
    rig.run();
    CHECK(rig.context.r[kEsp] == esp);
    CHECK(rig.context.r[kEbp] == 0x55);
    CHECK(rig.context.r[kEax] == 0x77);
}

TEST_CASE("DAA adjusts packed BCD")
{
    auto rig = Rig{};
    rig.code({ 0x04, 0x38, 0x27 }); // add al, 38h; daa (with al = 29h)
    rig.context.r[kEax] = 0x29;
    rig.run();
    CHECK((rig.context.r[kEax] & 0xFF) == 0x67);
}

TEST_CASE("CPUID reports GenuineIntel without MMX or SSE")
{
    auto rig = Rig{};
    rig.code({ 0x31, 0xC0, 0x0F, 0xA2, 0x89, 0xDE, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x0F, 0xA2 });
    rig.run();
    CHECK(rig.context.r[kEsi] == 0x756E6547);
    CHECK((rig.context.r[kEdx] & (1u << 23)) == 0); // MMX
    CHECK((rig.context.r[kEdx] & (1u << 25)) == 0); // SSE
    CHECK((rig.context.r[kEdx] & 1u) == 1);         // FPU
}

TEST_CASE("INT3 stops with vector 3 and eip past it; INT 2Dh carries its vector")
{
    auto rig = Rig{};
    rig.code({ 0xCC, 0xCD, 0x2D });
    auto event = rig.run();
    CHECK(event.stop == Stop::Interrupt);
    CHECK(event.vector == 3);
    CHECK(event.address == kCode);
    CHECK(rig.context.eip == kCode + 1);
    event = rig.run();
    CHECK(event.vector == 0x2D);
    CHECK(rig.context.eip == kCode + 3);
}

TEST_CASE("Privileged and invalid instructions stop before they run")
{
    auto rig = Rig{};
    rig.code({ 0xFA });
    auto event = rig.run();
    CHECK(event.stop == Stop::Interrupt);
    CHECK(event.vector == 13);
    CHECK(rig.context.eip == kCode);
    rig.code({ 0x0F, 0x0B });
    event = rig.run();
    CHECK(event.vector == 6);
}

TEST_CASE("POPF with TF set traps after the next instruction")
{
    auto rig = Rig{};
    rig.code({ 0x68, 0x02, 0x01, 0x00, 0x00, 0x9D, 0x40, 0x40 }); // push 102h; popf; inc eax; inc eax
    rig.cpu.step(rig.context);
    rig.cpu.step(rig.context);
    const auto event = rig.cpu.step(rig.context);
    CHECK(event.stop == Stop::Interrupt);
    CHECK(event.vector == 1);
    CHECK(rig.context.r[kEax] == 1);
    CHECK(rig.context.eip == kCode + 7);
}

TEST_CASE("Execution stops at a trap slot before decoding it")
{
    auto rig = Rig{};
    rig.memory.map(layout::kModules, 0x1000);
    rig.cpu.setTraps(TrapRange{ layout::kModules, 0x1000 });
    rig.code({ 0xB8, 0x10, 0x00, 0xA0, 0x10, 0xFF, 0xE0 }); // mov eax, 10A00010h; jmp eax
    const auto event = rig.run();
    CHECK(event.stop == Stop::Trap);
    CHECK(event.address == layout::kModules + TrapRange::kTrapOffset);
}

TEST_CASE("Retired instructions drive RDTSC")
{
    auto rig = Rig{};
    rig.code({ 0x90, 0x90, 0x0F, 0x31 });
    rig.run();
    CHECK(rig.context.r[kEax] == 2);
    CHECK(rig.context.r[kEdx] == 0);
}
