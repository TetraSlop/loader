#include "app/log.h"
#include "app/runtime.h"
#include "textures/texture_pack.h"
#include "ui/launcher.h"
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

// Double-clicked GUI processes have no console, so errors also get a message box.
void report(const char* error)
{
    std::fprintf(stderr, "tmloader: %s\n", error);
    if (!GetStdHandle(STD_ERROR_HANDLE))
    {
        MessageBoxA(nullptr, error, "Tetra Master", MB_OK | MB_ICONERROR);
    }
}

auto play(const tmloader::Config& config) -> int
{
    auto session = std::unique_ptr<tmloader::hosted::HostedSession>{};
    if (!config.hostedServer.empty())
    {
        session = tmloader::runLauncher(config);
        if (!session)
        {
            return 0;
        }
    }

    const auto exitCode = [&]
    {
        if (session)
        {
            return tmloader::launch(session->gameConfig(config));
        }

        return tmloader::launch(config);
    }();
    if (session)
    {
        session->endPlay();
    }

    // Legacy DLL teardown is unverified; exit without unloading them.
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exitCode));
    return exitCode;
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
        tmloader::options = config;
        // Open the log before lowering integrity; the low-integrity token cannot create it.
        tmloader::openLog(config.log);
        return play(config);
    }
    catch (const std::exception& e)
    {
        report(e.what());
        return 2;
    }
}
