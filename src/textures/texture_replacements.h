#pragma once
#include "textures/texture_hooks.h"
#include "textures/texture_pack.h"
#include <filesystem>

namespace tmloader
{

void initializeTextureReplacements(BYTE* game, const std::filesystem::path& manifest);
// Loads a pack without hooking the game, for tests.
void        configureTextureReplacements(const std::filesystem::path& manifest, TextureUploadFn upload);
auto        texturePackCount() -> size_t;
auto        textureReplacementCount() -> unsigned;
auto        attachTextureReplacement(void* original, void* replacement, TextureCategory category = TextureCategory::artwork) -> HRESULT;
auto WINAPI uploadTextureWithReplacement(void* device, const void* source, UINT bytes, UINT width, UINT height, UINT levels, DWORD usage, DWORD format, DWORD pool, DWORD filter, DWORD mipFilter, DWORD colorKey, void* info, void* palette, void** output) -> HRESULT;

// Binds a texture's replacement for one draw call and restores the original afterwards.
class TextureDrawBinding
{
public:
    TextureDrawBinding(void* device, bool artworkEnabled, bool fontsEnabled);
    ~TextureDrawBinding();
    TextureDrawBinding(const TextureDrawBinding&)                    = delete;
    auto operator=(const TextureDrawBinding&) -> TextureDrawBinding& = delete;
    TextureDrawBinding(TextureDrawBinding&&)                         = delete;
    auto operator=(TextureDrawBinding&&) -> TextureDrawBinding&      = delete;

private:
    void* device_     = nullptr;
    void* original_   = nullptr;
    void* attachment_ = nullptr;
    bool  bound_      = false;
};

} // namespace tmloader
