#include "ui/window_settings.h"
#include <catch2/catch_test_macros.hpp>
#include <sstream>

using namespace tmloader;

void rect(LONG width, LONG height, bool aspect, RECT expected)
{
    const auto actual = contentRect(width, height, aspect);
    REQUIRE(EqualRect(&actual, &expected));
}

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

TEST_CASE("Window settings preserve saved preferences and legacy defaults", "[window_settings]")
{
    for (const auto* const malformed : { "", "oops", "tmloader-window-v1 -1 480 0 1", "tmloader-window-v1 640 0 0 1", "tmloader-window-v1 99999 480 0 1", "tmloader-window-v1 640 480 2 1", "tmloader-window-v1 640 480 0 9" })
    {
        std::istringstream in(malformed);
        const auto         value = readWindowSettings(in);
        REQUIRE((value.width == 640 && value.height == 480 && !value.fullscreen && value.preserveAspect));
        REQUIRE((value.autoResolution && value.hdFonts && value.hdTextures));
    }
    std::stringstream stream;
    writeWindowSettings(stream, { 1280, 960, true, false });
    auto value = readWindowSettings(stream);
    REQUIRE((value.width == 1280 && value.height == 960 && value.fullscreen && !value.preserveAspect));
    std::istringstream legacy("tmloader-window-v1 960 720 0 1");
    value = readWindowSettings(legacy);
    REQUIRE((value.width == 960 && value.height == 720 && value.renderScale == 1));
    REQUIRE(!value.autoResolution);
    for (const auto scale : renderScales)
    {
        std::stringstream saved;
        value.renderScale = scale;
        writeWindowSettings(saved, value);
        REQUIRE(readWindowSettings(saved).renderScale == scale);
    }
    std::stringstream hd;
    value.hdTextures = false;
    writeWindowSettings(hd, value);
    REQUIRE(!readWindowSettings(hd).hdTextures);
    std::istringstream previous("tmloader-window-v2 1280 960 1 0 4");
    value = readWindowSettings(previous);
    REQUIRE((value.renderScale == 4 && value.hdTextures));
    REQUIRE((value.hdFonts && !value.autoResolution));
    std::istringstream third("tmloader-window-v3 1280 960 1 0 3 0");
    value = readWindowSettings(third);
    REQUIRE((value.renderScale == 3 && !value.hdTextures));
    REQUIRE((!value.hdFonts && !value.autoResolution));
    std::istringstream fourth("tmloader-window-v4 1280 960 0 1 4.5 1");
    value = readWindowSettings(fourth);
    REQUIRE((value.renderScale == 4.5f && value.hdTextures && value.hdFonts && !value.autoResolution));
    for (const auto automatic : { false, true })
    {
        for (const auto artwork : { false, true })
        {
            for (const auto fonts : { false, true })
            {
                value.autoResolution = automatic;
                value.hdTextures     = artwork;
                value.hdFonts        = fonts;
                std::stringstream saved;
                writeWindowSettings(saved, value);
                const auto restored = readWindowSettings(saved);
                REQUIRE((restored.autoResolution == automatic && restored.hdTextures == artwork && restored.hdFonts == fonts));
                REQUIRE(restored.renderScale == 4.5f);
            }
        }
    }
    for (const auto* const flags : { "", "invalid", "2 8" })
    {
        std::istringstream invalid(std::string("tmloader-window-v5 960 720 0 1 2.25 0 ") + flags);
        value = readWindowSettings(invalid);
        REQUIRE((value.renderScale == 2.25f && !value.hdTextures && !value.hdFonts && value.autoResolution));
    }
    for (const auto* const scale : { "0", "-1", "5", "999999999999999999", "oops", "" })
    {
        std::istringstream invalid(std::string("tmloader-window-v2 960 720 0 1 ") + scale);
        value = readWindowSettings(invalid);
        REQUIRE((value.width == 960 && value.height == 720 && value.renderScale == 1));
    }
}

TEST_CASE("Window settings reject unknown versions and retain partial-field compatibility", "[window_settings]")
{
    for (const auto* const magic : { "tmloader-window-v0", "tmloader-window-v6", "tmloader-window-v01", "tmloader-window-v5extra", "tmloader-window-v", "other-window-v5" })
    {
        CAPTURE(magic);
        auto       input = std::istringstream{ std::string{ magic } + " 960 720 0 1 2.25 0 0 0" };
        const auto value = readWindowSettings(input);
        REQUIRE(value.width == 640);
        REQUIRE(value.height == 480);
        REQUIRE(value.renderScale == 1);
        REQUIRE(value.hdTextures);
        REQUIRE(value.hdFonts);
        REQUIRE(value.autoResolution);
    }

    SECTION("Invalid numeric scale preserves later preferences")
    {
        auto       input = std::istringstream{ "tmloader-window-v5 960 720 0 1 5 0 1 0" };
        const auto value = readWindowSettings(input);
        REQUIRE(value.width == 960);
        REQUIRE(value.renderScale == 1);
        REQUIRE_FALSE(value.hdTextures);
        REQUIRE(value.hdFonts);
        REQUIRE_FALSE(value.autoResolution);
    }

    SECTION("Unreadable scale stops optional-field parsing")
    {
        auto       input = std::istringstream{ "tmloader-window-v5 960 720 0 1 invalid 0 0 0" };
        const auto value = readWindowSettings(input);
        REQUIRE(value.width == 960);
        REQUIRE(value.renderScale == 1);
        REQUIRE(value.hdTextures);
        REQUIRE(value.hdFonts);
        REQUIRE(value.autoResolution);
    }

    SECTION("Invalid numeric artwork flag leaves later valid flags readable")
    {
        auto       input = std::istringstream{ "tmloader-window-v5 960 720 0 1 2.25 2 0 0" };
        const auto value = readWindowSettings(input);
        REQUIRE(value.renderScale == 2.25f);
        REQUIRE(value.hdTextures);
        REQUIRE_FALSE(value.hdFonts);
        REQUIRE_FALSE(value.autoResolution);
    }

    SECTION("Invalid numeric font flag retains the artwork default")
    {
        auto       input = std::istringstream{ "tmloader-window-v5 960 720 0 1 2.25 0 2 0" };
        const auto value = readWindowSettings(input);
        REQUIRE_FALSE(value.hdTextures);
        REQUIRE_FALSE(value.hdFonts);
        REQUIRE_FALSE(value.autoResolution);
    }
}
