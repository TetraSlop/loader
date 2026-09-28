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
inline auto printable(const char* text) -> const char*
{
    if (text)
    {
        return text;
    }

    return "";
}

inline auto narrow(std::wstring_view text) -> std::string
{
    const auto length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    auto       result = std::string(static_cast<size_t>(length), char{});
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

inline auto narrow(const wchar_t* text) -> std::string
{
    if (!text)
    {
        return {};
    }

    return narrow(std::wstring_view{ text });
}

// Formats a handle or typed pointer as an address.
inline auto address(const void* pointer) -> const void*
{
    return pointer;
}

// Formats an HRESULT or status code as its unsigned hex bit pattern.
inline auto hex(long value) -> unsigned long
{
    return static_cast<unsigned long>(value);
}

} // namespace tmloader
