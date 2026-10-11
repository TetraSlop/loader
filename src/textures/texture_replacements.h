#pragma once
#include <windows.h>

#include "textures/texture_pack.h"
#include <filesystem>
#include <unknwn.h>
#include <wrl/client.h>

// HD texture packs. Optional: built when TMLOADER_HD_TEXTURES is on.
namespace tmloader
{

// The game's statically linked D3DX8 texture loader.
using TextureUploadFn = HRESULT(WINAPI*)(void* device, const void* source, UINT bytes, UINT width, UINT height, UINT levels, DWORD usage, DWORD format, DWORD pool, DWORD filter, DWORD mipFilter, DWORD colorKey, void* sourceInfo, void* palette, void** output);

// Loads the pack and routes the game's texture uploads through it. A missing or
// broken pack is logged and the game keeps its original artwork.
void initializeTextureReplacements(HMODULE game, const std::filesystem::path& manifest);
// Hooks a device's draw calls once a pack is loaded. The vtable must be writable.
void hookTextureDraws(void** deviceVtable);
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
    void*                            device_ = nullptr;
    Microsoft::WRL::ComPtr<IUnknown> original_;
    Microsoft::WRL::ComPtr<IUnknown> attachment_;
    bool                             bound_ = false;
};

} // namespace tmloader
