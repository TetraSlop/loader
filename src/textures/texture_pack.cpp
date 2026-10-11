#include "textures/texture_pack.h"
#include "config/config.h"
#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>
#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace tmloader
{
namespace
{

constexpr uint8_t kPngSignature[]{ 137, 80, 78, 71, 13, 10, 26, 10 };
constexpr auto    kFontAtlasPrefix = std::string_view{ "gW000/0090/font_" };

auto bigEndian(const uint8_t* bytes) -> uint32_t
{
    auto value = uint32_t{};
    std::memcpy(&value, bytes, sizeof(value));
    return std::byteswap(value);
}

auto readBounded(const std::filesystem::path& path, size_t limit) -> std::vector<uint8_t>
{
    auto       file   = std::ifstream(path, std::ios::binary | std::ios::ate);
    const auto length = file.tellg();
    if (!file || length <= 0 || length > static_cast<std::streamoff>(limit))
    {
        throw std::runtime_error("Missing or oversized texture file: " + path.string());
    }

    auto result = std::vector<uint8_t>(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(result.data()), length))
    {
        throw std::runtime_error("Cannot read texture file: " + path.string());
    }

    return result;
}

auto hashText(std::string text) -> std::string
{
    if (text.size() != 64 || !std::ranges::all_of(text, [](unsigned char c)
                                                  {
                                                      return std::isxdigit(c);
                                                  }))
    {
        throw std::runtime_error("Texture manifest requires SHA256 hashes");
    }

    std::ranges::transform(text, text.begin(), [](unsigned char c)
                           {
                               return static_cast<char>(std::toupper(c));
                           });
    return text;
}

auto dimension(const nlohmann::json& value) -> unsigned
{
    if (!value.is_number_integer())
    {
        throw std::runtime_error("Invalid texture dimension");
    }

    const auto n = value.get<int64_t>();
    if (n < 1 || n > 8192)
    {
        throw std::runtime_error("Texture dimension outside 1..8192");
    }

    return static_cast<unsigned>(n);
}

// Only the 42 bitmap font atlases go through the native text renderer; lettering
// painted into menus and cards is artwork.
auto categoryFor(std::string_view id) -> TextureCategory
{
    if (!id.starts_with(kFontAtlasPrefix) || id.size() != kFontAtlasPrefix.size() + 2)
    {
        return TextureCategory::artwork;
    }

    const auto digits = id.substr(kFontAtlasPrefix.size());
    auto       index  = 0U;
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), index);
    if (parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size() && index < 42)
    {
        return TextureCategory::font;
    }

    return TextureCategory::artwork;
}

} // namespace

auto pngInfo(const void* data, size_t bytes) -> PngInfo
{
    if (!data || bytes < 45 || bytes > 64 * 1024 * 1024)
    {
        return {};
    }

    const auto* const png = static_cast<const uint8_t*>(data);
    if (std::memcmp(png, kPngSignature, sizeof(kPngSignature)) || bigEndian(png + 8) != 13 || std::memcmp(png + 12, "IHDR", 4))
    {
        return {};
    }

    const auto width  = bigEndian(png + 16);
    const auto height = bigEndian(png + 20);
    if (!width || !height || width > 8192 || height > 8192)
    {
        return {};
    }

    // Walk chunks (length, type, data, CRC) to the end of IEND.
    for (size_t position = 8; position <= bytes - 12;)
    {
        const auto length = bigEndian(png + position);
        if (length > bytes - position - 12)
        {
            return {};
        }

        if (!std::memcmp(png + position + 4, "IEND", 4))
        {
            if (length)
            {
                return {};
            }

            return { width, height, position + 12 };
        }

        position += static_cast<size_t>(length) + 12;
    }

    return {};
}

auto TexturePack::size() const -> size_t
{
    return assets_.size();
}

void TexturePack::load(const std::filesystem::path& manifest)
{
    // Build a complete new index before replacing the previous one.
    const auto raw      = readBounded(manifest, 2 * 1024 * 1024);
    const auto document = nlohmann::json::parse(raw.begin(), raw.end());
    if (document.at("version") != 1 || !document.at("textures").is_array() || document.at("textures").size() > 2048)
    {
        throw std::runtime_error("Unsupported texture pack manifest");
    }

    const auto                                    root = std::filesystem::weakly_canonical(manifest).parent_path();
    std::unordered_map<std::string, TextureAsset> assets;
    for (const auto& record : document.at("textures"))
    {
        TextureAsset asset;
        asset.id            = record.at("id").get<std::string>();
        asset.category      = categoryFor(asset.id);
        asset.sourceHash    = hashText(record.at("source_sha256").get<std::string>());
        asset.imageHash     = hashText(record.at("sha256").get<std::string>());
        const auto file     = record.at("file").get<std::string>();
        const auto relative = std::filesystem::path(std::u8string(file.begin(), file.end()));
        if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
            relative.native().find(L':') != std::wstring::npos)
        {
            throw std::runtime_error("Texture paths must be relative to their pack");
        }

        asset.file = std::filesystem::weakly_canonical(root / relative);
        if (!pathIsWithin(asset.file, root))
        {
            throw std::runtime_error("Texture path escapes its pack");
        }

        const auto& logical  = record.at("logical_size");
        const auto& physical = record.at("physical_size");
        if (!logical.is_array() || logical.size() != 2 || !physical.is_array() || physical.size() != 2)
        {
            throw std::runtime_error("Texture dimensions require two elements");
        }

        asset.width               = dimension(logical[0]);
        asset.height              = dimension(logical[1]);
        const auto physicalWidth  = dimension(physical[0]);
        const auto physicalHeight = dimension(physical[1]);
        asset.scale               = physicalWidth / asset.width;
        if (asset.scale < 2 || asset.scale > 4 || physicalWidth != asset.width * asset.scale || physicalHeight != asset.height * asset.scale)
        {
            throw std::runtime_error("Texture replacement must be uniformly 2x, 3x or 4x");
        }

        const auto replacement = [](const TextureAsset& value)
        {
            return std::tie(value.imageHash, value.width, value.height, value.scale, value.category);
        };
        const auto previous = assets.find(asset.sourceHash);
        if (previous != assets.end() && replacement(previous->second) != replacement(asset))
        {
            throw std::runtime_error("Conflicting replacements for one original image");
        }

        assets.emplace(asset.sourceHash, std::move(asset));
    }

    root_   = root;
    assets_ = std::move(assets);
}

auto TexturePack::match(const void* png, size_t bytes) const -> const TextureAsset*
{
    if (assets_.empty())
    {
        return nullptr;
    }

    const auto info = pngInfo(png, bytes);
    if (!info.bytes)
    {
        return nullptr;
    }

    const auto found = assets_.find(sha256Hex(png, info.bytes));
    if (found == assets_.end() || found->second.width != info.width || found->second.height != info.height)
    {
        return nullptr;
    }

    return &found->second;
}

auto TexturePack::read(const TextureAsset& asset) const -> std::vector<uint8_t>
{
    // Recheck after indexing so a subsequently replaced junction cannot escape.
    if (!pathIsWithin(asset.file, root_))
    {
        throw std::runtime_error("Texture path escaped its pack");
    }

    auto       bytes = readBounded(asset.file, 64 * 1024 * 1024);
    const auto info  = pngInfo(bytes.data(), bytes.size());
    if (!info.bytes || info.width != asset.width * asset.scale || info.height != asset.height * asset.scale ||
        sha256Hex(bytes.data(), bytes.size()) != asset.imageHash)
    {
        throw std::runtime_error("Texture PNG differs from its manifest: " + asset.id);
    }

    return bytes;
}

void TexturePack::validateFiles() const
{
    for (const auto& asset : assets_ | std::views::values)
    {
        read(asset);
    }
}

} // namespace tmloader
