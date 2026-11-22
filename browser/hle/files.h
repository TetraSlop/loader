#pragma once

// The user's runtime folder as the stand-ins see it. Paths are relative to the folder, '/'-separated, and match
// without regard to case, as on Windows. Node and the native tests read a folder on disk; the browser host backs the
// same interface with a picked folder or OPFS. Read-only for now: the boot writes nothing.

#include <cstdint>
#include <filesystem>
#include <optional>
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
};

class DiskFiles final : public HostFiles
{
public:
    explicit DiskFiles(std::filesystem::path root);

    auto read(std::string_view path) -> std::optional<std::vector<std::uint8_t>> override;
    auto list(std::string_view directory) -> std::optional<std::vector<FileEntry>> override;

private:
    // The on-disk path for a relative guest path, matching each component without case
    auto locate(std::string_view path) const -> std::optional<std::filesystem::path>;

    std::filesystem::path root_;
};

} // namespace tmweb::hle
