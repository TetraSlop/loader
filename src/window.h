#pragma once
#include "window_settings.h"

namespace tmloader {
constexpr UINT settingsMessage = WM_APP + 0x54;
enum class WindowAction : WPARAM { size, fullscreen, aspect, close };
void initializeGameWindow(HWND window);
WindowSettings currentWindowSettings();
RECT gameContentRect();
void toggleSettingsPanel();
void stopSettingsPanel();
bool settingsPanelOpen();
// UI commands are dispatched on the game's window thread.
void postWindowAction(WindowAction action, LPARAM value = 0);
}
