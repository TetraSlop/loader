#include "ui/window_settings.h"
#include <catch2/catch_test_macros.hpp>
#include <sstream>

using namespace tmloader;

namespace
{

void rect(const LONG width, const LONG height, const bool aspect, const RECT expected)
{
    const auto actual = contentRect(width, height, aspect);
    REQUIRE(EqualRect(&actual, &expected));
}

} // namespace

TEST_CASE("Automatic rendering chooses the monitor-sized preset", "[window_settings]")
{
    REQUIRE(automaticRenderScale(0, 0) == 1);
    REQUIRE(automaticRenderScale(640, 480) == 1);
    REQUIRE(automaticRenderScale(1280, 720) == 1.5f);
    REQUIRE(automaticRenderScale(1920, 1080) == 2.25f);
    REQUIRE(automaticRenderScale(2560, 1440) == 3);
    REQUIRE(automaticRenderScale(3440, 1440) == 3);
    REQUIRE(automaticRenderScale(2560, 1600) == 4);
    REQUIRE(automaticRenderScale(3840, 2160) == 4.5f);
    REQUIRE(automaticRenderScale(5120, 2160) == 4.5f);
    REQUIRE(automaticRenderScale(1440, 2560) == 2.25f);
    REQUIRE(automaticRenderScale(7680, 4320) == 4.5f);
}

TEST_CASE("Content rectangles preserve optional aspect ratios", "[window_settings]")
{
    rect(640, 480, true, { 0, 0, 640, 480 });
    rect(1920, 1080, true, { 240, 0, 1680, 1080 });
    rect(800, 1000, true, { 0, 200, 800, 800 });
    rect(1920, 1080, false, { 0, 0, 1920, 1080 });
    rect(0, 0, true, {});
}

TEST_CASE("Window settings round-trip through JSON", "[window_settings]")
{
    auto value           = WindowSettings{ 1280, 960, true, false };
    value.renderScale    = 4.5f;
    value.hdArtwork      = false;
    value.autoResolution = false;
    auto stream          = std::stringstream{};
    writeWindowSettings(stream, value);
    const auto restored = readWindowSettings(stream);
    REQUIRE(restored.width == 1280);
    REQUIRE(restored.height == 960);
    REQUIRE(restored.fullscreen);
    REQUIRE_FALSE(restored.preserveAspect);
    REQUIRE(restored.renderScale == 4.5f);
    REQUIRE_FALSE(restored.hdArtwork);
    REQUIRE(restored.hdFonts);
    REQUIRE_FALSE(restored.autoResolution);
}

TEST_CASE("Window settings fall back to defaults for bad input", "[window_settings]")
{
    const auto defaults = WindowSettings{};
    for (const auto* const text : { "", "oops", "[]", R"({"width":"wide"})" })
    {
        CAPTURE(text);
        auto       input = std::istringstream{ text };
        const auto value = readWindowSettings(input);
        REQUIRE(value.width == defaults.width);
        REQUIRE(value.renderScale == defaults.renderScale);
        REQUIRE(value.autoResolution == defaults.autoResolution);
    }

    auto       input = std::istringstream{ R"({"width":99999,"height":480,"renderScale":5,"hdFonts":false})" };
    const auto value = readWindowSettings(input);
    REQUIRE(value.width == defaults.width);
    REQUIRE(value.height == defaults.height);
    REQUIRE(value.renderScale == defaults.renderScale);
    REQUIRE_FALSE(value.hdFonts);
}
