#include "app/hooks.h"
#include "app/log.h"
#include "platform/patch.h"
#include <cstring>
#include <intrin.h>
#include <string_view>

namespace tmloader
{
namespace
{

// Handle returned for every registry key the game opens; values are served below.
const auto kVirtualRegistryKey = reinterpret_cast<HKEY>(0x5a5a0001);

constexpr DWORD kMessageBoxImport    = 0x2041fc;
constexpr DWORD kRegOpenKeyImport    = 0x204000;
constexpr DWORD kRegQueryValueImport = 0x204004;
constexpr DWORD kRegCloseKeyImport   = 0x204008;
constexpr DWORD kRegCreateKeyImport  = 0x20400c;

constexpr DWORD kWriteAccess = GENERIC_WRITE | DELETE | WRITE_DAC | WRITE_OWNER | FILE_WRITE_DATA | FILE_APPEND_DATA;

auto registryValue(DWORD valueType, const void* value, DWORD size, LPDWORD type, LPBYTE data, LPDWORD length) -> LSTATUS
{
    if (type)
    {
        *type = valueType;
    }

    if (!length)
    {
        return ERROR_INVALID_PARAMETER;
    }

    const auto capacity = *length;
    *length             = size;
    if (!data)
    {
        return ERROR_SUCCESS;
    }

    if (capacity < size)
    {
        return ERROR_MORE_DATA;
    }

    std::memcpy(data, value, size);
    return ERROR_SUCCESS;
}

auto writesFile(DWORD access, DWORD disposition) -> bool
{
    return (access & kWriteAccess) || disposition == CREATE_ALWAYS || disposition == CREATE_NEW ||
           disposition == TRUNCATE_EXISTING || disposition == OPEN_ALWAYS;
}

auto WINAPI hookedCreateFileA(const char* path, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE templateFile) -> HANDLE
{
    char       full[MAX_PATH]{};
    const auto length = GetFullPathNameA(path, MAX_PATH, full, nullptr);
    if (!length || length >= MAX_PATH)
    {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return INVALID_HANDLE_VALUE;
    }

    if (writesFile(access, disposition) && !pathIsWithin(full, runtimeRoot))
    {
        logLine("Blocked file write outside runtime: {}", path);
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }

    const auto handle = CreateFileA(path, access, sharing, security, disposition, flags, templateFile);
    const auto error  = GetLastError();
    logLine("CreateFileA {} -> {} error={}", path, handle, error);
    SetLastError(error);
    return handle;
}

auto WINAPI hookedRegQueryValueExA(HKEY key, const char* name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD length) -> LSTATUS
{
    if (key != kVirtualRegistryKey)
    {
        const auto result = RegQueryValueExA(key, name, reserved, type, data, length);
        logLine("RegQuery {} -> {}", printable(name), result);
        return result;
    }

    if (!name)
    {
        logLine("Virtual registry query: (default)");
        return ERROR_FILE_NOT_FOUND;
    }

    logLine("Virtual registry query: {}", name);
    const auto valueName = std::string_view{ name };
    if (valueName == "LOADDIR")
    {
        // An empty prefix avoids the archive-name parser's first-dot handling.
        return registryValue(REG_SZ, "", 1, type, data, length);
    }

    if (valueName == "0002" || valueName == "Path" || valueName == "InstallFolder")
    {
        const auto size = static_cast<DWORD>(std::strlen(runtimeRoot) + 1);
        return registryValue(REG_SZ, runtimeRoot, size, type, data, length);
    }

    if (valueName == "SOUND" || valueName == "FULLSCREEN")
    {
        // SOUND=0 makes the game skip its sound engine entirely (+180E40).
        const auto value = static_cast<DWORD>(valueName == "SOUND");
        return registryValue(REG_DWORD, &value, sizeof(value), type, data, length);
    }

    return ERROR_FILE_NOT_FOUND;
}

auto WINAPI hookedRegCreateKeyExA(HKEY, const char* subkey, DWORD, LPSTR, DWORD, REGSAM, const LPSECURITY_ATTRIBUTES, PHKEY result, LPDWORD disposition) -> LSTATUS
{
    logLine("Virtual registry create: {}", printable(subkey));
    if (result)
    {
        *result = kVirtualRegistryKey;
    }

    if (disposition)
    {
        *disposition = REG_OPENED_EXISTING_KEY;
    }

    return ERROR_SUCCESS;
}

auto WINAPI hookedRegOpenKeyA(HKEY, const char* subkey, PHKEY result) -> LSTATUS
{
    logLine("Virtual registry open: {}", printable(subkey));
    if (result)
    {
        *result = kVirtualRegistryKey;
    }

    return ERROR_SUCCESS;
}

auto WINAPI hookedRegCloseKey(HKEY key) -> LSTATUS
{
    if (key == kVirtualRegistryKey)
    {
        return ERROR_SUCCESS;
    }

    return RegCloseKey(key);
}

auto WINAPI hookedMessageBoxA(HWND, const char* text, const char* title, UINT flags) -> int
{
    logLine("MessageBoxA caller={} [{}] flags={:X}: {}", _ReturnAddress(), printable(title), flags, printable(text));
    return IDOK;
}

auto WINAPI hookedMessageBoxW(HWND, const wchar_t* text, const wchar_t* title, UINT flags) -> int
{
    logLine("MessageBoxW [{}] flags={:X}: {}", narrow(title), flags, narrow(text));
    return IDOK;
}

} // namespace

void installSystemHooks(HMODULE module, bool game)
{
    replacePointer(module, reinterpret_cast<void*>(MessageBoxA), reinterpret_cast<void*>(hookedMessageBoxA));
    replacePointer(module, reinterpret_cast<void*>(MessageBoxW), reinterpret_cast<void*>(hookedMessageBoxW));
    replacePointer(module, reinterpret_cast<void*>(CreateFileA), reinterpret_cast<void*>(hookedCreateFileA));
    replacePointer(module, reinterpret_cast<void*>(RegQueryValueExA), reinterpret_cast<void*>(hookedRegQueryValueExA));
    if (!game)
    {
        return;
    }

    setImport(module, kMessageBoxImport, reinterpret_cast<void*>(hookedMessageBoxA));
    setImport(module, kRegOpenKeyImport, reinterpret_cast<void*>(hookedRegOpenKeyA));
    setImport(module, kRegQueryValueImport, reinterpret_cast<void*>(hookedRegQueryValueExA));
    setImport(module, kRegCloseKeyImport, reinterpret_cast<void*>(hookedRegCloseKey));
    setImport(module, kRegCreateKeyImport, reinterpret_cast<void*>(hookedRegCreateKeyExA));
}

} // namespace tmloader
