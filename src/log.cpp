#include "runtime.h"
#include <intrin.h>
#include <sddl.h>
#include <cstdarg>
#include <stdexcept>
namespace tmloader {
static FILE *logfile;
void logf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    if (!logfile) logfile = stderr;
    vfprintf(logfile, format, args);
    va_end(args);
    fputc('\n', logfile);
    fflush(logfile);
}
LONG WINAPI crash(EXCEPTION_POINTERS *ep)
{
    logf("EXCEPTION %08lX at %p EIP=%08lX ESP=%08lX", ep->ExceptionRecord->ExceptionCode,
         ep->ExceptionRecord->ExceptionAddress, ep->ContextRecord->Eip, ep->ContextRecord->Esp);
    for (DWORD i = 0; i < ep->ExceptionRecord->NumberParameters; i++)
        logf("  info[%lu]=%08lX", i, ep->ExceptionRecord->ExceptionInformation[i]);
    DWORD *stack = (DWORD *)ep->ContextRecord->Esp;
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
bool lowerIntegrity()
{
    HANDLE token = NULL;
    PSID sid = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_DEFAULT | TOKEN_QUERY, &token))
        return false;
    if (!ConvertStringSidToSidA("S-1-16-4096", &sid))
    {
        CloseHandle(token);
        return false;
    }
    TOKEN_MANDATORY_LABEL label = {};
    label.Label.Attributes = SE_GROUP_INTEGRITY;
    label.Label.Sid = sid;
    BOOL result = SetTokenInformation(token, TokenIntegrityLevel, &label, sizeof(label) + GetLengthSid(sid));
    DWORD error = GetLastError();
    LocalFree(sid);
    CloseHandle(token);
    logf("Low integrity: %d error=%lu", result, error);
    return result != FALSE;
}
int WINAPI loggedMessage(HWND, const char *text, const char *title, UINT flags)
{
    logf("MessageBoxA caller=%p [%s] flags=%X: %s", _ReturnAddress(), title ? title : "", flags, text ? text : "");
    return IDOK;
}
int WINAPI loggedMessageW(HWND, const wchar_t *text, const wchar_t *title, UINT flags)
{
    logf("MessageBoxW [%ls] flags=%X: %ls", title ? title : L"", flags, text ? text : L"");
    return IDOK;
}

void openLog(const std::filesystem::path& path) {
    logfile = _wfopen(path.c_str(), L"w");
    if (!logfile) throw std::runtime_error("Cannot open log file");
}
}
