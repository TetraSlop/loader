#pragma once
#include <windows.h>

namespace tmloader {
// D3D8 ABI layout, independently declared to avoid requiring the obsolete SDK.
struct Present8 {
    DWORD width, height, format, count, multisample, swap;
    HWND window;
    BOOL windowed, depth;
    DWORD depthFormat, flags, refresh, interval;
};
static_assert(sizeof(Present8) == 52, "D3D8 presentation parameters require x86");
}
