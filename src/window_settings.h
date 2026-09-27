#pragma once
#include <windows.h>
#include <istream>
#include <ostream>
#include <string>

namespace tmloader {
struct WindowSettings {
    int width = 640, height = 480;
    bool fullscreen = false;
    bool preserveAspect = true;
    int renderScale = 1;
};

inline bool validRenderScale(int factor) { return factor >= 1 && factor <= 4; }

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
        (magic == "tmloader-window-v1" || magic == "tmloader-window-v2") &&
        validWindowSize(width, height) && (fullscreen == 0 || fullscreen == 1) && (aspect == 0 || aspect == 1)) {
        result = {width, height, fullscreen != 0, aspect != 0};
        int scale;
        if (magic == "tmloader-window-v2" && (in >> scale) && validRenderScale(scale))
            result.renderScale = scale;
    }
    return result;
}
inline void writeWindowSettings(std::ostream& out, const WindowSettings& value) {
    out << "tmloader-window-v2\n" << value.width << ' ' << value.height << ' '
        << value.fullscreen << ' ' << value.preserveAspect << ' ' << value.renderScale << '\n';
}
}
