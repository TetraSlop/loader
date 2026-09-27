#ifdef NDEBUG
#undef NDEBUG
#endif
#include "runtime.h"
#include "window.h"
#include <cassert>
static BOOL fakeIconic(HWND) { return FALSE; }
#define IsIconic fakeIconic
#include "../src/graphics.cpp"
#undef IsIconic
namespace tmloader {
Config options;
BYTE* gameBase{};
HWND gameWindow = reinterpret_cast<HWND>(1);
DWORD frames = 10;
void logf(const char*, ...) {}
void initializeGameWindow(HWND) {}
RECT gameContentRect() { return {240, 0, 1680, 1080}; }
}
static HRESULT WINAPI reset(void*, tmloader::Present8* pp) {
    // Emulate D3D8's presentation contract: explicit rectangles require COPY.
    assert(pp->windowed && pp->width == 640 && pp->height == 480);
    assert(pp->swap == 3 && pp->count == 1 && pp->multisample == 0);
    assert(pp->window == tmloader::gameWindow && pp->refresh == 0);
    return S_OK;
}
static HRESULT WINAPI present(void*, const RECT*, const RECT* dest, HWND, const RGNDATA*) {
    assert(dest && dest->left == 240 && dest->top == 0 && dest->right == 1680 && dest->bottom == 1080);
    return S_OK;
}
int main() {
    using namespace tmloader;
    Present8 pp{};
    pp.width = 1920; pp.height = 1080; pp.swap = 1; pp.refresh = 60;
    originalReset = reset;
    originalPresent = present;
    assert(resetDevice(nullptr, &pp) == S_OK);
    assert(presentFrame(nullptr, nullptr, nullptr, nullptr, nullptr) == S_OK);
}
