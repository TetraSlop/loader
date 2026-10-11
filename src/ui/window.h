#pragma once
#include "ui/window_settings.h"

namespace tmloader
{

constexpr UINT kSettingsMessage = WM_APP + 0x54;

enum class WindowAction : WPARAM
{
    size,
    fullscreen,
    aspect,
    close,
    renderScale,
    hdTextures,
    hdFonts,
    autoResolution
};

void initializeGameWindow(HWND window);
auto currentWindowSettings() -> WindowSettings;
auto gameContentRect() -> RECT;
void toggleSettingsPanel();
void stopSettingsPanel();
auto settingsPanelOpen() -> bool;
// Returns and clears clicks seen since the last call (bit 0 left, bit 1 right).
auto takeMousePresses() -> BYTE;
// Runs the action on the game's window thread. renderScale takes an index into renderScales.
void postWindowAction(WindowAction action, LPARAM value = 0);

} // namespace tmloader
