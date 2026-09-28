#include "app/hooks.h"
#include "app/log.h"
#include "platform/patch.h"

#include <mmsystem.h>

#include <dsound.h>

namespace tmloader
{
namespace
{

constexpr DWORD directSoundCreateImport = 0x20401c;

// Music and effects need IDirectSoundBuffer8, which only DirectSoundCreate8 devices provide.
auto WINAPI hookedDirectSoundCreate8(LPCGUID device, LPDIRECTSOUND8* output, LPUNKNOWN outer) -> HRESULT
{
    static const auto factory = reinterpret_cast<decltype(&DirectSoundCreate8)>(GetProcAddress(LoadLibraryW(L"dsound.dll"), "DirectSoundCreate8"));
    const auto        result  = [&]() -> HRESULT
    {
        if (!factory)
        {
            return E_NOINTERFACE;
        }

        return factory(device, output, outer);
    }();
    logLine("DirectSoundCreate8={:08X}", hex(result));
    return result;
}

} // namespace

void installAudioHooks(HMODULE game)
{
    setImport(game, directSoundCreateImport, reinterpret_cast<void*>(hookedDirectSoundCreate8));
}

} // namespace tmloader
