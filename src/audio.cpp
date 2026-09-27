#include "runtime.h"
#include <dsound.h>

namespace tmloader {
HRESULT WINAPI tracedSound(LPCGUID guid, LPDIRECTSOUND* output, LPUNKNOWN outer) {
    using Factory = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);
    static auto factory = reinterpret_cast<Factory>(GetProcAddress(LoadLibraryW(L"dsound.dll"), "DirectSoundCreate"));
    HRESULT result = factory ? factory(guid, output, outer) : E_NOINTERFACE;
    logf("DirectSoundCreate=%08lX", result);
    return result;
}
}
