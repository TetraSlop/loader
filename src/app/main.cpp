#include "app/runtime.h"
#include "textures/texture_pack.h"
#include <algorithm>
#include <cstdio>
#include <exception>

namespace
{

auto check(const tmloader::Config& config) -> int
{
    tmloader::validateRuntime(config);
    if (std::filesystem::exists(config.texturePack))
    {
        auto pack = tmloader::TexturePack{};
        pack.load(config.texturePack);
        pack.validateFiles();
        std::printf("HD texture pack: %zu source images, dimensions and file hashes verified.\n", pack.size());
    }

    std::puts("Runtime and supported DLL hashes verified. No DLL loaded or game launched.");
    return 0;
}

// Without --config, a tmloader.ini next to the executable is used.
auto withDefaultConfig(std::vector<std::string> args) -> std::vector<std::string>
{
    if (std::ranges::contains(args, "--config"))
    {
        return args;
    }

    wchar_t executable[MAX_PATH]{};
    GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const auto config = std::filesystem::path(executable).replace_filename("tmloader.ini");
    if (std::filesystem::exists(config))
    {
        args.insert(args.begin(), { "--config", config.string() });
    }

    return args;
}

} // namespace

auto main(int argc, char** argv) -> int
{
    try
    {
        const auto config = tmloader::parseConfig(withDefaultConfig(std::vector<std::string>(argv + 1, argv + argc)));
        if (config.help)
        {
            std::fputs(tmloader::usage().c_str(), stdout);
            return 0;
        }

        if (config.version)
        {
            std::puts("tmloader " TMLOADER_VERSION);
            return 0;
        }

        if (config.checkOnly)
        {
            return check(config);
        }

        tmloader::validateRuntime(config);
        return tmloader::launch(config);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "tmloader: %s\n", e.what());
        return 2;
    }
}
