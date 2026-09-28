#include "ui/imgui_window.h"
#include "app/log.h"
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include <algorithm>
#include <d3d9.h>
#include <wrl/client.h>

extern IMGUI_IMPL_API auto ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM) -> LRESULT;

namespace tmloader
{
namespace
{

constexpr auto className = L"TMLoaderImGui";

// Per-window state, reachable from the window procedure through GWLP_USERDATA.
struct WindowState
{
    bool closeRequested = false;
    UINT resizedWidth   = 0;
    UINT resizedHeight  = 0;
};

auto CALLBACK windowProc(const HWND hwnd, const UINT message, const WPARAM wparam, const LPARAM lparam) -> LRESULT
{
    auto* const state = reinterpret_cast<WindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_CLOSE && state)
    {
        state->closeRequested = true;
        return 0;
    }

    if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam))
    {
        return 1;
    }

    if (message == WM_SIZE && wparam != SIZE_MINIMIZED && state)
    {
        state->resizedWidth  = LOWORD(lparam);
        state->resizedHeight = HIWORD(lparam);
        return 0;
    }

    if (message == WM_SYSCOMMAND && (wparam & 0xfff0) == SC_KEYMENU)
    {
        return 0;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

auto placement(const ImGuiWindowSpec& spec, float scale) -> RECT
{
    auto width   = static_cast<int>(spec.width * scale);
    auto height  = static_cast<int>(spec.height * scale);
    auto monitor = MONITORINFO{ sizeof(MONITORINFO) };
    GetMonitorInfoW(MonitorFromWindow(spec.owner, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const auto& work = monitor.rcWork;
    width            = std::min(width, static_cast<int>(work.right - work.left));
    height           = std::min(height, static_cast<int>(work.bottom - work.top));
    auto x           = work.left + (work.right - work.left - width) / 2;
    auto y           = work.top + (work.bottom - work.top - height) / 2;
    if (spec.owner)
    {
        auto owner = RECT{};
        GetWindowRect(spec.owner, &owner);
        x = std::clamp(static_cast<int>(owner.left + 40), static_cast<int>(work.left), static_cast<int>(work.right) - width);
        y = std::clamp(static_cast<int>(owner.top + 40), static_cast<int>(work.top), static_cast<int>(work.bottom) - height);
    }

    return { x, y, x + width, y + height };
}

// Renders frames until frame() returns false or the window is closed.
void renderLoop(HWND hwnd, IDirect3DDevice9* device, D3DPRESENT_PARAMETERS& parameters, WindowState& state, const std::function<bool()>& frame)
{
    auto lost = false;
    auto hr   = HRESULT{ S_OK };
    while (!state.closeRequested)
    {
        auto message = MSG{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        if (state.closeRequested || !IsWindow(hwnd))
        {
            return;
        }

        if (lost)
        {
            hr = device->TestCooperativeLevel();
            if (hr == D3DERR_DEVICELOST)
            {
                Sleep(20);
                continue;
            }
        }

        if (state.resizedWidth || hr == D3DERR_DEVICENOTRESET)
        {
            if (state.resizedWidth)
            {
                parameters.BackBufferWidth  = state.resizedWidth;
                parameters.BackBufferHeight = state.resizedHeight;
            }

            ImGui_ImplDX9_InvalidateDeviceObjects();
            hr = device->Reset(&parameters);
            if (FAILED(hr))
            {
                lost = true;
                Sleep(20);
                continue;
            }

            ImGui_ImplDX9_CreateDeviceObjects();
            state.resizedWidth  = 0;
            state.resizedHeight = 0;
            lost                = false;
        }

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        const auto keepOpen = frame();
        ImGui::Render();
        device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(24, 26, 32), 1.0f, 0);
        if (SUCCEEDED(device->BeginScene()))
        {
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            device->EndScene();
        }

        hr   = device->Present(nullptr, nullptr, nullptr, nullptr);
        lost = hr == D3DERR_DEVICELOST;
        if (!keepOpen)
        {
            return;
        }
    }
}

} // namespace

void applyImGuiStyle(const float scale)
{
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr; // Preferences are stored per account instead.
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    auto& style         = ImGui::GetStyle();
    style.WindowPadding = ImVec2(18, 16);
    style.ItemSpacing   = ImVec2(10, 10);
    style.FrameRounding = 4;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

auto runImGuiWindow(const ImGuiWindowSpec& spec, const std::function<bool()>& frame, const std::function<void(HWND)>& created) -> bool
{
    ImGui_ImplWin32_EnableDpiAwareness();
    const auto instance = GetModuleHandleW(nullptr);
    auto       wc       = WNDCLASSEXW{ sizeof(WNDCLASSEXW) };
    wc.style            = CS_CLASSDC;
    wc.lpfnWndProc      = windowProc;
    wc.hInstance        = instance;
    wc.hCursor          = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.lpszClassName    = className;
    RegisterClassExW(&wc); // Fails harmlessly when a previous window registered it.

    const auto scale     = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromWindow(spec.owner, MONITOR_DEFAULTTOPRIMARY));
    const auto frameRect = placement(spec, scale);
    auto       state     = WindowState{};
    const auto style     = [&]() -> DWORD
    {
        // Owned windows minimize with their owner; a standalone launcher gets its own button.
        if (spec.owner)
        {
            return WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        }

        return WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    }();
    const auto hwnd = CreateWindowExW(0, className, spec.title, style, frameRect.left, frameRect.top, frameRect.right - frameRect.left, frameRect.bottom - frameRect.top, spec.owner, nullptr, instance, nullptr);
    if (!hwnd)
    {
        return false;
    }

    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
    Microsoft::WRL::ComPtr<IDirect3D9> d3d;
    d3d.Attach(Direct3DCreate9(D3D_SDK_VERSION));
    Microsoft::WRL::ComPtr<IDirect3DDevice9> device;
    auto                                     parameters = D3DPRESENT_PARAMETERS{};
    parameters.Windowed                                 = TRUE;
    parameters.SwapEffect                               = D3DSWAPEFFECT_DISCARD;
    parameters.BackBufferFormat                         = D3DFMT_UNKNOWN;
    parameters.PresentationInterval                     = D3DPRESENT_INTERVAL_ONE;
    const auto hr                                       = [&]() -> HRESULT
    {
        if (!d3d)
        {
            return E_FAIL;
        }

        return d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, device.GetAddressOf());
    }();
    if (FAILED(hr))
    {
        logLine("ImGui window graphics initialization failed: {:08X}", hex(hr));
        DestroyWindow(hwnd);
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    applyImGuiStyle(scale);
    const auto platform = ImGui_ImplWin32_Init(hwnd);
    const auto renderer = platform && ImGui_ImplDX9_Init(device.Get());
    if (renderer)
    {
        if (created)
        {
            created(hwnd);
        }

        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        renderLoop(hwnd, device.Get(), parameters, state, frame);
        ImGui_ImplDX9_Shutdown();
    }

    if (platform)
    {
        ImGui_ImplWin32_Shutdown();
    }

    ImGui::DestroyContext();
    DestroyWindow(hwnd);
    return renderer;
}

} // namespace tmloader
