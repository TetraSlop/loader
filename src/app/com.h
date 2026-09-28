#pragma once
#include <windows.h>

namespace tmloader
{

// Calls a stdcall COM method by its vtable slot.
template <typename... Args>
auto callMethod(void* object, unsigned slot, Args... args) -> HRESULT
{
    using Method = HRESULT(STDMETHODCALLTYPE*)(void*, Args...);
    return reinterpret_cast<Method>((*static_cast<void***>(object))[slot])(object, args...);
}

} // namespace tmloader
