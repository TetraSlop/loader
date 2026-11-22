#pragma once

// The x86-32 user-mode interpreter. Flat segments except FS and GS, which have a base (FS points at the current
// thread's TEB). CPUID reports a Pentium-class CPU without MMX or SSE, so the game's CRT and D3DX take their x87
// paths. RDTSC counts retired instructions, so runs are deterministic.

#include "core/memory.h"
#include "f80/f80.h"

#include <array>
#include <cstdint>

namespace tmweb
{

enum Reg : std::uint8_t
{
    kEax,
    kEcx,
    kEdx,
    kEbx,
    kEsp,
    kEbp,
    kEsi,
    kEdi,
};

enum Flag : std::uint32_t
{
    kCF = 0x0001,
    kPF = 0x0004,
    kAF = 0x0010,
    kZF = 0x0040,
    kSF = 0x0080,
    kTF = 0x0100,
    kIF = 0x0200,
    kDF = 0x0400,
    kOF = 0x0800,
    kID = 0x200000,
};

struct X87
{
    std::array<f80::F80, 8> st{}; // physical registers R0..R7; ST(i) is st[(top + i) & 7]
    std::uint16_t           control         = 0x037F;
    std::uint16_t           status          = 0;      // top lives in bits 11..13
    std::uint16_t           tag             = 0xFFFF; // two bits per physical register, 3 = empty
    std::uint32_t           lastInstruction = 0;
    std::uint32_t           lastOperand     = 0;
    std::uint16_t           lastOpcode      = 0;

    auto top() const -> unsigned
    {
        return (status >> 11) & 7;
    }
};

// One guest thread's register state
struct Context
{
    std::array<std::uint32_t, 8> r{};
    std::uint32_t                eip    = 0;
    std::uint32_t                eflags = 0x202;
    std::uint32_t                fsBase = 0;
    std::uint32_t                gsBase = 0;
    X87                          fpu{};
};

// Why run() returned
enum class Stop : std::uint8_t
{
    Budget,    // the instruction budget ran out
    Trap,      // eip reached a stand-in entry point; the instruction there did not run
    Fault,     // an access violation; eip is the faulting instruction, state as before it
    Interrupt, // INT n, INT3, INTO, a divide error, an invalid opcode or a single-step trap
    Halt,      // HLT, or a stop the host requested
};

struct Event
{
    Stop          stop    = Stop::Budget;
    std::uint32_t address = 0; // eip at the stop; for Trap the entry point
    std::uint32_t vector  = 0; // Interrupt: 0 divide, 1 single step, 3 breakpoint, 4 overflow, 6 invalid opcode, n for INT n
    GuestFault    fault{};     // Fault: what was touched
};

// Entry points of host stand-ins: executing one stops the CPU before it decodes anything there. Addresses are slots
// of kSlotSize bytes in one contiguous range; the trap is at offset kTrapOffset of a slot (see hle/boundary.h).
struct TrapRange
{
    static constexpr std::uint32_t kSlotSize   = 32;
    static constexpr std::uint32_t kTrapOffset = 16;

    std::uint32_t base = 0;
    std::uint32_t size = 0;

    auto contains(const std::uint32_t eip) const -> bool
    {
        return eip - base < size && ((eip - base) & (kSlotSize - 1)) == kTrapOffset;
    }
};

class Cpu final
{
public:
    explicit Cpu(Memory& memory);

    // Run until budget instructions have retired or something stops it. A REP string instruction counts one per
    // iteration, as QEMU (and so Unicorn) does, so instruction counts line up in the differential test.
    auto run(Context& context, std::uint64_t budget) -> Event;
    auto step(Context& context) -> Event;

    void setTraps(TrapRange traps);
    auto retired() const -> std::uint64_t;

private:
    Memory&       memory_;
    TrapRange     traps_{};
    std::uint64_t retired_ = 0;
};

} // namespace tmweb
