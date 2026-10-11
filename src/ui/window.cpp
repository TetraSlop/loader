#include "ui/window.h"
#include "app/log.h"
#include "app/runtime.h"
#include "graphics/screenshot.h"
#include <fstream>
#include <mutex>

namespace tmloader
{
namespace
{

// Posted by the game's WM_ACTIVATEAPP handler to force itself back to the foreground.
constexpr UINT kReclaimFocusMessage = 0x64;
// Carries a WindowAction to the game's window thread.
constexpr UINT kSettingsMessage = WM_APP + 0x54;

WNDPROC         originalWindowProc;
volatile LONG   pendingMousePresses;
WindowSettings  settings;
std::mutex      settingsMutex;
WINDOWPLACEMENT windowedPlacement{ sizeof(WINDOWPLACEMENT) };
bool            changingWindow = false;

auto settingsFile() -> std::filesystem::path
{
    return options.runtimeDir / "settings" / (options.polId + ".json");
}

void saveSettings()
{
    try
    {
        const auto path = settingsFile();
        if (!settingsPathAllowed(options, path))
        {
            throw std::runtime_error("Unsafe settings destination");
        }

        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path);
        writeWindowSettings(out, currentWindowSettings());
        if (!out)
        {
            throw std::runtime_error("Cannot write window settings");
        }
    }
    catch (const std::exception& e)
    {
        logLine("Window settings: {}", e.what());
    }
}

void rememberWindowSize(HWND hwnd)
{
    RECT client{};
    if (changingWindow || currentWindowSettings().fullscreen || IsIconic(hwnd) || IsZoomed(hwnd) ||
        !GetClientRect(hwnd, &client) || !validWindowSize(client.right, client.bottom))
    {
        return;
    }

    const std::lock_guard lock(settingsMutex);
    settings.width  = client.right;
    settings.height = client.bottom;
}

void resizeClient(HWND hwnd, int width, int height)
{
    RECT rect{ 0, 0, width, height };
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, GetWindowLongW(hwnd, GWL_EXSTYLE));
    SetWindowPos(hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void setFullscreen(HWND hwnd, bool enabled)
{
    if (currentWindowSettings().fullscreen == enabled)
    {
        return;
    }

    changingWindow = true;
    if (enabled)
    {
        GetWindowPlacement(hwnd, &windowedPlacement);
        MONITORINFO monitor{ sizeof(MONITORINFO) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, nullptr, monitor.rcMonitor.left, monitor.rcMonitor.top, monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    else
    {
        SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        auto placement = windowedPlacement;
        if (settingsPanelOpen())
        {
            placement.showCmd = SW_SHOWNOACTIVATE;
        }

        SetWindowPlacement(hwnd, &placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    {
        const std::lock_guard lock(settingsMutex);
        settings.fullscreen = enabled;
    }

    changingWindow = false;
    InvalidateRect(hwnd, nullptr, TRUE);
    saveSettings();
}

void closeGame()
{
    saveSettings();
    stopSettingsPanel();
    logLine("Quit requested");
    // GameStart's message loop exits on WM_QUIT (+18778E/+18798C).
    PostQuitMessage(0);
}

auto isRepeat(LPARAM lparam) -> bool
{
    return (lparam & (1L << 30)) != 0;
}

template <typename Update>
void updateSettings(Update update)
{
    {
        const std::lock_guard lock(settingsMutex);
        update(settings);
    }

    saveSettings();
}

void applyWindowAction(HWND hwnd, WindowAction command, LPARAM value)
{
    switch (command)
    {
        case WindowAction::size:
        {
            const auto width  = static_cast<int>(LOWORD(value));
            const auto height = static_cast<int>(HIWORD(value));
            if (!validWindowSize(width, height))
            {
                break;
            }

            setFullscreen(hwnd, false);
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            resizeClient(hwnd, width, height);
            rememberWindowSize(hwnd);
            saveSettings();
            break;
        }

        case WindowAction::fullscreen:
            setFullscreen(hwnd, value != 0);
            break;
        case WindowAction::aspect:
            updateSettings([&](WindowSettings& current)
                           {
                               current.preserveAspect = value != 0;
                           });
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        case WindowAction::close:
            closeGame();
            break;
        case WindowAction::hdArtwork:
            updateSettings([&](WindowSettings& current)
                           {
                               current.hdArtwork = value != 0;
                           });
            break;
        case WindowAction::hdFonts:
            updateSettings([&](WindowSettings& current)
                           {
                               current.hdFonts = value != 0;
                           });
            break;
        case WindowAction::renderScale:
            // Resolution applies on the next launch, never as a mid-match device reset.
            if (value >= 0 && static_cast<size_t>(value) < kRenderScales.size())
            {
                updateSettings([&](WindowSettings& current)
                               {
                                   current.renderScale    = kRenderScales[value];
                                   current.autoResolution = false;
                               });
            }

            break;
        case WindowAction::autoResolution:
            updateSettings([](WindowSettings& current)
                           {
                               current.autoResolution = true;
                           });
            break;
    }
}

auto CALLBACK gameWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) -> LRESULT
{
    if (message == kReclaimFocusMessage)
    {
        return 0;
    }

    if (message == WM_SETCURSOR)
    {
        if (reinterpret_cast<HWND>(wparam) == hwnd && LOWORD(lparam) == HTCLIENT)
        {
            // The game draws its own cursor inside the content area.
            auto       point      = POINT{};
            const auto content    = gameContentRect();
            const auto gameCursor = GetForegroundWindow() == hwnd && !settingsPanelOpen() &&
                                    GetCursorPos(&point) && ScreenToClient(hwnd, &point) && PtInRect(&content, point);
            if (gameCursor)
            {
                SetCursor(nullptr);
            }
            else
            {
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512))); // IDC_ARROW
            }

            return TRUE;
        }

        return DefWindowProcA(hwnd, message, wparam, lparam);
    }

    if (message == kSettingsMessage)
    {
        applyWindowAction(hwnd, static_cast<WindowAction>(wparam), lparam);
        return 0;
    }

    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && wparam == VK_F10)
    {
        if (!isRepeat(lparam))
        {
            toggleSettingsPanel();
        }

        return 0;
    }

    if ((message == WM_KEYUP || message == WM_SYSKEYUP) && wparam == VK_F10)
    {
        return 0;
    }

    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && wparam == VK_F12)
    {
        if (!isRepeat(lparam))
        {
            requestScreenshot();
        }

        return 0;
    }

    if ((message == WM_KEYUP || message == WM_SYSKEYUP) && wparam == VK_F12)
    {
        return 0;
    }

    if (message == WM_SYSKEYDOWN && wparam == VK_F4)
    {
        closeGame();
        return 0;
    }

    if (message == WM_SYSKEYDOWN && wparam == VK_RETURN)
    {
        if (!isRepeat(lparam))
        {
            setFullscreen(hwnd, !currentWindowSettings().fullscreen);
        }

        return 0;
    }

    if (message == WM_SYSKEYUP && wparam == VK_RETURN)
    {
        return 0;
    }

    if (message == WM_CLOSE)
    {
        closeGame();
        return 0;
    }

    if (message == WM_SYSCOMMAND)
    {
        const auto command = static_cast<UINT>(wparam) & 0xfff0;
        if (command == SC_CLOSE)
        {
            closeGame();
            return 0;
        }

        if (command == SC_SCREENSAVE || command == SC_MONITORPOWER || command == SC_KEYMENU)
        {
            return 0;
        }

        // The game's window procedure discards system commands, disabling move and resize.
        return DefWindowProcA(hwnd, message, wparam, lparam);
    }

    if (message == WM_GETMINMAXINFO)
    {
        auto* const limits = reinterpret_cast<MINMAXINFO*>(lparam);
        RECT        minimum{ 0, 0, 320, 240 };
        AdjustWindowRect(&minimum, WS_OVERLAPPEDWINDOW, FALSE);
        limits->ptMinTrackSize = { minimum.right - minimum.left, minimum.bottom - minimum.top };
        return 0;
    }

    if (message == WM_SIZE)
    {
        rememberWindowSize(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0; // The back buffer keeps its size; only presentation is rescaled.
    }

    if (message == WM_EXITSIZEMOVE)
    {
        rememberWindowSize(hwnd);
        saveSettings();
        return 0;
    }

    if (message == WM_ERASEBKGND)
    {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wparam), &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }

    if (message == WM_LBUTTONDOWN)
    {
        InterlockedOr(&pendingMousePresses, LeftButton);
    }
    else if (message == WM_RBUTTONDOWN)
    {
        InterlockedOr(&pendingMousePresses, RightButton);
    }
    else if (message == WM_KILLFOCUS || (message == WM_ACTIVATEAPP && !wparam))
    {
        InterlockedExchange(&pendingMousePresses, 0);
    }

    return CallWindowProcA(originalWindowProc, hwnd, message, wparam, lparam);
}

} // namespace

auto currentWindowSettings() -> WindowSettings
{
    const std::lock_guard lock(settingsMutex);
    return settings;
}

auto gameContentRect() -> RECT
{
    RECT client{};
    GetClientRect(gameWindow, &client);
    return contentRect(client.right, client.bottom, currentWindowSettings().preserveAspect);
}

auto takeMousePresses() -> BYTE
{
    return static_cast<BYTE>(InterlockedExchange(&pendingMousePresses, 0));
}

void postWindowAction(WindowAction command, LPARAM value)
{
    PostMessageW(gameWindow, kSettingsMessage, static_cast<WPARAM>(command), value);
}

void initializeGameWindow(HWND hwnd)
{
    // The game may create a second device; subclassing twice would forward to ourselves.
    if (hwnd == gameWindow && originalWindowProc &&
        GetWindowLongPtrA(hwnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(gameWindowProc))
    {
        return;
    }

    gameWindow = hwnd;
    WindowSettings loaded;
    if (settingsPathAllowed(options, settingsFile()))
    {
        std::ifstream in(settingsFile());
        loaded = readWindowSettings(in);
    }

    {
        const std::lock_guard lock(settingsMutex);
        settings            = loaded;
        settings.fullscreen = false;
    }

    originalWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(gameWindowProc)));
    SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
    SetWindowTextW(hwnd, L"Tetra Master  |  F10 Settings");
    resizeClient(hwnd, loaded.width, loaded.height);
    // The original fullscreen window starts at (0,0); keep its caption on screen.
    SetWindowPos(hwnd, nullptr, 80, 80, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (loaded.fullscreen)
    {
        setFullscreen(hwnd, true);
    }

    logLine("Window controls installed: F10 settings, F12 screenshot, Alt+Enter fullscreen");
}

} // namespace tmloader
