#pragma once
#include <windows.h>

#include <functional>

namespace tmloader
{

struct ImGuiWindowSpec
{
    const wchar_t* title  = L"";
    int            width  = 0; // At 100% display scale.
    int            height = 0;
    // Placed near this window when set, otherwise centred on the primary monitor.
    HWND owner = nullptr;
};

// Runs a Dear ImGui (Win32 + DirectX 9) window on the calling thread until frame()
// returns false or the user closes it. created() receives the window handle.
// Returns false when the window or its device could not be created.
auto runImGuiWindow(const ImGuiWindowSpec& spec, const std::function<bool()>& frame, const std::function<void(HWND)>& created = {}) -> bool;

void applyImGuiStyle(float scale);

} // namespace tmloader
