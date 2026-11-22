#pragma once

// The x87 instructions (opcodes D8..DF) on top of the f80 soft float.

#include "core/cpu.h"

#include <cstdint>

namespace tmweb
{

struct X87Operand
{
    bool          memory  = false;
    std::uint32_t address = 0; // effective address, segment base included
    unsigned      rm      = 0; // ST(i) for register forms
    unsigned      reg     = 0; // the ModRM reg field: the operation
    std::uint8_t  modrm   = 0; // the whole ModRM byte, for the FPU opcode register
};

// Execute one x87 instruction whose ModRM was decoded; false for an encoding that is not an instruction.
// instructionAddress becomes the FPU instruction pointer that FNSTENV/FNSAVE report.
auto x87Execute(Memory& memory, Context& context, std::uint32_t opcode, const X87Operand& operand, std::uint32_t instructionAddress) -> bool;

} // namespace tmweb
