#include "hle/files.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <ranges>

namespace tmweb::hle
{
namespace
{

auto sameText(const std::string_view a, const std::string_view b) -> bool
{
    return std::ranges::equal(a, b, [](const char x, const char y)
                              {
                                  return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
                              });
}

} // namespace

DiskFiles::DiskFiles(std::filesystem::path root)
: root_(std::move(root))
{
}

auto DiskFiles::locate(const std::string_view path) const -> std::optional<std::filesystem::path>
{
    auto here  = root_;
    auto error = std::error_code{};
    for (const auto part : std::views::split(path, '/'))
    {
        const auto name = std::string_view{ part.begin(), part.end() };
        if (name.empty() || name == ".")
        {
            continue;
        }
        if (name == "..")
        {
            return std::nullopt; // the guest path was already normalised; this would leave the folder
        }
        if (std::filesystem::exists(here / name, error))
        {
            here /= name;
            continue;
        }
        auto found = false;
        for (const auto& entry : std::filesystem::directory_iterator(here, error))
        {
            if (sameText(entry.path().filename().string(), name))
            {
                here  = entry.path();
                found = true;
                break;
            }
        }
        if (!found)
        {
            return std::nullopt;
        }
    }
    return here;
}

auto DiskFiles::read(const std::string_view path) -> std::optional<std::vector<std::uint8_t>>
{
    const auto full  = locate(path);
    auto       error = std::error_code{};
    if (!full || !std::filesystem::is_regular_file(*full, error))
    {
        return std::nullopt;
    }
    auto file = std::ifstream(*full, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), {});
}

auto DiskFiles::list(const std::string_view directory) -> std::optional<std::vector<FileEntry>>
{
    const auto full  = locate(directory);
    auto       error = std::error_code{};
    if (!full || !std::filesystem::is_directory(*full, error))
    {
        return std::nullopt;
    }
    auto out = std::vector<FileEntry>{};
    for (const auto& entry : std::filesystem::directory_iterator(*full, error))
    {
        const auto isDirectory = entry.is_directory(error);
        out.push_back(FileEntry{ entry.path().filename().string(), isDirectory ? 0 : entry.file_size(error), isDirectory });
    }
    std::ranges::sort(out, {}, &FileEntry::name);
    return out;
}

// Existing folders and the file keep their case; new parts are made as the guest spells them
auto DiskFiles::write(const std::string_view path, const std::span<const std::uint8_t> bytes) -> bool
{
    auto full  = root_;
    auto error = std::error_code{};
    for (const auto part : std::views::split(path, '/'))
    {
        const auto name = std::string_view{ part.begin(), part.end() };
        if (name.empty() || name == "." || name == "..")
        {
            continue;
        }
        const auto relative = std::filesystem::relative(full / name, root_, error).generic_string();
        const auto existing = locate(relative);
        full                = existing ? *existing : full / name;
    }
    std::filesystem::create_directories(full.parent_path(), error);
    auto file = std::ofstream(full, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file)
    {
        return false;
    }
    if (written)
    {
        written(path);
    }
    return true;
}

} // namespace tmweb::hle
