#pragma once
#include <windows.h>

#include <span>

namespace tmloader
{

// Replaces every aligned occurrence of a pointer in the module's committed memory.
void replacePointer(HMODULE module, void* oldValue, void* newValue);
// Replaces one import address table entry at a fixed RVA.
void setImport(HMODULE module, DWORD rva, void* function);
// Overwrites code at site; throws when the page cannot be made writable.
void writeCode(void* site, std::span<const BYTE> bytes);

// Stores a writable vtable entry in original and points it at replacement.
// Objects may share a vtable, so an entry that is already ours is left alone.
template <class T>
void hook(T& original, void*& entry, T replacement)
{
    if (entry != reinterpret_cast<void*>(replacement))
    {
        original = reinterpret_cast<T>(entry);
        entry    = reinterpret_cast<void*>(replacement);
    }
}

} // namespace tmloader
