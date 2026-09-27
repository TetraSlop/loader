#include "runtime.h"
#include <dsound.h>

namespace tmloader {
HRESULT WINAPI tracedSound8(LPCGUID guid, LPDIRECTSOUND8* output, LPUNKNOWN outer) {
    // TM imports dsound ordinal 11 (DirectSoundCreate8) and requires
    // IDirectSoundBuffer8 for both music and effects. DirectSoundCreate can
    // succeed here but its buffers reject that interface with E_NOINTERFACE.
    static auto factory = reinterpret_cast<decltype(&DirectSoundCreate8)>(
        GetProcAddress(LoadLibraryW(L"dsound.dll"), "DirectSoundCreate8"));
    HRESULT result = factory ? factory(guid, output, outer) : E_NOINTERFACE;
    logf("DirectSoundCreate8=%08lX", result);
    return result;
}
}
