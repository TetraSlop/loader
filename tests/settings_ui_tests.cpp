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
float activeRenderScale() { return 4.5f; }
size_t texturePackCount() { return 250; }
unsigned textureReplacementCount() { return 5; }
void postWindowAction(WindowAction, LPARAM) {}
}
int main() {
    // Build the real panel's draw lists without a window, GPU, game or desktop input.
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        ImGui::CreateContext();
        tmloader::configurePanel(scale);
        auto& io = ImGui::GetIO();
        assert((io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) != 0);
        assert((io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) == 0);
        assert(io.IniFilename == nullptr && io.LogFilename == nullptr);
        io.DisplaySize = ImVec2(524 * scale, 730 * scale);
        io.DeltaTime = 1.0f / 60.0f;
        io.Fonts->AddFontDefault();
        unsigned char* pixels;
        int width, height;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
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
