#ifdef NDEBUG
#undef NDEBUG
#endif
#include "window_settings.h"
#include <cassert>
#include <sstream>

using namespace tmloader;
void rect(LONG width, LONG height, bool aspect, RECT expected) {
    RECT actual = contentRect(width, height, aspect);
    assert(EqualRect(&actual, &expected));
}
int main() {
    assert(automaticRenderScale(0, 0) == 1);
    assert(automaticRenderScale(640, 480) == 1);
    assert(automaticRenderScale(1280, 720) == 1.5f);
    assert(automaticRenderScale(1920, 1080) == 2.25f);
    assert(automaticRenderScale(2560, 1440) == 3);
    assert(automaticRenderScale(3440, 1440) == 3);
    assert(automaticRenderScale(2560, 1600) == 4);
    assert(automaticRenderScale(3840, 2160) == 4.5f);
    assert(automaticRenderScale(5120, 2160) == 4.5f);
    assert(automaticRenderScale(1440, 2560) == 2.25f);
    assert(automaticRenderScale(7680, 4320) == 4.5f);
    rect(640, 480, true, {0, 0, 640, 480});
    rect(1920, 1080, true, {240, 0, 1680, 1080});
    rect(800, 1000, true, {0, 200, 800, 800});
    rect(1920, 1080, false, {0, 0, 1920, 1080});
    rect(0, 0, true, {});
    for (const char* malformed : {"", "oops", "tmloader-window-v1 -1 480 0 1",
        "tmloader-window-v1 640 0 0 1", "tmloader-window-v1 99999 480 0 1",
        "tmloader-window-v1 640 480 2 1", "tmloader-window-v1 640 480 0 9"}) {
        std::istringstream in(malformed);
        auto value = readWindowSettings(in);
        assert(value.width == 640 && value.height == 480 && !value.fullscreen && value.preserveAspect);
        assert(value.autoResolution && value.hdFonts && value.hdTextures);
    }
    std::stringstream stream;
    writeWindowSettings(stream, {1280, 960, true, false});
    auto value = readWindowSettings(stream);
    assert(value.width == 1280 && value.height == 960 && value.fullscreen && !value.preserveAspect);
    std::istringstream legacy("tmloader-window-v1 960 720 0 1");
    value = readWindowSettings(legacy);
    assert(value.width == 960 && value.height == 720 && value.renderScale == 1);
    assert(!value.autoResolution);
    for (float scale : renderScales) {
        std::stringstream saved;
        value.renderScale = scale;
        writeWindowSettings(saved, value);
        assert(readWindowSettings(saved).renderScale == scale);
    }
    std::stringstream hd;
    value.hdTextures = false;
    writeWindowSettings(hd, value);
    assert(!readWindowSettings(hd).hdTextures);
    std::istringstream previous("tmloader-window-v2 1280 960 1 0 4");
    value = readWindowSettings(previous);
    assert(value.renderScale == 4 && value.hdTextures);
    assert(value.hdFonts && !value.autoResolution);
    std::istringstream third("tmloader-window-v3 1280 960 1 0 3 0");
    value = readWindowSettings(third);
    assert(value.renderScale == 3 && !value.hdTextures);
    assert(!value.hdFonts && !value.autoResolution);
    std::istringstream fourth("tmloader-window-v4 1280 960 0 1 4.5 1");
    value = readWindowSettings(fourth);
    assert(value.renderScale == 4.5f && value.hdTextures && value.hdFonts && !value.autoResolution);
    for (bool automatic : {false, true}) {
        for (bool artwork : {false, true}) {
            for (bool fonts : {false, true}) {
                value.autoResolution = automatic; value.hdTextures = artwork; value.hdFonts = fonts;
                std::stringstream saved;
                writeWindowSettings(saved, value);
                auto restored = readWindowSettings(saved);
                assert(restored.autoResolution == automatic && restored.hdTextures == artwork && restored.hdFonts == fonts);
                assert(restored.renderScale == 4.5f);
            }
        }
    }
    for (const char* flags : {"", "invalid", "2 8"}) {
        std::istringstream invalid(std::string("tmloader-window-v5 960 720 0 1 2.25 0 ") + flags);
        value = readWindowSettings(invalid);
        assert(value.renderScale == 2.25f && !value.hdTextures && !value.hdFonts && value.autoResolution);
    }
    for (const char* scale : {"0", "-1", "5", "999999999999999999", "oops", ""}) {
        std::istringstream invalid(std::string("tmloader-window-v2 960 720 0 1 ") + scale);
        value = readWindowSettings(invalid);
        assert(value.width == 960 && value.height == 720 && value.renderScale == 1);
    }
}
