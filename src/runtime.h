#pragma once
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <dinput.h>
#include <mmsystem.h>
#include <dsound.h>
#include <cstdio>
#include "config.h"

namespace tmloader {
using Command = int(__cdecl*)(...);
extern Config options;
extern BYTE* gameBase;
extern HWND gameWindow;
extern DWORD frames;
extern char runtimeRoot[MAX_PATH];

void openLog(const std::filesystem::path& path);
void logf(const char* format, ...);
LONG WINAPI crash(EXCEPTION_POINTERS* ep);
bool lowerIntegrity();
void replacePointer(HMODULE image, void* oldValue, void* newValue);
void setSlot(HMODULE module, DWORD rva, void* function);
void installFileHooks(HMODULE module, bool game);
void installNetworkHooks(HMODULE module);
void installDesktopHooks(HMODULE module);
void initializeViewerSession(HMODULE core, Command* table);
HRESULT WINAPI tracedInput(HINSTANCE, DWORD, REFIID, void**, IUnknown*);
HRESULT WINAPI tracedSound8(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN);
void* WINAPI localD3D8(UINT);
BYTE takeMousePresses();
int WINAPI loggedMessage(HWND, const char*, const char*, UINT);
int WINAPI loggedMessageW(HWND, const wchar_t*, const wchar_t*, UINT);
int launch(const Config& config);
inline DWORD& gameValue(DWORD rva) { return *reinterpret_cast<DWORD*>(gameBase + rva); }
}
