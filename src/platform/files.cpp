#include "app/runtime.h"
#include <cstring>
#include <string_view>

namespace tmloader
{
namespace
{

const auto virtualRegistryKey = reinterpret_cast<HKEY>(0x5a5a0001);

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

auto WINAPI tracedFile(const char* path, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES sec, DWORD create, DWORD flags, HANDLE tmpl) -> HANDLE
{
    char       full[MAX_PATH] = {};
    const auto length         = GetFullPathNameA(path, MAX_PATH, full, nullptr);
    if (!length || length >= MAX_PATH)
    {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return INVALID_HANDLE_VALUE;
    }
    if (((access & (GENERIC_WRITE | DELETE | WRITE_DAC | WRITE_OWNER | FILE_WRITE_DATA | FILE_APPEND_DATA)) ||
         create == CREATE_ALWAYS || create == CREATE_NEW || create == TRUNCATE_EXISTING || create == OPEN_ALWAYS) &&
        !pathIsWithin(full, runtimeRoot))
    {
        logf("Blocked file write outside runtime: %s", path);
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    const auto handle = CreateFileA(path, access, sharing, sec, create, flags, tmpl);
    const auto error  = GetLastError();
    logf("CreateFileA %s -> %p error=%lu", path, handle, error);
    SetLastError(error);
    return handle;
}

auto WINAPI tracedQuery(HKEY key, const char* name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD length) -> LSTATUS
{
    if (key == virtualRegistryKey)
    {
        if (!name)
        {
            logf("Virtual registry query: (default)");
            return ERROR_FILE_NOT_FOUND;
        }
        logf("Virtual registry query: %s", name);
        const auto valueName = std::string_view{ name };
        if (valueName == "LOADDIR")
        {
            // An empty prefix preserves TM's first-dot archive-number parser.
            return registryValue(REG_SZ, "", 1, type, data, length);
        }
        if (valueName == "0002" || valueName == "Path" || valueName == "InstallFolder")
        {
            const auto size = static_cast<DWORD>(std::strlen(runtimeRoot) + 1);
            return registryValue(REG_SZ, runtimeRoot, size, type, data, length);
        }
        if (valueName == "SOUND" || valueName == "FULLSCREEN")
        {
            // TM +180E40 skips its native sound engine when SOUND is zero.
            const auto value = static_cast<DWORD>(valueName == "SOUND");
            return registryValue(REG_DWORD, &value, sizeof(value), type, data, length);
        }
        return ERROR_FILE_NOT_FOUND;
    }
    const auto result = RegQueryValueExA(key, name, reserved, type, data, length);
    logf("RegQuery %s -> %ld bytes=%lu", name ? name : "(default)", result, length ? *length : 0);
    return result;
}

auto WINAPI virtualCreate(HKEY, const char* subkey, DWORD, LPSTR, DWORD, REGSAM, const LPSECURITY_ATTRIBUTES, PHKEY result, LPDWORD disposition) -> LSTATUS
{
    logf("Virtual registry create: %s", subkey ? subkey : "");
    if (result)
        *result = virtualRegistryKey;
    if (disposition)
        *disposition = REG_OPENED_EXISTING_KEY;
    return ERROR_SUCCESS;
}

auto WINAPI virtualOpen(HKEY, const char* subkey, PHKEY result) -> LSTATUS
{
    logf("Virtual registry open: %s", subkey ? subkey : "");
    if (result)
        *result = virtualRegistryKey;
    return ERROR_SUCCESS;
}

auto WINAPI virtualClose(HKEY key) -> LSTATUS
{
    return key == virtualRegistryKey ? ERROR_SUCCESS : RegCloseKey(key);
}

} // namespace

void installFileHooks(HMODULE module, bool game)
{
    replacePointer(module, reinterpret_cast<void*>(MessageBoxA), reinterpret_cast<void*>(loggedMessage));
    replacePointer(module, reinterpret_cast<void*>(MessageBoxW), reinterpret_cast<void*>(loggedMessageW));
    replacePointer(module, reinterpret_cast<void*>(CreateFileA), reinterpret_cast<void*>(tracedFile));
    replacePointer(module, reinterpret_cast<void*>(RegQueryValueExA), reinterpret_cast<void*>(tracedQuery));
    if (game)
    {
        setSlot(module, 0x2041fc, reinterpret_cast<void*>(loggedMessage));
        setSlot(module, 0x204000, reinterpret_cast<void*>(virtualOpen));
        setSlot(module, 0x204004, reinterpret_cast<void*>(tracedQuery));
        setSlot(module, 0x204008, reinterpret_cast<void*>(virtualClose));
        setSlot(module, 0x20400c, reinterpret_cast<void*>(virtualCreate));
    }
}

} // namespace tmloader
