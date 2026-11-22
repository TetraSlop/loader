#include "app/log.h"
#include "app/runtime.h"
#include <algorithm>
#include <cstdio>
#include <exception>
#include <wil/stl.h>
#include <wil/win32_helpers.h>
#ifdef TMLOADER_HOSTED
#include "ui/launcher.h"
#endif

namespace
{

using tmloader::Config;

auto check(const Config& config) -> int
{
    tmloader::validateRuntime(config);
    std::puts("Runtime verified.");
    return 0;
}

// Without --config, a tmloader.ini next to the executable is used.
auto withDefaultConfig(std::vector<std::string> args) -> std::vector<std::string>
{
    if (std::ranges::contains(args, "--config"))
    {
        return args;
    }

    const auto executable = wil::GetModuleFileNameW<std::wstring>(nullptr);
    const auto config     = std::filesystem::path(executable).replace_filename("tmloader.ini");
    if (std::filesystem::exists(config))
    {
        args.insert(args.begin(), { "--config", config.string() });
    }

    return args;
}

// A double-clicked GUI process has no console, so errors also get a message box.
void report(const char* error)
{
    std::fprintf(stderr, "tmloader: %s\n", error);
    if (!GetStdHandle(STD_ERROR_HANDLE))
    {
        MessageBoxA(nullptr, error, "Tetra Master", MB_OK | MB_ICONERROR);
    }
}

auto play(const Config& config) -> int
{
#ifdef TMLOADER_HOSTED
    if (!config.hostedServer.empty())
    {
        const auto session = tmloader::runLauncher(config);
        if (!session)
        {
            return 0;
        }

        const auto exitCode = tmloader::launch(session->gameConfig(config));
        session->endPlay();
        return exitCode;
    }
#endif

    return tmloader::launch(config);
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
        // The log is opened first because a low-integrity token cannot create it.
        tmloader::openLog(config.log);
        const auto exitCode = play(config);
        // The legacy DLLs are not safe to unload, so skip their teardown.
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exitCode));
        return exitCode;
    }
    catch (const std::exception& e)
    {
        report(e.what());
        return 2;
    }
}
