#include "runtime.h"
#include <stdexcept>
namespace tmloader {
void replacePointer(HMODULE image, void *oldValue, void *newValue)
{
    BYTE *base = (BYTE *)image;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    DWORD size = nt->OptionalHeader.SizeOfImage;
    for (DWORD rva = 0x1000; rva + 4 < size;)
    {
        MEMORY_BASIC_INFORMATION mem = {};
        if (!VirtualQuery(base + rva, &mem, sizeof(mem)))
            break;
        DWORD end = min(size, (DWORD)((BYTE *)mem.BaseAddress + mem.RegionSize - base));
        if (end <= rva)
            break;
        if (mem.State == MEM_COMMIT && !(mem.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        {
            for (; rva + 4 <= end; rva += 4)
                if (*(void **)(base + rva) == oldValue)
                {
                    DWORD old = 0;
                    if (VirtualProtect(base + rva, 4, PAGE_EXECUTE_READWRITE, &old))
                    {
                        *(void **)(base + rva) = newValue;
                        VirtualProtect(base + rva, 4, old, &old);
                    }
                }
        }
        rva = end;
    }
}
void setSlot(HMODULE module, DWORD rva, void *function)
{
    DWORD old = 0;
    void **slot = (void **)((BYTE *)module + rva);
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old))
        throw std::runtime_error("Cannot make import slot writable");
    logf("Replace TM import +%X: %p -> %p", rva, *slot, function);
    *slot = function;
    VirtualProtect(slot, 4, old, &old);
}
}
