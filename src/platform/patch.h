#pragma once
#include <windows.h>

namespace tmloader
{

// Replaces every aligned occurrence of a pointer in the module's committed memory.
void replacePointer(HMODULE module, void* oldValue, void* newValue);
// Replaces one import address table entry at a fixed RVA.
void setImport(HMODULE module, DWORD rva, void* function);

} // namespace tmloader
