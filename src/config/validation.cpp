#include <windows.h>

#include "config/config.h"
#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cctype>
#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>
#include <wil/resource.h>

namespace tmloader
{
namespace
{

constexpr auto kGameHash   = "29C965A835FAD561A94860262F780BC087B7BCC89BE9E3E89DA8F08D57F52003";
constexpr auto kViewerHash = "73B1864BF522D3AA6EC28E9F4BB226FCEE956F7F4848A238C9C2D756DC703B06";
// Rewritten by the Viewer when settings are saved, so a mismatch is only reported.
constexpr auto   kViewerSettings = "pub/home00/option.bin";
constexpr size_t kDigestDigits   = 64;

// A sha256sum line: 64 hex digits, a space, a space or '*' (binary mode), then the path.
auto parseRetailLine(std::string_view line, size_t number) -> std::pair<std::string, std::string>
{
    const auto malformed = [number](const char* reason) -> std::runtime_error
    {
        return std::runtime_error(std::format("retail-files.txt line {}: {}", number, reason));
    };

    if (line.size() <= kDigestDigits + 2 || line[kDigestDigits] != ' ' || (line[kDigestDigits + 1] != ' ' && line[kDigestDigits + 1] != '*'))
    {
        throw malformed("expected \"<sha256>  <path>\"");
    }

    auto digest = std::string(line.substr(0, kDigestDigits));
    if (!std::ranges::all_of(digest, [](unsigned char c) -> bool
                             {
                                 return std::isxdigit(c) != 0;
                             }))
    {
        throw malformed("the hash is not 64 hex digits");
    }

    std::ranges::transform(digest, digest.begin(), [](unsigned char c) -> char
                           {
                               return static_cast<char>(std::toupper(c));
                           });
    const auto name = std::string(line.substr(kDigestDigits + 2));
    const auto path = std::filesystem::path(name);
    if (path.has_root_name() || path.has_root_directory() || std::ranges::contains(path, std::filesystem::path("..")))
    {
        throw malformed("the path must stay inside the runtime folder");
    }

    return { name, digest };
}

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

auto checkRetailFiles(const std::filesystem::path& list, const std::filesystem::path& runtimeDir) -> RetailFileReport
{
    auto input = std::ifstream(list);
    if (!input)
    {
        throw std::runtime_error("Cannot read file: " + list.string());
    }

    auto report = RetailFileReport{};
    auto line   = std::string{};
    for (size_t number = 1; std::getline(input, line); ++number)
    {
        if (line.ends_with('\r'))
        {
            line.pop_back();
        }

        if (line.empty() || line.starts_with('#'))
        {
            continue;
        }

        const auto [name, digest] = parseRetailLine(line, number);
        const auto path           = runtimeDir / name;
        ++report.checked;
        if (!std::filesystem::is_regular_file(path))
        {
            report.missing.push_back(name);
        }
        else if (sha256File(path) != digest)
        {
            if (name == kViewerSettings)
            {
                report.changed.push_back(name);
            }
            else
            {
                report.mismatched.push_back(name);
            }
        }
    }

    return report;
}

auto describeRetailFailures(const RetailFileReport& report, size_t maxLines) -> std::string
{
    auto lines = std::vector<std::string>{};
    for (const auto& name : report.mismatched)
    {
        lines.push_back("wrong hash: " + name);
    }

    for (const auto& name : report.missing)
    {
        lines.push_back("missing: " + name);
    }

    if (lines.empty())
    {
        return {};
    }

    auto text = std::format("{} of {} retail files are missing or do not match retail-files.txt:", lines.size(), report.checked);
    for (size_t i = 0; i < lines.size() && i < maxLines; ++i)
    {
        text += "\n  " + lines[i];
    }

    if (lines.size() > maxLines)
    {
        text += std::format("\n  ... and {} more", lines.size() - maxLines);
    }

    return text;
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
