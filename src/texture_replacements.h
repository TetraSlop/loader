#pragma once
#include "texture_hooks.h"
#include <filesystem>

namespace tmloader {
void initializeTextureReplacements(BYTE* game, const std::filesystem::path& manifest);
// Separate initialization permits headless upload tests without loading the game.
void configureTextureReplacements(const std::filesystem::path& manifest, TextureUploadFn upload);
size_t texturePackCount();
unsigned textureReplacementCount();
HRESULT attachTextureReplacement(void* original, void* replacement);
HRESULT WINAPI uploadTextureWithReplacement(void* device, const void* source, UINT bytes,
    UINT width, UINT height, UINT levels, DWORD usage, DWORD format, DWORD pool,
    DWORD filter, DWORD mipFilter, DWORD colorKey, void* info, void* palette, void** output);

// Restores the native binding before returning to the game's cached renderer.
class TextureDrawBinding {
public:
    TextureDrawBinding(void* device, bool enabled);
    ~TextureDrawBinding();
    TextureDrawBinding(const TextureDrawBinding&) = delete;
    TextureDrawBinding& operator=(const TextureDrawBinding&) = delete;
private:
    void* device_ = nullptr;
    void* original_ = nullptr;
    void* replacement_ = nullptr;
    bool bound_ = false;
};
}
