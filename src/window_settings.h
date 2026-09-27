#pragma once
#include <windows.h>
#include <istream>
#include <ostream>
#include <string>
#include <array>
#include <algorithm>

namespace tmloader {
struct WindowSettings {
    int width = 640, height = 480;
    bool fullscreen = false;
    bool preserveAspect = true;
    float renderScale = 1;
    bool hdTextures = true;
    bool hdFonts = true;
    bool autoResolution = true;
};

// 4:3 content fills a 1440p display at 3x and a 2160p display at 4.5x.
// The final preset allows supersampling on a 4K display.
inline constexpr std::array<float, 8> renderScales{1, 1.5f, 2, 2.25f, 3, 4, 4.5f, 6};
inline bool validRenderScale(float factor) {
    return std::find(renderScales.begin(), renderScales.end(), factor) != renderScales.end();
}

inline float automaticRenderScale(unsigned width, unsigned height) {
    if (!width || !height) return 1;
    const float required = (std::min)(width / 640.0f, height / 480.0f);
    for (float factor : renderScales) {
        if (factor >= required || factor >= 4.5f) return factor;
    }
    return 4.5f; // Supersampling is a manual choice.
}

inline bool validWindowSize(int width, int height) {
    return width >= 320 && width <= 7680 && height >= 240 && height <= 4320;
}

// The game's logical coordinates remain 640x480 at every rendering resolution.
inline RECT contentRect(LONG width, LONG height, bool preserveAspect) {
    if (width <= 0 || height <= 0) return {};
    LONG w = width, h = height;
    if (preserveAspect) {
        if (static_cast<long long>(width) * 3 > static_cast<long long>(height) * 4)
            w = MulDiv(height, 4, 3);
        else
            h = MulDiv(width, 3, 4);
    }
    LONG x = (width - w) / 2, y = (height - h) / 2;
    return {x, y, x + w, y + h};
}

inline WindowSettings readWindowSettings(std::istream& in) {
    WindowSettings result;
    std::string magic;
    int width, height, fullscreen, aspect;
    if ((in >> magic >> width >> height >> fullscreen >> aspect) &&
        (magic == "tmloader-window-v1" || magic == "tmloader-window-v2" ||
         magic == "tmloader-window-v3" || magic == "tmloader-window-v4" || magic == "tmloader-window-v5") &&
        validWindowSize(width, height) && (fullscreen == 0 || fullscreen == 1) && (aspect == 0 || aspect == 1)) {
        result = {width, height, fullscreen != 0, aspect != 0};
        // Existing accounts keep their chosen resolution until they select Auto.
        result.autoResolution = magic == "tmloader-window-v5";
        float scale;
        if (magic != "tmloader-window-v1" && (in >> scale) && validRenderScale(scale))
            result.renderScale = scale;
        int hd;
        if ((magic == "tmloader-window-v3" || magic == "tmloader-window-v4" || magic == "tmloader-window-v5") &&
            (in >> hd) && (hd == 0 || hd == 1))
            result.hdTextures = hd != 0;
        result.hdFonts = result.hdTextures;
        if (magic == "tmloader-window-v5") {
            int fonts, automatic;
            if ((in >> fonts) && (fonts == 0 || fonts == 1)) result.hdFonts = fonts != 0;
            if ((in >> automatic) && (automatic == 0 || automatic == 1)) result.autoResolution = automatic != 0;
        }
    }
    return result;
}
inline void writeWindowSettings(std::ostream& out, const WindowSettings& value) {
    out << "tmloader-window-v5\n" << value.width << ' ' << value.height << ' '
        << value.fullscreen << ' ' << value.preserveAspect << ' ' << value.renderScale << ' '
        << value.hdTextures << ' ' << value.hdFonts << ' ' << value.autoResolution << '\n';
}
}
