#pragma once

// The user's runtime folder as the stand-ins see it. Paths are relative to the folder, '/'-separated, and match
// without regard to case, as on Windows. Node and the native tests read a folder on disk; the page's folder is in
// MEMFS, and the host copies each written file to OPFS. Writes are whole files: the stand-ins buffer an open file
// and write it when the guest flushes or closes it.

#include <functional>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tmweb::hle
{

struct FileEntry
{
    std::string   name;
    std::uint64_t size      = 0;
    bool          directory = false;
};

class HostFiles
{
public:
    HostFiles()                                    = default;
    HostFiles(const HostFiles&)                    = delete;
    HostFiles(HostFiles&&)                         = delete;
    auto operator=(const HostFiles&) -> HostFiles& = delete;
    auto operator=(HostFiles&&) -> HostFiles&      = delete;
    virtual ~HostFiles()                           = default;

    // The whole file, or nothing when it does not exist or is a directory
    virtual auto read(std::string_view path) -> std::optional<std::vector<std::uint8_t>> = 0;
    // A directory's entries sorted by name, or nothing when it does not exist; "" is the root
    virtual auto list(std::string_view directory) -> std::optional<std::vector<FileEntry>> = 0;
    // Replace or create the file, and the folders above it; false when that failed
    virtual auto write(std::string_view path, std::span<const std::uint8_t> bytes) -> bool = 0;
};

class DiskFiles final : public HostFiles
{
public:
    explicit DiskFiles(std::filesystem::path root);

    auto read(std::string_view path) -> std::optional<std::vector<std::uint8_t>> override;
    auto list(std::string_view directory) -> std::optional<std::vector<FileEntry>> override;
    auto write(std::string_view path, std::span<const std::uint8_t> bytes) -> bool override;

    // Called with each path written, for a host that keeps the files elsewhere too (the page's OPFS)
    std::function<void(std::string_view)> written;

private:
    // The on-disk path for a relative guest path, matching each component without case
    auto locate(std::string_view path) const -> std::optional<std::filesystem::path>;

    std::filesystem::path root_;
};

} // namespace tmweb::hle
