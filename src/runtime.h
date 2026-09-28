#pragma once
#include <winsock2.h>

#include <windows.h>

#include <mmsystem.h>

#include "config.h"
#include <cstdio>
#include <dinput.h>
#include <dsound.h>
#include <objbase.h>

namespace tmloader
{

using Command = int(__cdecl*)(...);
extern Config options;
extern BYTE*  gameBase;
extern HWND   gameWindow;
extern DWORD  frames;
extern char   runtimeRoot[MAX_PATH];

void        openLog(const std::filesystem::path& path);
void        logf(const char* format, ...);
auto WINAPI crash(EXCEPTION_POINTERS* ep) -> LONG;
auto        lowerIntegrity() -> bool;
void        replacePointer(HMODULE image, void* oldValue, void* newValue);
void        setSlot(HMODULE module, DWORD rva, void* function);
void        installFileHooks(HMODULE module, bool game);
void        installNetworkHooks(HMODULE module);
void        installDesktopHooks(HMODULE module);
void        initializeViewerSession(HMODULE core, Command* table);
auto WINAPI tracedInput(HINSTANCE, DWORD, REFIID, void**, IUnknown*) -> HRESULT;
auto WINAPI tracedSound8(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN) -> HRESULT;
auto WINAPI localD3D8(UINT) -> void*;
auto        takeMousePresses() -> BYTE;
auto WINAPI loggedMessage(HWND, const char*, const char*, UINT) -> int;
auto WINAPI loggedMessageW(HWND, const wchar_t*, const wchar_t*, UINT) -> int;
auto        launch(const Config& config) -> int;

inline auto gameValue(DWORD rva) -> DWORD&
{
    return *reinterpret_cast<DWORD*>(gameBase + rva);
}

} // namespace tmloader
