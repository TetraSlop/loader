#pragma once
#include <windows.h>

#include <array>
#include <iosfwd>

namespace tmloader
{

struct WindowSettings
{
    int   width          = 640;
    int   height         = 480;
    bool  fullscreen     = false;
    bool  preserveAspect = true;
    float renderScale    = 1;
    bool  hdArtwork      = true;
    bool  hdFonts        = true;
    bool  autoResolution = true;
};

// 4:3 content fills a 1440p display at 3x and a 2160p display at 4.5x.
// The final preset allows supersampling on a 4K display.
inline constexpr std::array<float, 8> kRenderScales{ 1, 1.5f, 2, 2.25f, 3, 4, 4.5f, 6 };

auto validRenderScale(float scale) -> bool;
auto automaticRenderScale(unsigned width, unsigned height) -> float;
auto validWindowSize(int width, int height) -> bool;
// The game's logical coordinates remain 640x480 at every rendering resolution.
auto contentRect(LONG width, LONG height, bool preserveAspect) -> RECT;

// Malformed files yield defaults; invalid sizes or scales fall back individually.
auto readWindowSettings(std::istream& in) -> WindowSettings;
void writeWindowSettings(std::ostream& out, const WindowSettings& settings);

} // namespace tmloader
