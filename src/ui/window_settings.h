#pragma once
#include <windows.h>

#include <algorithm>
#include <array>
#include <istream>
#include <nlohmann/json.hpp>
#include <ostream>

namespace tmloader
{

struct WindowSettings
{
    int   width          = 640;
    int   height         = 480;
    bool  fullscreen     = false;
    bool  preserveAspect = true;
    float renderScale    = 1;
    bool  hdTextures     = true;
    bool  hdFonts        = true;
    bool  autoResolution = true;
};

// 4:3 content fills a 1440p display at 3x and a 2160p display at 4.5x.
// The final preset allows supersampling on a 4K display.
inline constexpr std::array<float, 8> renderScales{ 1, 1.5f, 2, 2.25f, 3, 4, 4.5f, 6 };

inline auto validRenderScale(const float factor) -> bool
{
    return std::ranges::contains(renderScales, factor);
}

inline auto automaticRenderScale(const unsigned width, const unsigned height) -> float
{
    if (!width || !height)
    {
        return 1;
    }

    const auto required = std::min(width / 640.0f, height / 480.0f);
    for (const auto factor : renderScales)
    {
        if (factor >= required || factor >= 4.5f)
        {
            return factor;
        }
    }

    return 4.5f; // Supersampling is a manual choice.
}

inline auto validWindowSize(const int width, const int height) -> bool
{
    return width >= 320 && width <= 7680 && height >= 240 && height <= 4320;
}

// The game's logical coordinates remain 640x480 at every rendering resolution.
inline auto contentRect(const LONG width, const LONG height, const bool preserveAspect) -> RECT
{
    if (width <= 0 || height <= 0)
    {
        return {};
    }

    auto w = width;
    auto h = height;
    if (preserveAspect)
    {
        if (static_cast<long long>(width) * 3 > static_cast<long long>(height) * 4)
        {
            w = MulDiv(height, 4, 3);
        }
        else
        {
            h = MulDiv(width, 3, 4);
        }
    }

    const auto x = (width - w) / 2;
    const auto y = (height - h) / 2;
    return { x, y, x + w, y + h };
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WindowSettings, width, height, fullscreen, preserveAspect, renderScale, hdTextures, hdFonts, autoResolution)

// Malformed files yield defaults; invalid sizes or scales fall back individually.
inline auto readWindowSettings(std::istream& in) -> WindowSettings
{
    auto settings = WindowSettings{};
    try
    {
        settings = nlohmann::json::parse(in).get<WindowSettings>();
    }
    catch (const nlohmann::json::exception&)
    {
        return {};
    }

    const auto defaults = WindowSettings{};
    if (!validWindowSize(settings.width, settings.height))
    {
        settings.width  = defaults.width;
        settings.height = defaults.height;
    }

    if (!validRenderScale(settings.renderScale))
    {
        settings.renderScale = defaults.renderScale;
    }

    return settings;
}

inline void writeWindowSettings(std::ostream& out, const WindowSettings& settings)
{
    out << nlohmann::json(settings).dump(2) << '\n';
}

} // namespace tmloader
