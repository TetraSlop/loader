#include "app/runtime.h"
#include <algorithm>
#include <stdexcept>

namespace tmloader
{

void replacePointer(HMODULE image, void* oldValue, void* newValue)
{
    auto* const       base = reinterpret_cast<BYTE*>(image);
    const auto* const dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* const nt   = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto        size = nt->OptionalHeader.SizeOfImage;
    for (DWORD rva = 0x1000; rva + 4 < size;)
    {
        MEMORY_BASIC_INFORMATION mem = {};
        if (!VirtualQuery(base + rva, &mem, sizeof(mem)))
            break;
        const auto regionEnd = static_cast<BYTE*>(mem.BaseAddress) + mem.RegionSize;
        const auto end       = (std::min)(size, static_cast<DWORD>(regionEnd - base));
        if (end <= rva)
            break;
        if (mem.State == MEM_COMMIT && !(mem.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        {
            for (; rva + 4 <= end; rva += 4)
                if (*reinterpret_cast<void**>(base + rva) == oldValue)
                {
                    DWORD old = 0;
                    if (VirtualProtect(base + rva, 4, PAGE_EXECUTE_READWRITE, &old))
                    {
                        *reinterpret_cast<void**>(base + rva) = newValue;
                        VirtualProtect(base + rva, 4, old, &old);
                    }
                }
        }
        rva = end;
    }
}

void setSlot(HMODULE module, DWORD rva, void* function)
{
    DWORD       old  = 0;
    auto* const slot = reinterpret_cast<void**>(reinterpret_cast<BYTE*>(module) + rva);
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old))
        throw std::runtime_error("Cannot make import slot writable");
    logf("Replace TM import +%X: %p -> %p", rva, *slot, function);
    *slot = function;
    VirtualProtect(slot, 4, old, &old);
}

} // namespace tmloader
