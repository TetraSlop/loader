#include "app/runtime.h"
#include "imgui_internal.h"
#include "ui/settings_ui.cpp"
#include "ui/window.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>

namespace
{

constexpr float  kScales[]       = { 1.0f, 1.5f, 2.0f };
constexpr float  kPanelWidth     = 524;
constexpr float  kPanelHeight    = 730;
constexpr float  kShortHeight    = 260;
constexpr size_t kPackImageCount = 503;

const char* const kTabs[] = {
    "Display",
#ifdef TMLOADER_HD_TEXTURES
    "Artwork & text",
#endif
    "Screenshots",
};

const std::string kSavedStatus = "Screenshot saved: C:/Users/Player/Documents/GitHub/tmloader/runtime/logs/screenshots/" +
                                 std::string(180, 'A') + "-20260927-234959-999-player-EEEEEEEF-000001.png";

const std::string kFailedStatus = "Screenshot failed: Cannot write the PNG to C:/Users/Player/Documents/GitHub/tmloader/runtime/logs/screenshots/" +
                                  std::string(220, 'B') + ".png. The destination folder is unavailable; please check its permissions and try again.";

tmloader::WindowSettings testSettings;
std::string              captureStatus;
unsigned                 screenshotRequests;
unsigned                 statusReads;
size_t                   availableImages = kPackImageCount;

} // namespace

namespace tmloader
{

HWND gameWindow{};

void writeLog(std::string_view)
{
}

auto currentWindowSettings() -> WindowSettings
{
    return testSettings;
}

auto activeRenderScale() -> float
{
    return 4.5f;
}

auto texturePackCount() -> size_t
{
    return availableImages;
}

auto textureReplacementCount() -> unsigned
{
    return 5;
}

void postWindowAction(WindowAction, LPARAM)
{
}

void requestScreenshot() noexcept
{
    ++screenshotRequests;
}

auto screenshotStatus() -> std::string
{
    ++statusReads;
    return captureStatus;
}

} // namespace tmloader

namespace
{

void frame()
{
    ImGui::NewFrame();
    tmloader::settingsContents();
    ImGui::Render();
    REQUIRE(ImGui::GetDrawData()->Valid);
    REQUIRE(ImGui::GetDrawData()->TotalVtxCount > 0);
}

// Tab transitions and scrollbar visibility use previous-frame measurements.
void settle()
{
    for (int i = 0; i < 3; ++i)
    {
        frame();
    }
}

auto selectTab(const char* label, ImVec2 size) -> ImGuiWindow*
{
    auto& io       = ImGui::GetIO();
    io.DisplaySize = size;
    frame();
    auto* const window = ImGui::FindWindowByName("Settings");
    REQUIRE(window);
    auto* const tabs = ImGui::GetCurrentContext()->TabBars.GetByKey(window->GetID("settings-tabs"));
    REQUIRE(tabs);
    REQUIRE(tabs->Tabs.Size == static_cast<int>(std::size(kTabs)));
    ImGuiID selected = 0;
    for (auto& tab : tabs->Tabs)
    {
        if (std::strcmp(ImGui::TabBarGetTabName(tabs, &tab), label) == 0)
        {
            selected = tab.ID;
        }
    }

    REQUIRE(selected);
    tabs->NextSelectedTabId = selected;
    ImGui::SetScrollY(window, 0);
    settle();

    REQUIRE(tabs->VisibleTabId == selected);
    return window;
}

void checkWidth(ImGuiWindow* window)
{
    REQUIRE(window->ScrollMax.x == 0);
    const auto available = window->InnerRect.GetWidth() - window->WindowPadding.x * 2;
    REQUIRE(window->ContentSize.x <= available + 1);
}

void checkFits(ImGuiWindow* window)
{
    checkWidth(window);
    REQUIRE(window->ScrollMax.y == 0);
}

void useManualResolution()
{
    testSettings.autoResolution = false;
    testSettings.renderScale    = 4.5f;
}

// Runs body once per supported DPI scale. Each run builds the real panel's draw
// lists without a window, GPU, game or desktop input.
template <typename Body>
void atEachScale(Body body)
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    for (const float scale : kScales)
    {
        CAPTURE(scale);
        testSettings       = {};
        availableImages    = kPackImageCount;
        screenshotRequests = 0;
        captureStatus.clear();

        ImGui::CreateContext();
        tmloader::applyImGuiStyle(scale);
        auto& io     = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.Fonts->AddFontDefault();
        unsigned char* pixels = nullptr;
        int            width  = 0;
        int            height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

        body(scale);

        // Laying the panel out must not request a capture.
        REQUIRE(screenshotRequests == 0);
        ImGui::DestroyContext();
    }
}

} // namespace

TEST_CASE("the panel style enables keyboard navigation and writes no files", "[settings_ui]")
{
    atEachScale([](float)
                {
                    const auto& io = ImGui::GetIO();
                    REQUIRE((io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) != 0);
                    REQUIRE((io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) == 0);
                    REQUIRE(io.IniFilename == nullptr);
                    REQUIRE(io.LogFilename == nullptr);
                });
}

TEST_CASE("every tab fits the panel without scrolling", "[settings_ui]")
{
    atEachScale([](float scale)
                {
                    const auto preferred = ImVec2{ kPanelWidth * scale, kPanelHeight * scale };
                    for (const char* const tab : kTabs)
                    {
                        CAPTURE(tab);
                        checkFits(selectTab(tab, preferred));
                    }

                    useManualResolution();
                    checkFits(selectTab("Display", preferred));
#ifdef TMLOADER_HD_TEXTURES
                    availableImages = 0;
                    checkFits(selectTab("Artwork & text", preferred));
#endif
                });
}

TEST_CASE("a long screenshot status wraps inside the panel", "[settings_ui]")
{
    atEachScale([](float scale)
                {
                    const auto preferred = ImVec2{ kPanelWidth * scale, kPanelHeight * scale };
                    for (const auto* const status : { &kSavedStatus, &kFailedStatus })
                    {
                        captureStatus      = *status;
                        const auto  before = statusReads;
                        auto* const window = selectTab("Screenshots", preferred);
                        REQUIRE(statusReads > before);
                        checkFits(window);
                    }
                });
}

TEST_CASE("a short window scrolls vertically and keeps its width", "[settings_ui]")
{
    atEachScale([](float scale)
                {
                    useManualResolution();
                    captureStatus = kFailedStatus;

                    // The native window clamps to the monitor work area. A short monitor
                    // should produce vertical scrolling, never clip or widen the controls.
                    for (const char* const tab : kTabs)
                    {
                        CAPTURE(tab);
                        auto* const window = selectTab(tab, { kPanelWidth * scale, kShortHeight * scale });
                        checkWidth(window);
                        REQUIRE(window->ScrollMax.y > 0);
                        REQUIRE(window->ScrollbarY);
                        ImGui::SetScrollY(window, window->ScrollMax.y);
                        settle();

                        REQUIRE(window->Scroll.y >= window->ScrollMax.y - 1);
                        REQUIRE(window->DC.CursorMaxPos.y <= window->InnerRect.Max.y + 1);
                    }
                });
}
