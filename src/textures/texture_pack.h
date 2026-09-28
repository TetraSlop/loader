#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace tmloader
{

struct PngInfo
{
    unsigned width  = 0;
    unsigned height = 0;
    size_t   bytes  = 0; // Through IEND; archives pad PNGs with alignment bytes.
};

auto pngInfo(const void* data, size_t bytes) -> PngInfo;

enum class TextureCategory
{
    artwork,
    font
};

struct TextureAsset
{
    std::string           id;
    std::string           sourceHash;
    std::string           imageHash;
    std::filesystem::path file;
    unsigned              width    = 0;
    unsigned              height   = 0;
    unsigned              scale    = 0;
    TextureCategory       category = TextureCategory::artwork;
};

class TexturePack
{
public:
    void load(const std::filesystem::path& manifest);
    auto match(const void* png, size_t bytes) const -> const TextureAsset*;
    auto read(const TextureAsset& asset) const -> std::vector<uint8_t>;
    void validateFiles() const;

    auto size() const -> size_t
    {
        return assets_.size();
    }

private:
    std::filesystem::path                         root_;
    std::unordered_map<std::string, TextureAsset> assets_;
};

} // namespace tmloader
