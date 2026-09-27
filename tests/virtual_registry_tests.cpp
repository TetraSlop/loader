#ifdef NDEBUG
#undef NDEBUG
#endif
#include "runtime.h"
#include <cassert>
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
int main() {
    using namespace tmloader;
    HKEY key = reinterpret_cast<HKEY>(0x5a5a0001);
    DWORD type = 0, size = 0, value = 99;
    assert(tracedQuery(key, "SOUND", nullptr, &type, nullptr, &size) == ERROR_SUCCESS);
    assert(type == REG_DWORD && size == 4);
    size = 1;
    assert(tracedQuery(key, "SOUND", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_MORE_DATA);
    assert(value == 99 && size == 4);
    assert(tracedQuery(key, "SOUND", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS);
    assert(value == 1); // Regression: the original startup experiment forced silent mode.
    assert(tracedQuery(key, "FULLSCREEN", nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS);
    assert(value == 0);
}
