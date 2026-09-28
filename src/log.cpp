#include "runtime.h"
#include <cstdarg>
#include <intrin.h>
#include <sddl.h>
#include <stdexcept>
#include <wil/resource.h>

namespace tmloader
{
namespace
{

FILE* logfile = nullptr;

}

void logf(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    if (!logfile)
        logfile = stderr;
    vfprintf(logfile, format, args);
    va_end(args);
    fputc('\n', logfile);
    fflush(logfile);
}

auto WINAPI crash(EXCEPTION_POINTERS* ep) -> LONG
{
    logf("EXCEPTION %08lX at %p EIP=%08lX ESP=%08lX", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress, ep->ContextRecord->Eip, ep->ContextRecord->Esp);
    for (DWORD i = 0; i < ep->ExceptionRecord->NumberParameters; i++)
        logf("  info[%lu]=%08lX", i, ep->ExceptionRecord->ExceptionInformation[i]);
    const auto* const stack = reinterpret_cast<const DWORD*>(ep->ContextRecord->Esp);
    __try
    {
        for (int i = 0; i < 16; i++)
            logf("  stack[%d]=%08lX", i, stack[i]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

auto lowerIntegrity() -> bool
{
    wil::unique_handle token;
    wil::unique_hlocal sid;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_DEFAULT | TOKEN_QUERY, token.put()))
        return false;
    if (!ConvertStringSidToSidA("S-1-16-4096", sid.put()))
        return false;
    TOKEN_MANDATORY_LABEL label = {};
    label.Label.Attributes      = SE_GROUP_INTEGRITY;
    label.Label.Sid             = sid.get();
    const auto result           = SetTokenInformation(token.get(), TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(sid.get()));
    const auto error            = GetLastError();
    logf("Low integrity: %d error=%lu", result, error);
    return result != FALSE;
}

auto WINAPI loggedMessage(HWND, const char* text, const char* title, UINT flags) -> int
{
    logf("MessageBoxA caller=%p [%s] flags=%X: %s", _ReturnAddress(), title ? title : "", flags, text ? text : "");
    return IDOK;
}

auto WINAPI loggedMessageW(HWND, const wchar_t* text, const wchar_t* title, UINT flags) -> int
{
    logf("MessageBoxW [%ls] flags=%X: %ls", title ? title : L"", flags, text ? text : L"");
    return IDOK;
}

void openLog(const std::filesystem::path& path)
{
    if (!outputPathAllowed(options, path))
        throw std::runtime_error("Refusing unsafe log destination");
    std::filesystem::create_directories(path.parent_path());
    if (options.diagnostics)
        std::filesystem::create_directories(options.runtimeDir / "logs/captures");
    logfile = _wfopen(path.c_str(), L"w");
    if (!logfile)
        throw std::runtime_error("Cannot open log file");
}

} // namespace tmloader
