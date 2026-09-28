#include "app/runtime.h"
#include "app/log.h"
#include <aclapi.h>
#include <sddl.h>
#include <stdexcept>
#include <wil/resource.h>

namespace tmloader
{

Config options;
BYTE*  gameBase   = nullptr;
HWND   gameWindow = nullptr;
char   runtimeRoot[MAX_PATH]{};

namespace
{

// Wine has no mandatory integrity control.
auto isWine() -> bool
{
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version");
}

} // namespace

void allowLowIntegrityWrites(const std::filesystem::path& runtimeDir)
{
    if (isWine())
    {
        return;
    }

    wil::unique_hlocal_security_descriptor descriptor;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;OICI;NW;;;LW)", SDDL_REVISION_1, &descriptor, nullptr))
    {
        throw std::runtime_error("Cannot build low-integrity label");
    }

    PACL label     = nullptr;
    BOOL present   = FALSE;
    BOOL defaulted = FALSE;
    GetSecurityDescriptorSacl(descriptor.get(), &present, &label, &defaulted);
    for (const auto* const name : { "pub", "logs", "settings" })
    {
        const auto folder = runtimeDir / name;
        std::filesystem::create_directories(folder);
        // Inheritable, so existing and future contents are labeled too.
        auto path = folder.wstring();
        if (SetNamedSecurityInfoW(path.data(), SE_FILE_OBJECT, LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, label) != ERROR_SUCCESS)
        {
            throw std::runtime_error("Cannot make runtime folder writable: " + folder.string());
        }
    }
}

auto lowerIntegrity() -> bool
{
    if (isWine())
    {
        logLine("Wine runtime: Windows mandatory integrity is unavailable");
        return true;
    }

    wil::unique_handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_DEFAULT | TOKEN_QUERY, token.put()))
    {
        return false;
    }

    wil::unique_hlocal sid;
    if (!ConvertStringSidToSidA("S-1-16-4096", sid.put()))
    {
        return false;
    }

    auto label             = TOKEN_MANDATORY_LABEL{};
    label.Label.Attributes = SE_GROUP_INTEGRITY;
    label.Label.Sid        = sid.get();
    const auto result      = SetTokenInformation(token.get(), TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(sid.get()));
    logLine("Low integrity: {} error={}", result, GetLastError());
    return result != FALSE;
}

auto WINAPI logUnhandledException(EXCEPTION_POINTERS* exception) -> LONG
{
    const auto* const record  = exception->ExceptionRecord;
    const auto* const context = exception->ContextRecord;
    logLine("EXCEPTION {:08X} at {} EIP={:08X} ESP={:08X}", record->ExceptionCode, record->ExceptionAddress, context->Eip, context->Esp);
    for (DWORD i = 0; i < record->NumberParameters; ++i)
    {
        logLine("  info[{}]={:08X}", i, record->ExceptionInformation[i]);
    }

    const auto* const stack = reinterpret_cast<const DWORD*>(context->Esp);
    __try
    {
        for (int i = 0; i < 16; ++i)
        {
            logLine("  stack[{}]={:08X}", i, stack[i]);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

} // namespace tmloader
