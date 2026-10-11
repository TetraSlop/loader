#include <windows.h>

#include "config/config.h"
#include <array>
#include <bcrypt.h>
#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>
#include <wil/resource.h>

namespace tmloader
{
namespace
{

constexpr auto kGameHash   = "29C965A835FAD561A94860262F780BC087B7BCC89BE9E3E89DA8F08D57F52003";
constexpr auto kViewerHash = "73B1864BF522D3AA6EC28E9F4BB226FCEE956F7F4848A238C9C2D756DC703B06";

} // namespace

auto sha256(const void* data, size_t bytes) -> std::array<std::uint8_t, 32>
{
    auto digest = std::array<std::uint8_t, 32>{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(bytes), digest.data(), static_cast<ULONG>(digest.size())) < 0)
    {
        throw std::runtime_error("SHA256 failed");
    }

    return digest;
}

auto sha256Hex(const void* data, size_t bytes) -> std::string
{
    auto text = std::string{};
    for (const auto byte : sha256(data, bytes))
    {
        text += std::format("{:02X}", byte);
    }

    return text;
}

auto sha256File(const std::filesystem::path& path) -> std::string
{
    auto file = std::ifstream(path, std::ios::binary);
    if (!file)
    {
        throw std::runtime_error("Cannot read file: " + path.string());
    }

    const auto bytes = std::vector<char>(std::istreambuf_iterator<char>(file), {});
    return sha256Hex(bytes.data(), bytes.size());
}

void validateRuntime(const Config& config)
{
    namespace fs = std::filesystem;
    if (!fs::is_directory(config.runtimeDir))
    {
        throw std::runtime_error("Runtime directory does not exist");
    }

    if (config.runtimeDir.string().size() >= MAX_PATH - 40)
    {
        throw std::runtime_error("Runtime path is too long for legacy client paths");
    }

    const std::pair<fs::path, const char*> binaries[] = {
        { config.game, kGameHash },
        { config.viewer, kViewerHash },
    };
    for (const auto& entry : binaries)
    {
        if (sha256File(entry.first) != entry.second)
        {
            throw std::runtime_error("Unsupported DLL hash: " + entry.first.string());
        }
    }

    for (const auto& name : { "data/gW201.dat", "data/utf/env.dat", "pub/home00/option.bin" })
    {
        if (!fs::is_regular_file(config.runtimeDir / name))
        {
            throw std::runtime_error("Missing runtime asset: " + std::string(name));
        }
    }
}

} // namespace tmloader
