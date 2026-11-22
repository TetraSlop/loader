#include "app/log.h"
#include "app/runtime.h"
#include "login/login.h"
#include "ui/account.h"
#include <algorithm>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <wil/stl.h>
#include <wil/win32_helpers.h>

namespace
{

using tmloader::Config;

// A file shipped beside tmloader.exe.
auto besideExecutable(const char* name) -> std::filesystem::path
{
    return std::filesystem::path(wil::GetModuleFileNameW<std::wstring>(nullptr)).replace_filename(name);
}

auto check(const Config& config) -> int
{
    tmloader::validateRuntime(config);
    const auto list = besideExecutable("retail-files.txt");
    if (!std::filesystem::exists(list))
    {
        std::puts("No retail-files.txt beside tmloader.exe; checked the DLLs and required files only.");
    }
    else
    {
        const auto report = tmloader::checkRetailFiles(list, config.runtimeDir);
        for (const auto& name : report.changed)
        {
            std::printf("Warning: %s differs from the factory copy, as expected once the Viewer has saved settings.\n", name.c_str());
        }

        std::fflush(stdout);
        if (const auto failures = tmloader::describeRetailFailures(report); !failures.empty())
        {
            throw std::runtime_error(failures);
        }

        std::printf("Retail files verified: %zu.\n", report.checked);
    }

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

    const auto config = besideExecutable("tmloader.ini");
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

auto play(Config config) -> int
{
    if (config.localAccount)
    {
        config.server = tmloader::resolveIpv4(config.server);
        return tmloader::launch(config);
    }

    auto configFile = config.configFile;
    if (configFile.empty())
    {
        configFile = besideExecutable("tmloader.ini");
    }

    const auto game = tmloader::signIn(config, configFile);
    if (!game)
    {
        return 0;
    }

    return tmloader::launch(*game);
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
