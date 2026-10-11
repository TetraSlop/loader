#include "app/runtime.h"
#include "graphics/render_scale.h"
#include "graphics/screenshot.h"
#include "imgui.h"
#include "ui/imgui_window.h"
#include "ui/window.h"
#include <algorithm>
#include <atomic>
#include <thread>
#ifdef TMLOADER_HD_TEXTURES
#include "textures/texture_replacements.h"
#endif

namespace tmloader
{
namespace
{

std::atomic<bool> running{ false };
std::atomic<bool> closeRequested{ false };
std::atomic<HWND> panelWindow{ nullptr };

void displaySettings(WindowSettings value)
{
    ImGui::Spacing();
    if (ImGui::Checkbox("Borderless fullscreen", &value.fullscreen))
    {
        postWindowAction(WindowAction::fullscreen, value.fullscreen);
    }

    if (ImGui::Checkbox("Keep original 4:3 proportions", &value.preserveAspect))
    {
        postWindowAction(WindowAction::aspect, value.preserveAspect);
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Window size");
    if (ImGui::Button("640 x 480"))
    {
        postWindowAction(WindowAction::size, MAKELPARAM(640, 480));
    }

    ImGui::SameLine();
    if (ImGui::Button("960 x 720"))
    {
        postWindowAction(WindowAction::size, MAKELPARAM(960, 720));
    }

    ImGui::SameLine();
    if (ImGui::Button("1280 x 960"))
    {
        postWindowAction(WindowAction::size, MAKELPARAM(1280, 960));
    }

    ImGui::TextWrapped("You can also drag the game window's edges or maximize it.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Rendering resolution");
    auto resolution = [&value]() -> int
    {
        if (value.autoResolution)
        {
            return 0;
        }

        return 1 + static_cast<int>(std::ranges::find(kRenderScales, value.renderScale) - kRenderScales.begin());
    }();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##resolution", &resolution, "Automatic - match display\0"
                                                  "1x - 640 x 480 (original)\0"
                                                  "720p - 960 x 720\0"
                                                  "2x - 1280 x 960\0"
                                                  "1080p - 1440 x 1080\0"
                                                  "1440p - 1920 x 1440\0"
                                                  "4x - 2560 x 1920\0"
                                                  "4K - 2880 x 2160\0"
                                                  "Supersampled - 3840 x 2880\0"))
    {
        if (resolution == 0)
        {
            postWindowAction(WindowAction::autoResolution);
        }
        else
        {
            postWindowAction(WindowAction::renderScale, resolution - 1);
        }
    }

    const auto active = activeRenderScale();
    ImGui::TextDisabled("Currently rendering at %.0f x %.0f", 640 * active, 480 * active);
    ImGui::TextWrapped("Applies after relaunch. Automatic chooses the resolution for your monitor, up to 4K height.");
}

#ifdef TMLOADER_HD_TEXTURES
void artworkSettings(WindowSettings value)
{
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Artwork");
    if (ImGui::Checkbox("HD artwork", &value.hdArtwork))
    {
        postWindowAction(WindowAction::hdArtwork, value.hdArtwork);
    }

    if (ImGui::Checkbox("HD fonts", &value.hdFonts))
    {
        postWindowAction(WindowAction::hdFonts, value.hdFonts);
    }

    if (const auto images = texturePackCount())
    {
        ImGui::TextDisabled("%zu images available; %u replacements loaded", images, textureReplacementCount());
    }
    else
    {
        ImGui::TextDisabled("No HD texture pack loaded");
    }

    ImGui::TextWrapped("Switches immediately. Images missing from the pack use their originals.");
    ImGui::TextWrapped("Font selection affects in-game text. Lettering painted into menus or card images follows the artwork setting.");
}

#endif

void screenshotSettings()
{
    ImGui::Spacing();
    ImGui::TextWrapped("Press F12 in the game to save a PNG at the active rendering resolution, without window borders or this panel.");
    ImGui::TextWrapped("Saved in runtime/logs/screenshots. Each capture gets its own filename.");
    if (ImGui::Button("Save screenshot and return"))
    {
        requestScreenshot();
        closeRequested = true;
    }

    const auto status = screenshotStatus();
    if (!status.empty())
    {
        ImGui::TextWrapped("%s", status.c_str());
    }
}

void settingsFooter()
{
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextUnformatted("Audio");
    ImGui::TextWrapped("Adjust music and sound effect levels in the game's Options menu.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextWrapped("F10: settings   F12: screenshot   Alt+Enter: fullscreen");
    ImGui::TextWrapped("Window preferences are saved for this local account. Online matches continue while this panel is open.");
    ImGui::Spacing();
    if (ImGui::Button("Return to game"))
    {
        closeRequested = true;
    }

    ImGui::SameLine();
    if (ImGui::Button("Quit game"))
    {
        postWindowAction(WindowAction::close);
        closeRequested = true;
    }
}

void settingsContents()
{
    const auto* const viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("TETRA MASTER");
    ImGui::TextDisabled("Game settings");
    ImGui::Separator();
    const auto value = currentWindowSettings();
    if (ImGui::BeginTabBar("settings-tabs"))
    {
        if (ImGui::BeginTabItem("Display"))
        {
            displaySettings(value);
            ImGui::EndTabItem();
        }

#ifdef TMLOADER_HD_TEXTURES
        if (ImGui::BeginTabItem("Artwork & text"))
        {
            artworkSettings(value);
            ImGui::EndTabItem();
        }
#endif

        if (ImGui::BeginTabItem("Screenshots"))
        {
            screenshotSettings();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    settingsFooter();
    ImGui::End();
}

auto panelFrame() -> bool
{
    settingsContents();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_F10, false))
    {
        closeRequested = true;
    }

    return !closeRequested && IsWindow(gameWindow);
}

// A separate window keeps the panel responsive while the game is inactive.
void panelMain()
{
    const auto spec = ImGuiWindowSpec{ L"Tetra Master settings", 540, 770, gameWindow };
    runImGuiWindow(spec, panelFrame, [](HWND hwnd, IDirect3DDevice9*)
                   {
                       panelWindow = hwnd;
                   });
    panelWindow = nullptr;
    running     = false;
}

} // namespace

auto settingsPanelOpen() -> bool
{
    return running;
}

void stopSettingsPanel()
{
    closeRequested = true;
    if (const auto hwnd = panelWindow.load())
    {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
}

void toggleSettingsPanel()
{
    bool expected = false;
    if (!running.compare_exchange_strong(expected, true))
    {
        stopSettingsPanel();
        return;
    }

    closeRequested = false;
    std::thread(panelMain).detach();
}

} // namespace tmloader
