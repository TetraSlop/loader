#ifdef NDEBUG
#undef NDEBUG
#endif
#include "runtime.h"
#include "window.h"
#include <cassert>
#include <cmath>
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
WindowSettings currentWindowSettings() { return {}; }
RECT gameContentRect() { return {240, 0, 1680, 1080}; }
}
static HRESULT WINAPI reset(void*, tmloader::Present8* pp) {
    // Emulate D3D8's presentation contract: explicit rectangles require COPY.
    int scale = tmloader::activeRenderScale();
    assert(pp->windowed && pp->width == 640U * scale && pp->height == 480U * scale);
    assert(pp->swap == 3 && pp->count == 1 && pp->multisample == 0);
    assert(pp->window == tmloader::gameWindow && pp->refresh == 0);
    return S_OK;
}
static HRESULT WINAPI present(void*, const RECT* source, const RECT* dest, HWND, const RGNDATA*) {
    assert(dest && dest->left == 240 && dest->top == 0 && dest->right == 1680 && dest->bottom == 1080);
    if (source) {
        int scale = tmloader::activeRenderScale();
        assert(source->left == 10 * scale && source->top == 20 * scale &&
               source->right == 630 * scale && source->bottom == 470 * scale);
    }
    return S_OK;
}
static DWORD selectedShader, bufferFormat;
static int shaderCreates, shaderDeletes, constantsCalls;
static HRESULT WINAPI createShader(void*, const DWORD* declaration, const DWORD* code, DWORD* shader, DWORD) {
    assert(declaration[0] == 0x20000000 && code[0] == 0xfffe0101);
    *shader = 0x123;
    ++shaderCreates;
    return S_OK;
}
static HRESULT WINAPI deleteShader(void*, DWORD shader) {
    assert(shader == 0x123); ++shaderDeletes; return S_OK;
}
static HRESULT WINAPI constants(void*, DWORD index, const void* data, DWORD count) {
    assert(index == 0 && count == 2);
    auto* values = static_cast<const float*>(data);
    assert(std::abs(values[0] - 2.0f / 640) < 0.000001f && values[3] == 1);
    ++constantsCalls;
    return S_OK;
}
static HRESULT WINAPI selectShader(void*, DWORD shader) { selectedShader = shader; return S_OK; }
static HRESULT WINAPI makeBuffer(void*, UINT size, DWORD usage, DWORD format, DWORD pool, void**) {
    assert(size == 128 && usage == 0x238 && pool == 0);
    bufferFormat = format;
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
    void* table[97]{};
    table[75] = reinterpret_cast<void*>(createShader);
    table[78] = reinterpret_cast<void*>(deleteShader);
    table[79] = reinterpret_cast<void*>(constants);
    void** device = table;
    originalVertexShader = selectShader;
    originalVertexBuffer = makeBuffer;
    for (int factor : {1, 2, 3, 4}) {
        renderingFactor = factor;
        assert(resetDevice(&device, &pp) == S_OK);
        assert(setVertexShader(&device, gameVertexFormat) == S_OK);
        assert(selectedShader == (factor == 1 ? gameVertexFormat : 0x123));
        assert(vertexBuffer(&device, 128, 0x238, gameVertexFormat, 0, nullptr) == S_OK);
        assert(bufferFormat == (factor == 1 ? gameVertexFormat : 0));
        assert(vertexBuffer(&device, 128, 0x238, 0x142, 0, nullptr) == S_OK && bufferFormat == 0x142);
        assert(setVertexShader(&device, 0x142) == S_OK && selectedShader == 0x142);
        RECT source{10, 20, 630, 470};
        assert(presentFrame(nullptr, &source, nullptr, nullptr, nullptr) == S_OK);
        assert(source.left == 10 && source.right == 630);
    }
    deleteScaleShader(&device, scaleShader);
    assert(shaderCreates == 3 && shaderDeletes == 3 && constantsCalls == 3);
}
