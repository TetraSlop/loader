#include "ui/window_settings.h"
#include <algorithm>
#include <istream>
#include <nlohmann/json.hpp>
#include <ostream>

namespace tmloader
{

auto validRenderScale(const float scale) -> bool
{
    return std::ranges::contains(kRenderScales, scale);
}

auto automaticRenderScale(const unsigned width, const unsigned height) -> float
{
    if (!width || !height)
    {
        return 1;
    }

    const auto required = std::min(width / 640.0f, height / 480.0f);
    for (const auto scale : kRenderScales)
    {
        if (scale >= required || scale >= 4.5f)
        {
            return scale;
        }
    }

    return 4.5f; // Supersampling is a manual choice.
}

auto validWindowSize(const int width, const int height) -> bool
{
    return width >= 320 && width <= 7680 && height >= 240 && height <= 4320;
}

auto contentRect(const LONG width, const LONG height, const bool preserveAspect) -> RECT
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

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(WindowSettings, width, height, fullscreen, preserveAspect, renderScale, autoResolution)

auto readWindowSettings(std::istream& in) -> WindowSettings
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

void writeWindowSettings(std::ostream& out, const WindowSettings& settings)
{
    out << nlohmann::json(settings).dump(2) << '\n';
}

} // namespace tmloader
