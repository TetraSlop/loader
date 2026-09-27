#include "runtime.h"
#include "window.h"
#include <fstream>
#include <mutex>

namespace tmloader {
namespace {
WNDPROC originalWindowProc;
volatile LONG pendingMousePresses;
WindowSettings settings;
std::mutex settingsMutex;
WINDOWPLACEMENT windowedPlacement{sizeof(WINDOWPLACEMENT)};
bool changingWindow = false;

std::filesystem::path settingsFile() {
    return options.runtimeDir / "settings" / (options.polId + ".window");
}
void rememberSettings() {
    try {
        const auto path = settingsFile();
        if (!settingsPathAllowed(options, path)) throw std::runtime_error("Unsafe settings destination");
        std::filesystem::create_directories(path.parent_path());
        // Each account has its own file; only this window's thread writes it.
        std::ofstream out(path);
        writeWindowSettings(out, currentWindowSettings());
        if (!out) throw std::runtime_error("Cannot write window settings");
    } catch (const std::exception& e) { logf("Window settings: %s", e.what()); }
}
void rememberWindowSize(HWND hwnd) {
    RECT client{};
    if (changingWindow || currentWindowSettings().fullscreen || IsIconic(hwnd) || IsZoomed(hwnd) ||
        !GetClientRect(hwnd, &client) || !validWindowSize(client.right, client.bottom)) return;
    std::lock_guard<std::mutex> lock(settingsMutex);
    settings.width = client.right;
    settings.height = client.bottom;
}
void resizeClient(HWND hwnd, int width, int height) {
    RECT rect{0, 0, width, height};
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, GetWindowLongW(hwnd, GWL_EXSTYLE));
    SetWindowPos(hwnd, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
void fullscreen(HWND hwnd, bool enabled) {
    if (currentWindowSettings().fullscreen == enabled) return;
    changingWindow = true;
    if (enabled) {
        GetWindowPlacement(hwnd, &windowedPlacement);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        SetWindowLongW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, nullptr, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left,
                     monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        auto placement = windowedPlacement;
        if (settingsPanelOpen()) placement.showCmd = SW_SHOWNOACTIVATE;
        SetWindowPlacement(hwnd, &placement);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    { std::lock_guard<std::mutex> lock(settingsMutex); settings.fullscreen = enabled; }
    changingWindow = false;
    InvalidateRect(hwnd, nullptr, TRUE);
    rememberSettings();
}
void closeGame() {
    rememberSettings();
    stopSettingsPanel();
    logf("Closing standalone game window through the native message loop");
    // GameStart's PeekMessage loop exits on WM_QUIT (TM +18778E/+18798C).
    // The old WndProc swallowed SC_CLOSE, so the title-bar X never got here.
    PostQuitMessage(0);
}
void action(HWND hwnd, WindowAction command, LPARAM value) {
    switch (command) {
    case WindowAction::size: {
        int width = LOWORD(value), height = HIWORD(value);
        if (!validWindowSize(width, height)) break;
        fullscreen(hwnd, false);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        resizeClient(hwnd, width, height);
        rememberWindowSize(hwnd);
        rememberSettings();
        break;
    }
    case WindowAction::fullscreen: fullscreen(hwnd, value != 0); break;
    case WindowAction::aspect:
        { std::lock_guard<std::mutex> lock(settingsMutex); settings.preserveAspect = value != 0; }
        InvalidateRect(hwnd, nullptr, TRUE);
        rememberSettings();
        break;
    case WindowAction::close: closeGame(); break;
    case WindowAction::renderScale:
        if (validRenderScale(static_cast<int>(value))) {
            { std::lock_guard<std::mutex> lock(settingsMutex); settings.renderScale = static_cast<int>(value); }
            rememberSettings(); // Applied on next launch; no mid-match device reset.
        }
        break;
    }
}
LRESULT CALLBACK wrapperWindow(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    // The native WM_ACTIVATEAPP handler posts 0x64 after losing foreground.
    // Its recipient forcibly restores and foregrounds the game. A desktop
    // host must leave focus with the window the user actually selected.
    if (message == 0x64) return 0;
    if (message == WM_SETCURSOR) {
        if (reinterpret_cast<HWND>(wparam) == hwnd && LOWORD(lparam) == HTCLIENT) {
            POINT point{};
            RECT content = gameContentRect();
            bool gameCursor = GetForegroundWindow() == hwnd && !settingsPanelOpen() &&
                GetCursorPos(&point) && ScreenToClient(hwnd, &point) && PtInRect(&content, point);
            SetCursor(gameCursor ? nullptr : LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
            return TRUE;
        }
        return DefWindowProcA(hwnd, message, wparam, lparam);
    }
    if (message == settingsMessage) { action(hwnd, static_cast<WindowAction>(wparam), lparam); return 0; }
    if ((message == WM_KEYDOWN || message == WM_SYSKEYDOWN) && wparam == VK_F10) {
        if (!(lparam & (1L << 30))) toggleSettingsPanel();
        return 0;
    }
    if ((message == WM_KEYUP || message == WM_SYSKEYUP) && wparam == VK_F10) return 0;
    if (message == WM_SYSKEYDOWN && wparam == VK_F4) { closeGame(); return 0; }
    if (message == WM_SYSKEYDOWN && wparam == VK_RETURN) {
        if (!(lparam & (1L << 30))) fullscreen(hwnd, !currentWindowSettings().fullscreen);
        return 0;
    }
    if (message == WM_SYSKEYUP && wparam == VK_RETURN) return 0;
    if (message == WM_CLOSE) { closeGame(); return 0; }
    if (message == WM_SYSCOMMAND) {
        UINT command = static_cast<UINT>(wparam) & 0xfff0;
        if (command == SC_CLOSE) { closeGame(); return 0; }
        if (command == SC_SCREENSAVE || command == SC_MONITORPOWER || command == SC_KEYMENU) return 0;
        // The native fullscreen WndProc discards all system commands. Windows
        // must handle move/size/minimize/maximize/restore for a desktop host.
        return DefWindowProcA(hwnd, message, wparam, lparam);
    }
    if (message == WM_GETMINMAXINFO) {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        RECT minimum{0, 0, 320, 240};
        AdjustWindowRect(&minimum, WS_OVERLAPPEDWINDOW, FALSE);
        limits->ptMinTrackSize = {minimum.right - minimum.left, minimum.bottom - minimum.top};
        return 0;
    }
    if (message == WM_SIZE) {
        rememberWindowSize(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0; // The selected back buffer size stays fixed; only presentation changes.
    }
    if (message == WM_EXITSIZEMOVE) { rememberWindowSize(hwnd); rememberSettings(); return 0; }
    if (message == WM_ERASEBKGND) {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wparam), &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    if (message == WM_LBUTTONDOWN) InterlockedOr(&pendingMousePresses, 1);
    else if (message == WM_RBUTTONDOWN) InterlockedOr(&pendingMousePresses, 2);
    else if (message == WM_KILLFOCUS || (message == WM_ACTIVATEAPP && !wparam))
        InterlockedExchange(&pendingMousePresses, 0);
    return CallWindowProcA(originalWindowProc, hwnd, message, wparam, lparam);
}
}

WindowSettings currentWindowSettings() {
    std::lock_guard<std::mutex> lock(settingsMutex);
    return settings;
}
RECT gameContentRect() {
    RECT client{};
    GetClientRect(gameWindow, &client);
    return contentRect(client.right, client.bottom, currentWindowSettings().preserveAspect);
}
BYTE takeMousePresses() { return static_cast<BYTE>(InterlockedExchange(&pendingMousePresses, 0)); }
void postWindowAction(WindowAction command, LPARAM value) {
    PostMessageW(gameWindow, settingsMessage, static_cast<WPARAM>(command), value);
}
void initializeGameWindow(HWND hwnd) {
    gameWindow = hwnd;
    WindowSettings loaded;
    if (settingsPathAllowed(options, settingsFile())) {
        std::ifstream in(settingsFile());
        loaded = readWindowSettings(in);
    }
    { std::lock_guard<std::mutex> lock(settingsMutex); settings = loaded; settings.fullscreen = false; }
    originalWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(hwnd, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(wrapperWindow)));
    SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW);
    SetWindowTextW(hwnd, L"Tetra Master  |  F10 Settings");
    resizeClient(hwnd, loaded.width, loaded.height);
    // The original fullscreen window starts at (0,0); keep its caption on screen.
    SetWindowPos(hwnd, nullptr, 80, 80, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (loaded.fullscreen) fullscreen(hwnd, true);
    logf("Window controls installed: F10 settings, Alt+Enter fullscreen");
}
}
