#pragma once
#include <windows.h>

namespace tmloader
{

// Replaces every aligned occurrence of a pointer in the module's committed memory.
void replacePointer(HMODULE module, void* oldValue, void* newValue);
// Replaces one import address table entry at a fixed RVA.
void setImport(HMODULE module, DWORD rva, void* function);
// Points the relative CALL at callRva to replacement after checking it currently
// calls targetRva. Returns the original target; throws when the code differs.
auto redirectCall(HMODULE module, DWORD callRva, DWORD targetRva, void* replacement) -> void*;

} // namespace tmloader
