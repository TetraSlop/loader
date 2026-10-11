#pragma once
#include "ui/window_settings.h"

namespace tmloader
{

enum MouseButton : BYTE
{
    LeftButton  = 1,
    RightButton = 2,
};

enum class WindowAction : WPARAM
{
    size,
    fullscreen,
    aspect,
    close,
    renderScale,
    hdArtwork,
    hdFonts,
    autoResolution
};

void initializeGameWindow(HWND window);
auto currentWindowSettings() -> WindowSettings;
auto gameContentRect() -> RECT;
void toggleSettingsPanel();
void stopSettingsPanel();
auto settingsPanelOpen() -> bool;
// Returns and clears the MouseButton clicks seen since the last call.
auto takeMousePresses() -> BYTE;
// Runs the action on the game's window thread. renderScale takes an index into renderScales.
void postWindowAction(WindowAction action, LPARAM value = 0);

} // namespace tmloader
