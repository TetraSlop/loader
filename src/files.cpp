#include "runtime.h"

namespace tmloader
{
namespace
{

const auto virtualRegistryKey = reinterpret_cast<HKEY>(0x5a5a0001);

}

static HANDLE WINAPI tracedFile(const char* path, DWORD access, DWORD sharing, LPSECURITY_ATTRIBUTES sec, DWORD create, DWORD flags, HANDLE tmpl)
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

static LSTATUS WINAPI tracedQuery(HKEY key, const char* name, LPDWORD reserved, LPDWORD type, LPBYTE data, LPDWORD length)
{
    if (key == virtualRegistryKey)
    {
        logf("Virtual registry query: %s", name ? name : "(default)");
        if (name && !strcmp(name, "LOADDIR"))
        {
            // TM scans for the FIRST dot when deriving gW archive numbers.
            // An empty prefix makes paths "data/gW201.dat" relative to our
            // runtime working directory. ".\\" silently corrupts that index.
            if (type)
                *type = REG_SZ;
            if (!length)
                return ERROR_INVALID_PARAMETER;
            const auto capacity = *length;
            *length             = 1;
            if (data && capacity < 1)
                return ERROR_MORE_DATA;
            if (data)
                *data = 0;
            return ERROR_SUCCESS;
        }
        if (name && (!strcmp(name, "0002") || !strcmp(name, "Path") || !strcmp(name, "InstallFolder")))
        {
            const auto size = static_cast<DWORD>(strlen(runtimeRoot) + 1);
            if (type)
                *type = REG_SZ;
            if (!length)
                return ERROR_INVALID_PARAMETER;
            const auto capacity = *length;
            *length             = size;
            if (data && capacity < size)
                return ERROR_MORE_DATA;
            if (data)
                memcpy(data, runtimeRoot, size);
            return ERROR_SUCCESS;
        }
        if (name && (!strcmp(name, "SOUND") || !strcmp(name, "FULLSCREEN")))
        {
            if (type)
                *type = REG_DWORD;
            if (!length)
                return ERROR_INVALID_PARAMETER;
            const auto capacity = *length;
            *length             = sizeof(DWORD);
            if (data && capacity < sizeof(DWORD))
                return ERROR_MORE_DATA;
            // SOUND=0 was intentional in the original startup-only experiment.
            // TM +180E40 uses zero to skip its entire native sound engine.
            if (data)
            {
                const DWORD value = !strcmp(name, "SOUND") ? 1 : 0;
                memcpy(data, &value, sizeof(value));
            }
            return ERROR_SUCCESS;
        }
        return ERROR_FILE_NOT_FOUND;
    }
    const auto result = RegQueryValueExA(key, name, reserved, type, data, length);
    logf("RegQuery %s -> %ld bytes=%lu", name ? name : "(default)", result, length ? *length : 0);
    return result;
}

static LSTATUS WINAPI virtualCreate(HKEY, const char* subkey, DWORD, LPSTR, DWORD, REGSAM, const LPSECURITY_ATTRIBUTES, PHKEY result, LPDWORD disposition)
{
    logf("Virtual registry create: %s", subkey ? subkey : "");
    if (result)
        *result = virtualRegistryKey;
    if (disposition)
        *disposition = REG_OPENED_EXISTING_KEY;
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI virtualOpen(HKEY, const char* subkey, PHKEY result)
{
    logf("Virtual registry open: %s", subkey ? subkey : "");
    if (result)
        *result = virtualRegistryKey;
    return ERROR_SUCCESS;
}

static LSTATUS WINAPI virtualClose(HKEY key)
{
    return key == virtualRegistryKey ? ERROR_SUCCESS : RegCloseKey(key);
}

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
