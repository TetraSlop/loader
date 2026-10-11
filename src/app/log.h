#pragma once
#include <windows.h>

#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace tmloader
{

void openLog(const std::filesystem::path& path);
// Appends one line; falls back to stderr before openLog.
void writeLog(std::string_view line);

// Formats into a stack buffer so the crash handler can log without allocating.
// Lines longer than the buffer are truncated.
template <typename... Args>
void logLine(std::format_string<Args...> format, Args&&... args)
{
    char       buffer[1024];
    const auto result = std::format_to_n(buffer, sizeof(buffer), format, std::forward<Args>(args)...);
    writeLog({ buffer, static_cast<size_t>(result.out - buffer) });
}

// Native callers may pass null strings to logged APIs.
auto printable(const char* text) -> const char*;
auto narrow(std::wstring_view text) -> std::string;
auto narrow(const wchar_t* text) -> std::string;
// Formats a handle or typed pointer as an address.
auto address(const void* pointer) -> const void*;
// Formats an HRESULT or status code as its unsigned hex bit pattern.
auto hex(long value) -> unsigned long;

} // namespace tmloader
