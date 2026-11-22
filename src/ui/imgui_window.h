#pragma once
#include <windows.h>

#include <d3d9.h>

#include <functional>

namespace tmloader
{

struct ImGuiWindowSpec
{
    const wchar_t* title  = L"";
    int            width  = 0; // At 100% display scale.
    int            height = 0;
    // Placed near this window when set, otherwise centered on the primary monitor.
    HWND owner = nullptr;
};

// Runs a Dear ImGui (Win32 + DirectX 9) window on the calling thread until frame()
// returns false or the user closes it. created() receives the window and its device,
// e.g. to load images. Returns false when the window or its device could not be created.
using ImGuiWindowCreated = std::function<void(HWND, IDirect3DDevice9*)>;
auto runImGuiWindow(const ImGuiWindowSpec& spec, const std::function<bool()>& frame, const ImGuiWindowCreated& created = {}) -> bool;

void applyImGuiStyle(float scale);

} // namespace tmloader
