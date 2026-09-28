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
    unsigned width = 0, height = 0;
    size_t   bytes = 0; // Through IEND, excluding archive alignment bytes.
};

PngInfo     pngInfo(const void* data, size_t bytes);
std::string sha256Bytes(const void* data, size_t bytes);

enum class TextureCategory
{
    artwork,
    font
};

struct TextureAsset
{
    std::string           id, sourceHash, imageHash;
    std::filesystem::path file;
    unsigned              width = 0, height = 0, scale = 0;
    TextureCategory       category = TextureCategory::artwork;
};

class TexturePack
{
public:
    void                 load(const std::filesystem::path& manifest);
    const TextureAsset*  match(const void* png, size_t bytes) const;
    std::vector<uint8_t> read(const TextureAsset& asset) const;
    void                 validateFiles() const;

    size_t size() const
    {
        return assets_.size();
    }

private:
    std::filesystem::path                         root_;
    std::unordered_map<std::string, TextureAsset> assets_;
};

} // namespace tmloader
