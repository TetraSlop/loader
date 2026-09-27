#ifdef NDEBUG
#undef NDEBUG
#endif
#include "runtime.h"
#include "window.h"
#include "imgui_internal.h"
#include <cassert>
#include "../src/settings_ui.cpp"
namespace tmloader {
HWND gameWindow{};
void logf(const char*, ...) {}
WindowSettings currentWindowSettings() { return {}; }
void postWindowAction(WindowAction, LPARAM) {}
}
int main() {
    // Build the real panel's draw lists without a window, GPU, game or desktop input.
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(524 * scale, 480 * scale);
        io.DeltaTime = 1.0f / 60.0f;
        io.Fonts->AddFontDefault();
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        auto& style = ImGui::GetStyle();
        style.WindowPadding = ImVec2(18, 16);
        style.ItemSpacing = ImVec2(10, 10);
        style.ScaleAllSizes(scale);
        style.FontScaleDpi = scale;
        // Two frames settle auto-measured content bounds.
        for (int frame = 0; frame < 2; ++frame) {
            ImGui::NewFrame();
            tmloader::settingsContents();
            ImGui::Render();
        }
        assert(ImGui::GetDrawData()->Valid && ImGui::GetDrawData()->TotalVtxCount > 0);
        auto* window = ImGui::FindWindowByName("Wrapper settings");
        assert(window && window->ScrollMax.x == 0 && window->ScrollMax.y == 0);
        ImGui::DestroyContext();
    }
}
