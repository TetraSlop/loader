#include "texture_pack.h"
#include "config.h"
#include <windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace tmloader {
namespace {
unsigned bigEndian(const uint8_t* p) {
    return (unsigned(p[0]) << 24) | (unsigned(p[1]) << 16) | (unsigned(p[2]) << 8) | p[3];
}
std::vector<uint8_t> readBounded(const std::filesystem::path& path, size_t limit) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto length = file.tellg();
    if (!file || length <= 0 || length > static_cast<std::streamoff>(limit))
        throw std::runtime_error("Missing or oversized texture file: " + path.string());
    std::vector<uint8_t> result(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(result.data()), length))
        throw std::runtime_error("Cannot read texture file: " + path.string());
    return result;
}
std::string hashText(std::string text) {
    if (text.size() != 64 || text.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
        throw std::runtime_error("Texture manifest requires SHA256 hashes");
    for (auto& c : text) if (c >= 'a' && c <= 'f') c -= 'a' - 'A';
    return text;
}
unsigned dimension(const nlohmann::json& value) {
    if (!value.is_number_integer()) throw std::runtime_error("Invalid texture dimension");
    const auto n = value.get<int64_t>();
    if (n < 1 || n > 8192) throw std::runtime_error("Texture dimension outside 1..8192");
    return static_cast<unsigned>(n);
}
}

PngInfo pngInfo(const void* data, size_t bytes) {
    static constexpr uint8_t signature[]{137, 80, 78, 71, 13, 10, 26, 10};
    if (!data || bytes < 45 || bytes > 64 * 1024 * 1024) return {};
    const auto* p = static_cast<const uint8_t*>(data);
    if (std::memcmp(p, signature, 8) || bigEndian(p + 8) != 13 || std::memcmp(p + 12, "IHDR", 4)) return {};
    const unsigned width = bigEndian(p + 16), height = bigEndian(p + 20);
    if (!width || !height || width > 8192 || height > 8192) return {};
    for (size_t pos = 8; pos <= bytes - 12;) {
        const auto length = bigEndian(p + pos);
        if (length > bytes - pos - 12) return {};
        if (!std::memcmp(p + pos + 4, "IEND", 4))
            return length == 0 ? PngInfo{width, height, pos + 12} : PngInfo{};
        pos += size_t(length) + 12;
    }
    return {};
}

std::string sha256Bytes(const void* data, size_t bytes) {
    if (bytes > ULONG_MAX) throw std::runtime_error("SHA256 input too large");
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize texture SHA256");
    std::array<uint8_t, 32> digest{};
    const auto status = BCryptHash(algorithm, nullptr, 0,
        const_cast<PUCHAR>(static_cast<const uint8_t*>(data)), static_cast<ULONG>(bytes),
        digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) throw std::runtime_error("Cannot hash texture");
    std::string result;
    for (const auto byte : digest) {
        result += "0123456789ABCDEF"[byte >> 4];
        result += "0123456789ABCDEF"[byte & 15];
    }
    return result;
}

void TexturePack::load(const std::filesystem::path& manifest) {
    // Build a complete new index before replacing the previous one.
    const auto raw = readBounded(manifest, 2 * 1024 * 1024);
    const auto document = nlohmann::json::parse(raw.begin(), raw.end());
    if (document.at("version") != 1 || !document.at("textures").is_array() || document.at("textures").size() > 2048)
        throw std::runtime_error("Unsupported texture pack manifest");
    const auto root = std::filesystem::weakly_canonical(manifest).parent_path();
    std::unordered_map<std::string, TextureAsset> assets;
    for (const auto& record : document.at("textures")) {
        TextureAsset asset;
        asset.id = record.at("id").get<std::string>();
        asset.sourceHash = hashText(record.at("source_sha256").get<std::string>());
        asset.imageHash = hashText(record.at("sha256").get<std::string>());
        const auto relative = std::filesystem::u8path(record.at("file").get<std::string>());
        if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
            relative.native().find(L':') != std::wstring::npos)
            throw std::runtime_error("Texture paths must be relative to their pack");
        asset.file = std::filesystem::weakly_canonical(root / relative);
        if (!pathIsWithin(asset.file, root)) throw std::runtime_error("Texture path escapes its pack");
        const auto& logical = record.at("logical_size");
        const auto& physical = record.at("physical_size");
        if (!logical.is_array() || logical.size() != 2 || !physical.is_array() || physical.size() != 2)
            throw std::runtime_error("Texture dimensions require two elements");
        asset.width = dimension(logical[0]); asset.height = dimension(logical[1]);
        const auto w = dimension(physical[0]), h = dimension(physical[1]);
        asset.scale = w / asset.width;
        if (asset.scale < 2 || asset.scale > 4 || w != asset.width * asset.scale || h != asset.height * asset.scale)
            throw std::runtime_error("Texture replacement must be uniformly 2x, 3x or 4x");
        const auto previous = assets.find(asset.sourceHash);
        if (previous != assets.end() && (previous->second.imageHash != asset.imageHash ||
            previous->second.width != asset.width || previous->second.height != asset.height || previous->second.scale != asset.scale))
            throw std::runtime_error("Conflicting replacements for one original image");
        assets.emplace(asset.sourceHash, std::move(asset));
    }
    root_ = root;
    assets_ = std::move(assets);
}

const TextureAsset* TexturePack::match(const void* png, size_t bytes) const {
    if (assets_.empty()) return nullptr;
    const auto info = pngInfo(png, bytes);
    if (!info.bytes) return nullptr;
    const auto found = assets_.find(sha256Bytes(png, info.bytes));
    if (found == assets_.end() || found->second.width != info.width || found->second.height != info.height) return nullptr;
    return &found->second;
}

std::vector<uint8_t> TexturePack::read(const TextureAsset& asset) const {
    // Recheck after indexing so a subsequently replaced junction cannot escape.
    if (!pathIsWithin(asset.file, root_)) throw std::runtime_error("Texture path escaped its pack");
    auto bytes = readBounded(asset.file, 64 * 1024 * 1024);
    const auto info = pngInfo(bytes.data(), bytes.size());
    if (!info.bytes || info.width != asset.width * asset.scale || info.height != asset.height * asset.scale ||
        sha256Bytes(bytes.data(), bytes.size()) != asset.imageHash)
        throw std::runtime_error("Texture PNG differs from its manifest: " + asset.id);
    return bytes;
}

void TexturePack::validateFiles() const {
    for (const auto& entry : assets_) read(entry.second);
}
}
