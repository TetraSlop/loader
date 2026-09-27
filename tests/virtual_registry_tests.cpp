#include <catch2/catch_test_macros.hpp>
#include "runtime.h"
#include "../src/files.cpp"
namespace tmloader {
Config options;
char runtimeRoot[MAX_PATH]{};
void logf(const char*, ...) {}
void replacePointer(HMODULE, void*, void*) {}
void setSlot(HMODULE, DWORD, void*) {}
int WINAPI loggedMessage(HWND, const char*, const char*, UINT) { return IDOK; }
int WINAPI loggedMessageW(HWND, const wchar_t*, const wchar_t*, UINT) { return IDOK; }
}
TEST_CASE("Virtual registry preserves native sound and value sizes", "[virtual_registry]") {
    using namespace tmloader;
    HKEY key = reinterpret_cast<HKEY>(0x5a5a0001);
    DWORD type = 0, size = 0, value = 99;
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
