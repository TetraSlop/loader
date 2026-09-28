#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "render_scale.h"
#include "runtime.h"
#include "screenshot.h"
#include "texture_replacements.h"
#include "window.h"
#include <algorithm>
#include <atomic>
#include <d3d9.h>
#include <thread>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace tmloader
{
namespace
{

std::atomic<bool> running{ false }, closeRequested{ false };
std::atomic<HWND> panelWindow{ nullptr };
// Used only on the panel thread, including its window procedure.
UINT resizedWidth = 0, resizedHeight = 0;

void configurePanel(float scale)
{
    auto& io       = ImGui::GetIO();
    io.IniFilename = nullptr; // Our account-scoped preferences are separate.
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Keep ImGui's normal cursor handling. Native cursor visibility is now
    // isolated to the game client area by the desktop adapter.
    ImGui::StyleColorsDark();
    auto& style         = ImGui::GetStyle();
    style.WindowPadding = ImVec2(18, 16);
    style.ItemSpacing   = ImVec2(10, 10);
    style.FrameRounding = 4;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

LRESULT CALLBACK panelProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE || ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) &&
                                (wparam == VK_F10 || wparam == VK_ESCAPE) && !(lparam & (1L << 30))))
    {
        closeRequested = true;
        return 0;
    }
    if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam))
        return 1;
    if (message == WM_SIZE && wparam != SIZE_MINIMIZED)
    {
        resizedWidth  = LOWORD(lparam);
        resizedHeight = HIWORD(lparam);
        return 0;
    }
    if (message == WM_SYSCOMMAND && (wparam & 0xfff0) == SC_KEYMENU)
        return 0;
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void displaySettings(WindowSettings value)
{
    ImGui::Spacing();
    if (ImGui::Checkbox("Borderless fullscreen", &value.fullscreen))
        postWindowAction(WindowAction::fullscreen, value.fullscreen);
    if (ImGui::Checkbox("Keep original 4:3 proportions", &value.preserveAspect))
        postWindowAction(WindowAction::aspect, value.preserveAspect);
    ImGui::Spacing();
    ImGui::TextUnformatted("Window size");
    if (ImGui::Button("640 x 480"))
        postWindowAction(WindowAction::size, MAKELPARAM(640, 480));
    ImGui::SameLine();
    if (ImGui::Button("960 x 720"))
        postWindowAction(WindowAction::size, MAKELPARAM(960, 720));
    ImGui::SameLine();
    if (ImGui::Button("1280 x 960"))
        postWindowAction(WindowAction::size, MAKELPARAM(1280, 960));
    ImGui::TextWrapped("You can also drag the game window's edges or maximize it.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Rendering resolution");
    int resolution = value.autoResolution ? 0 : 1 + static_cast<int>(std::find(renderScales.begin(), renderScales.end(), value.renderScale) - renderScales.begin());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##resolution", &resolution, "Automatic - match display\0"
                                                  "1x - 640 x 480 (original)\0"
                                                  "720p - 960 x 720\0"
                                                  "2x - 1280 x 960\0"
                                                  "1080p - 1440 x 1080\0"
                                                  "1440p - 1920 x 1440\0"
                                                  "4x - 2560 x 1920\0"
                                                  "4K - 2880 x 2160\0"
                                                  "Supersampled - 3840 x 2880\0"))
        postWindowAction(resolution == 0 ? WindowAction::autoResolution : WindowAction::renderScale, resolution - 1);
    float active = activeRenderScale();
    ImGui::TextDisabled("Currently rendering at %.0f x %.0f", 640 * active, 480 * active);
    ImGui::TextWrapped("Applies after relaunch. Automatic chooses the resolution for your monitor, up to 4K height.");
}

void artworkSettings(WindowSettings value)
{
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Artwork");
    if (ImGui::Checkbox("Enhanced cards and artwork", &value.hdTextures))
        postWindowAction(WindowAction::hdTextures, value.hdTextures);
    if (ImGui::Checkbox("Enhanced fonts", &value.hdFonts))
        postWindowAction(WindowAction::hdFonts, value.hdFonts);
    if (texturePackCount())
        ImGui::TextDisabled("%zu images available; %u replacements loaded", texturePackCount(), textureReplacementCount());
    else
        ImGui::TextDisabled("No HD texture pack loaded");
    ImGui::TextWrapped("Switches immediately. Images missing from the pack use their originals.");
    ImGui::TextWrapped("Font selection affects in-game text. Lettering painted into menus or card images follows the artwork setting.");
}

void screenshotSettings()
{
    ImGui::Spacing();
    ImGui::TextWrapped("Press F12 in the game to save a PNG at the active rendering resolution, without window borders or this panel.");
    ImGui::TextWrapped("Saved in runtime/logs/screenshots. Each capture gets its own filename.");
    if (ImGui::Button("Save screenshot and return"))
    {
        requestScreenshot();
        closeRequested = true;
    }
    const auto status = screenshotStatus();
    if (!status.empty())
        ImGui::TextWrapped("%s", status.c_str());
}

void settingsFooter()
{
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Audio");
    ImGui::TextWrapped("Adjust music and sound effect levels in the game's Options menu.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped("F10: settings   F12: screenshot   Alt+Enter: fullscreen");
    ImGui::TextWrapped("Window preferences are saved for this local account. Online matches continue while this panel is open.");
    ImGui::Spacing();
    if (ImGui::Button("Return to game"))
        closeRequested = true;
    ImGui::SameLine();
    if (ImGui::Button("Quit game"))
    {
        postWindowAction(WindowAction::close);
        closeRequested = true;
    }
}

void settingsContents()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    // Keep ImGui's vertical scrolling available when the monitor's work area
    // is shorter than the preferred panel; controls must remain reachable.
    ImGui::Begin("Wrapper settings", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("TETRA MASTER");
    ImGui::TextDisabled("Game settings");
    ImGui::Separator();
    const auto value = currentWindowSettings();
    if (ImGui::BeginTabBar("settings-tabs"))
    {
        if (ImGui::BeginTabItem("Display"))
        {
            displaySettings(value);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Artwork & text"))
        {
            artworkSettings(value);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Screenshots"))
        {
            screenshotSettings();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    settingsFooter();
    ImGui::End();
}

void panelMain()
{
    // The supported ImGui Win32/DX9 backends own a separate settings window.
    // Its message/render loop stays responsive if the legacy D3D8 game pauses.
    ImGui_ImplWin32_EnableDpiAwareness();
    HINSTANCE   instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{ sizeof(WNDCLASSEXW) };
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = panelProc;
    wc.hInstance     = instance;
    wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"TMLoaderSettings";
    RegisterClassExW(&wc);
    float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(gameWindow);
    RECT  owner{};
    GetWindowRect(gameWindow, &owner);
    int         width = static_cast<int>(540 * scale), height = static_cast<int>(770 * scale);
    int         x = owner.left + 40, y = owner.top + 40;
    MONITORINFO monitor{ sizeof(MONITORINFO) };
    if (GetMonitorInfoW(MonitorFromWindow(gameWindow, MONITOR_DEFAULTTONEAREST), &monitor))
    {
        const auto& work = monitor.rcWork;
        width            = (std::min)(width, static_cast<int>(work.right - work.left));
        height           = (std::min)(height, static_cast<int>(work.bottom - work.top));
        x                = std::clamp(x, static_cast<int>(work.left), static_cast<int>(work.right) - width);
        y                = std::clamp(y, static_cast<int>(work.top), static_cast<int>(work.bottom) - height);
    }
    HWND hwnd                    = CreateWindowExW(0, wc.lpszClassName, L"Tetra Master settings", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, width, height, gameWindow, nullptr, instance, nullptr);
    panelWindow                  = hwnd;
    IDirect3D9*           d3d    = hwnd ? Direct3DCreate9(D3D_SDK_VERSION) : nullptr;
    IDirect3DDevice9*     device = nullptr;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed             = TRUE;
    pp.SwapEffect           = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat     = D3DFMT_UNKNOWN;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    HRESULT hr              = d3d ? d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device) : E_FAIL;
    bool    platform = false, renderer = false;
    if (SUCCEEDED(hr))
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        configurePanel(scale);
        platform = ImGui_ImplWin32_Init(hwnd);
        renderer = platform && ImGui_ImplDX9_Init(device);
        if (!renderer)
            closeRequested = true;
        if (!closeRequested)
        {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
        }
        bool lost = false;
        while (!closeRequested && IsWindow(gameWindow))
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (closeRequested)
                break;
            if (lost)
            {
                hr = device->TestCooperativeLevel();
                if (hr == D3DERR_DEVICELOST)
                {
                    Sleep(20);
                    continue;
                }
            }
            if (resizedWidth || hr == D3DERR_DEVICENOTRESET)
            {
                if (resizedWidth)
                {
                    pp.BackBufferWidth  = resizedWidth;
                    pp.BackBufferHeight = resizedHeight;
                }
                ImGui_ImplDX9_InvalidateDeviceObjects();
                hr = device->Reset(&pp);
                if (FAILED(hr))
                {
                    lost = true;
                    Sleep(20);
                    continue;
                }
                ImGui_ImplDX9_CreateDeviceObjects();
                resizedWidth = resizedHeight = 0;
                lost                         = false;
            }
            ImGui_ImplDX9_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            settingsContents();
            ImGui::Render();
            device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(24, 26, 32), 1.0f, 0);
            if (SUCCEEDED(device->BeginScene()))
            {
                ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
                device->EndScene();
            }
            hr   = device->Present(nullptr, nullptr, nullptr, nullptr);
            lost = hr == D3DERR_DEVICELOST;
        }
        if (renderer)
            ImGui_ImplDX9_Shutdown();
        if (platform)
            ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    else
    {
        logf("Settings panel graphics initialization failed: %08lX", hr);
    }
    if (device)
        device->Release();
    if (d3d)
        d3d->Release();
    panelWindow = nullptr;
    if (hwnd)
        DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, instance);
    running = false;
}

} // namespace

bool settingsPanelOpen()
{
    return running;
}

void stopSettingsPanel()
{
    closeRequested = true;
    if (auto hwnd = panelWindow.load())
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

void toggleSettingsPanel()
{
    bool expected = false;
    if (!running.compare_exchange_strong(expected, true))
    {
        stopSettingsPanel();
        return;
    }
    closeRequested = false;
    try
    {
        std::thread(panelMain).detach();
    }
    catch (const std::exception& e)
    {
        running = false;
        logf("Cannot open settings: %s", e.what());
    }
}

} // namespace tmloader
