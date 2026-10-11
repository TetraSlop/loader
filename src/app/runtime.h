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
void allowLowIntegrityWrites(const std::filesystem::path& runtimeDir);
// Where the crash handler writes its minidump; set once at startup.
void setCrashDumpPath(const std::filesystem::path& path);
// Writes a minidump of this process without allocating; false with the error in GetLastError.
auto writeCrashDump(EXCEPTION_POINTERS* exception) -> bool;
// Unhandled-exception filter: logs the exception and writes the minidump.
auto WINAPI logUnhandledException(EXCEPTION_POINTERS* exception) -> LONG;

} // namespace tmloader
