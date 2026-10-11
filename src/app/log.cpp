#include "app/log.h"
#include <cstdio>
#include <stdexcept>

namespace tmloader
{
namespace
{

FILE* logFile = nullptr;

} // namespace

void openLog(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    logFile = _wfopen(path.c_str(), L"w");
    if (!logFile)
    {
        throw std::runtime_error("Cannot open log file");
    }
}

void writeLog(std::string_view line)
{
    if (!logFile)
    {
        logFile = stderr;
    }

    std::fwrite(line.data(), 1, line.size(), logFile);
    std::fputc('\n', logFile);
    std::fflush(logFile);
}

} // namespace tmloader
