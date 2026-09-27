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
    }
    std::stringstream stream;
    writeWindowSettings(stream, {1280, 960, true, false});
    auto value = readWindowSettings(stream);
    assert(value.width == 1280 && value.height == 960 && value.fullscreen && !value.preserveAspect);
    std::istringstream legacy("tmloader-window-v1 960 720 0 1");
    value = readWindowSettings(legacy);
    assert(value.width == 960 && value.height == 720 && value.renderScale == 1);
    for (int scale : {1, 2, 3, 4}) {
        std::stringstream saved;
        value.renderScale = scale;
        writeWindowSettings(saved, value);
        assert(readWindowSettings(saved).renderScale == scale);
    }
    for (const char* scale : {"0", "-1", "5", "999999999999999999", "oops", ""}) {
        std::istringstream invalid(std::string("tmloader-window-v2 960 720 0 1 ") + scale);
        value = readWindowSettings(invalid);
        assert(value.width == 960 && value.height == 720 && value.renderScale == 1);
    }
}
