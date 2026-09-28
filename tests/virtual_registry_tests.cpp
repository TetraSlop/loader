#include "../src/files.cpp"
#include "runtime.h"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string_view>

namespace tmloader
{

Config options;
char   runtimeRoot[MAX_PATH]{};

void logf(const char*, ...)
{
}

void replacePointer(HMODULE, void*, void*)
{
}

void setSlot(HMODULE, DWORD, void*)
{
}

auto WINAPI loggedMessage(HWND, const char*, const char*, UINT) -> int
{
    return IDOK;
}

auto WINAPI loggedMessageW(HWND, const wchar_t*, const wchar_t*, UINT) -> int
{
    return IDOK;
}

} // namespace tmloader

TEST_CASE("Virtual registry preserves native sound and value sizes", "[virtual_registry]")
{
    using namespace tmloader;
    const auto key  = reinterpret_cast<HKEY>(0x5a5a0001);
    DWORD      type = 0, size = 0, value = 99;
    REQUIRE(tracedQuery(key, "SOUND", nullptr, &type, nullptr, &size) == ERROR_SUCCESS);
    REQUIRE((type == REG_DWORD && size == 4));
    size = 1;
    REQUIRE(tracedQuery(key, "SOUND", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_MORE_DATA);
    REQUIRE((value == 99 && size == 4));
    REQUIRE(tracedQuery(key, "SOUND", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS);
    REQUIRE(value == 1); // Regression: the original startup experiment forced silent mode.
    REQUIRE(tracedQuery(key, "FULLSCREEN", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS);
    REQUIRE(value == 0);
}

TEST_CASE("Virtual registry strings preserve query and buffer contracts", "[virtual_registry]")
{
    using namespace tmloader;
    const auto key = reinterpret_cast<HKEY>(0x5a5a0001);
    strcpy_s(runtimeRoot, "C:\\fixture");
    for (const auto* name : { "LOADDIR", "0002", "Path", "InstallFolder" })
    {
        CAPTURE(name);
        const auto expected = [&]() -> std::string_view
        {
            if (std::string_view{ name } == "LOADDIR")
            {
                return "";
            }
            return runtimeRoot;
        }();
        const auto required = static_cast<DWORD>(expected.size() + 1);
        auto       type     = DWORD{ 0 };
        auto       size     = DWORD{ 0 };
        REQUIRE(tracedQuery(key, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS);
        REQUIRE(type == REG_SZ);
        REQUIRE(size == required);

        auto buffer = std::array<BYTE, 64>{};
        buffer.fill(0xa5);
        const auto untouched = buffer;
        size                 = required - 1;
        REQUIRE(tracedQuery(key, name, nullptr, &type, buffer.data(), &size) == ERROR_MORE_DATA);
        REQUIRE(size == required);
        REQUIRE(buffer == untouched);

        REQUIRE(tracedQuery(key, name, nullptr, nullptr, buffer.data(), &size) == ERROR_SUCCESS);
        REQUIRE(std::string_view{ reinterpret_cast<const char*>(buffer.data()) } == expected);
        REQUIRE(buffer[required] == 0xa5);
        type = 0;
        REQUIRE(tracedQuery(key, name, nullptr, &type, nullptr, nullptr) == ERROR_INVALID_PARAMETER);
        REQUIRE(type == REG_SZ);
    }
}

TEST_CASE("Virtual registry rejects missing values without changing outputs", "[virtual_registry]")
{
    using namespace tmloader;
    const auto key = reinterpret_cast<HKEY>(0x5a5a0001);
    for (const auto* name : { static_cast<const char*>(nullptr), "unknown" })
    {
        auto type = DWORD{ 42 };
        auto size = DWORD{ 4 };
        auto data = DWORD{ 99 };
        REQUIRE(tracedQuery(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&data), &size) == ERROR_FILE_NOT_FOUND);
        REQUIRE(type == 42);
        REQUIRE(size == 4);
        REQUIRE(data == 99);
    }
    for (const auto* name : { "SOUND", "FULLSCREEN" })
    {
        auto type = DWORD{ 0 };
        REQUIRE(tracedQuery(key, name, nullptr, &type, nullptr, nullptr) == ERROR_INVALID_PARAMETER);
        REQUIRE(type == REG_DWORD);
    }
}
