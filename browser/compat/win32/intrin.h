#pragma once

#include <xmmintrin.h>

#include "msvcrt.h"

// No x86 CPU underneath: CPUID reports nothing, so the client takes its baseline code paths.
inline void __cpuid(int registers[4], int)
{
    registers[0] = registers[1] = registers[2] = registers[3] = 0;
}

// EFLAGS reads back with no ID bit toggled, the pre-CPUID answer.
inline unsigned int __readeflags()
{
    return 0;
}

inline void __writeeflags(unsigned int)
{
}
