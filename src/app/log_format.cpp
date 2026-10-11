#include "app/log.h"

namespace tmloader
{

auto printable(const char* text) -> const char*
{
    if (text)
    {
        return text;
    }

    return "";
}

auto narrow(std::wstring_view text) -> std::string
{
    const auto length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    auto       result = std::string(static_cast<size_t>(length), char{});
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

auto narrow(const wchar_t* text) -> std::string
{
    if (!text)
    {
        return {};
    }

    return narrow(std::wstring_view{ text });
}

auto widen(std::string_view text) -> std::wstring
{
    const auto length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    auto       result = std::wstring(static_cast<size_t>(length), wchar_t{});
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

auto address(const void* pointer) -> const void*
{
    return pointer;
}

auto hex(long value) -> unsigned long
{
    return static_cast<unsigned long>(value);
}

} // namespace tmloader
