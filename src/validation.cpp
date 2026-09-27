#include "config.h"
#include <windows.h>
#include <bcrypt.h>
#include <wil/resource.h>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace tmloader {
std::string sha256File(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read file: " + path.string());
    wil::unique_bcrypt_algorithm algorithm;
    wil::unique_bcrypt_hash hash;
    if (BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot initialize SHA256");
    if (BCryptCreateHash(algorithm.get(), hash.put(), nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("Cannot create SHA256");
    char buffer[65536];
    while (file.read(buffer, sizeof(buffer)) || file.gcount())
        if (BCryptHashData(hash.get(), reinterpret_cast<PUCHAR>(buffer), static_cast<ULONG>(file.gcount()), 0) < 0)
            throw std::runtime_error("SHA256 update failed");
    if (!file.eof()) throw std::runtime_error("File read failed");
    BYTE digest[32];
    if (BCryptFinishHash(hash.get(), digest, sizeof(digest), 0) < 0)
        throw std::runtime_error("SHA256 finalization failed");
    std::string out;
    constexpr char hex[] = "0123456789ABCDEF";
    for (const auto byte : digest) { out += hex[byte >> 4]; out += hex[byte & 15]; }
    return out;
}

void validateRuntime(const Config& config) {
    namespace fs = std::filesystem;
    if (!fs::is_directory(config.runtimeDir)) throw std::runtime_error("Runtime directory does not exist");
    if (config.runtimeDir.string().size() >= MAX_PATH - 40) throw std::runtime_error("Runtime path is too long for legacy client paths");
    const std::pair<fs::path, const char*> binaries[] = {
        {config.game, "29C965A835FAD561A94860262F780BC087B7BCC89BE9E3E89DA8F08D57F52003"},
        {config.core, "73B1864BF522D3AA6EC28E9F4BB226FCEE956F7F4848A238C9C2D756DC703B06"},
    };
    for (const auto& entry : binaries)
        if (sha256File(entry.first) != entry.second)
            throw std::runtime_error("Unsupported DLL hash: " + entry.first.string());
    for (const auto& name : {"data/gW201.dat", "data/utf/env.dat", "pub/home00/option.bin"})
        if (!fs::is_regular_file(config.runtimeDir / name))
            throw std::runtime_error("Missing runtime asset: " + std::string(name));
}
}
