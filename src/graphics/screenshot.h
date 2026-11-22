#pragma once
#include <string>

namespace tmloader
{

// May be called by the game-window hotkey or settings UI, and at fixed startup
// frames with --diagnostics. Repeated pending requests coalesce into one frame.
void requestScreenshot() noexcept;
// Call on the render thread after drawing and immediately before Present.
// The backbuffer is read at its actual internal resolution, without desktop UI.
void saveRequestedScreenshot(void* device) noexcept;
// UTF-8 text for the settings window; empty until the first request.
auto screenshotStatus() -> std::string;

} // namespace tmloader
