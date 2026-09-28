#include "app/runtime.h"
#include "imgui_internal.h"
#include "ui/settings_ui.cpp"
#include "ui/window.h"
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>

namespace
{

tmloader::WindowSettings testSettings;
std::string              captureStatus;
unsigned                 screenshotRequests, statusReads;
size_t                   availableImages = 503;

} // namespace

namespace tmloader
{

HWND gameWindow{};

void logf(const char*, ...)
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
    REQUIRE((ImGui::GetDrawData()->Valid && ImGui::GetDrawData()->TotalVtxCount > 0));
}

auto selectTab(const char* label, ImVec2 size) -> ImGuiWindow*
{
    auto& io       = ImGui::GetIO();
    io.DisplaySize = size;
    frame();
    auto* const window = ImGui::FindWindowByName("Wrapper settings");
    REQUIRE(window);
    auto* const tabs = ImGui::GetCurrentContext()->TabBars.GetByKey(window->GetID("settings-tabs"));
    REQUIRE((tabs && tabs->Tabs.Size == 3));
    ImGuiID selected = 0;
    for (auto& tab : tabs->Tabs)
        if (std::strcmp(ImGui::TabBarGetTabName(tabs, &tab), label) == 0)
            selected = tab.ID;
    REQUIRE(selected);
    tabs->NextSelectedTabId = selected;
    ImGui::SetScrollY(window, 0);
    // Tab transitions and scrollbar visibility use previous-frame measurements.
    for (int i = 0; i < 3; ++i)
        frame();
    REQUIRE(tabs->VisibleTabId == selected);
    return window;
}

void checkWidth(ImGuiWindow* window)
{
    REQUIRE(window->ScrollMax.x == 0);
    const float available = window->InnerRect.GetWidth() - window->WindowPadding.x * 2;
    if (window->ContentSize.x > available + 1)
    {
        printf("Content width %.1f exceeds available %.1f\n", window->ContentSize.x, available);
        REQUIRE(false);
    }
}

} // namespace

TEST_CASE("Settings layout fits supported DPI scales", "[settings_ui]")
{
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    // Build the real panel's draw lists without a window, GPU, game or desktop input.
    for (const float scale : { 1.0f, 1.5f, 2.0f })
    {
        ImGui::CreateContext();
        tmloader::configurePanel(scale);
        auto& io = ImGui::GetIO();
        REQUIRE((io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) != 0);
        REQUIRE((io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) == 0);
        REQUIRE((io.IniFilename == nullptr && io.LogFilename == nullptr));
        io.DeltaTime = 1.0f / 60.0f;
        io.Fonts->AddFontDefault();
        unsigned char* pixels;
        int            width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        const ImVec2 preferred{ 524 * scale, 730 * scale };
        captureStatus.clear();
        for (const char* const tab : { "Display", "Artwork & text", "Screenshots" })
        {
            auto* const window = selectTab(tab, preferred);
            checkWidth(window);
            REQUIRE(window->ScrollMax.y == 0);
        }
        testSettings.autoResolution = false;
        testSettings.renderScale    = 4.5f;
        auto* window                = selectTab("Display", preferred);
        checkWidth(window);
        REQUIRE(window->ScrollMax.y == 0);
        availableImages = 0;
        window          = selectTab("Artwork & text", preferred);
        checkWidth(window);
        REQUIRE(window->ScrollMax.y == 0);
        availableImages = 503;
        for (const auto& status : {
                 "Screenshot saved: C:/Users/Player/Documents/GitHub/tmloader/runtime/logs/screenshots/" +
                     std::string(180, 'A') + "-20260927-234959-999-player-EEEEEEEF-000001.png",
                 "Screenshot failed: Cannot write the PNG to C:/Users/Player/Documents/GitHub/tmloader/runtime/logs/screenshots/" +
                     std::string(220, 'B') + ".png. The destination folder is unavailable; please check its permissions and try again." })
        {
            captureStatus         = status;
            const unsigned before = statusReads;
            window                = selectTab("Screenshots", preferred);
            REQUIRE(statusReads > before);
            checkWidth(window);
            REQUIRE(window->ScrollMax.y == 0);
        }
        // The native window clamps to the monitor work area. A short monitor
        // should produce vertical scrolling, never clip or widen the controls.
        for (const char* const tab : { "Display", "Artwork & text", "Screenshots" })
        {
            window = selectTab(tab, { 524 * scale, 260 * scale });
            checkWidth(window);
            REQUIRE((window->ScrollMax.y > 0 && window->ScrollbarY));
            ImGui::SetScrollY(window, window->ScrollMax.y);
            for (int i = 0; i < 3; ++i)
                frame();
            REQUIRE(window->Scroll.y >= window->ScrollMax.y - 1);
            REQUIRE(window->DC.CursorMaxPos.y <= window->InnerRect.Max.y + 1);
        }
        REQUIRE(screenshotRequests == 0); // Layout must not request a capture.
        printf("PASS: all tabs and long screenshot status fit at %.0f%%; short panels scroll\n", scale * 100);
        ImGui::DestroyContext();
    }
}
