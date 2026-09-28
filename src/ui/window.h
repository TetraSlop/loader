#pragma once
#include "ui/window_settings.h"

namespace tmloader
{

constexpr UINT settingsMessage = WM_APP + 0x54;
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
// UI commands are dispatched on the game's window thread. renderScale takes
// an index into renderScales, so fractional scales need no message encoding.
void postWindowAction(WindowAction action, LPARAM value = 0);

} // namespace tmloader
