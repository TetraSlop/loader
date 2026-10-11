#pragma once
#include <winsock2.h>

#include <windows.h>

#include "config/config.h"

namespace tmloader
{

// Entries of the Viewer's common-function table are cdecl and variadic.
using Command = int(__cdecl*)(...);

// Legacy native state is process-wide; one game session runs per process.
extern Config options;
extern BYTE*  gameBase;
extern HWND   gameWindow;
extern char   runtimeRoot[MAX_PATH];

auto launch(const Config& config) -> int;
auto lowerIntegrity() -> bool;
// Labels the folders the game writes to (saves, logs, settings) as writable at low integrity.
void        allowLowIntegrityWrites(const std::filesystem::path& runtimeDir);
auto WINAPI logUnhandledException(EXCEPTION_POINTERS* exception) -> LONG;

} // namespace tmloader
