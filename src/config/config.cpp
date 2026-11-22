#include "config/config.h"
#include <CLI/CLI.hpp>
#include <algorithm>
#include <stdexcept>

namespace tmloader
{
namespace
{

constexpr auto kPolIdAlphabet = "EFKAOYMJVNGTDSWBQLPCIRHZXU6328401795";

void defineOptions(CLI::App& app, Config& config)
{
    app.set_help_flag();
    app.get_formatter()->column_width(40);
    app.set_config("--config", "", "Read key=value settings; command-line options override them");
    app.get_config_ptr()->check(CLI::ExistingFile.description(""))->type_name("FILE");
    app.allow_config_extras(false);
    app.add_option("--runtime-dir", config.runtimeDir, "Directory holding the game DLLs and data")->type_name("DIR");
    app.add_option("--server", config.server, "Game server IPv4 address")->type_name("IPV4")->capture_default_str();
    app.add_option("--game-port", config.gamePort, "Game IRC port")->type_name("PORT")->capture_default_str()->check(CLI::Range(1, 65535).description(""));
    app.add_option("--profile-port", config.profilePort, "Profile port")->type_name("PORT")->capture_default_str()->check(CLI::Range(1, 65535).description(""));
    app.add_option("--pol-id", config.polId, "Account ID")->type_name("ID")->capture_default_str();
    app.add_option("--password", config.password, "Account password")->type_name("TEXT")->capture_default_str();
    app.add_flag("--diagnostics", config.diagnostics, "Extra logging, and screenshots at fixed startup frames");
    app.add_flag("--check", config.checkOnly, "Verify files, hashes and settings without launching")->configurable(false);
    app.add_flag("-h,--help", config.help, "Show this help")->configurable(false);
    app.add_flag("--version", config.version, "Show the version")->configurable(false);
    // Reject a setting given twice, e.g. duplicated in the config file.
    for (auto* const option : app.get_options())
    {
        if (option != app.get_config_ptr())
        {
            option->multi_option_policy(CLI::MultiOptionPolicy::Throw);
        }
    }
}

void validate(const Config& config)
{
    if (!isIpv4Address(config.server))
    {
        throw std::runtime_error("--server must be a numeric IPv4 address");
    }

    if (config.gamePort == config.profilePort)
    {
        throw std::runtime_error("Game and profile ports must differ");
    }

    if (config.profilePort == kClientGamePort)
    {
        throw std::runtime_error("Profile port cannot use the client's reserved Game IRC port 51241");
    }

    if (config.polId.size() != 8 || config.polId.find_first_not_of(kPolIdAlphabet) != std::string::npos)
    {
        throw std::runtime_error("--pol-id must be an eight-character PlayOnline ID");
    }

    if (config.password.empty() || config.password.size() > 15 || config.password.find_first_of("\r\n") != std::string::npos)
    {
        throw std::runtime_error("Password must contain 1..15 bytes without CR/LF");
    }

    if (!outputPathAllowed(config, config.log))
    {
        throw std::runtime_error("Log file is not a safe output path");
    }
}

} // namespace

auto parseConfig(const std::vector<std::string>& args) -> Config
{
    auto config = Config{};
    auto app    = CLI::App{};
    defineOptions(app, config);
    // CLI11 consumes arguments from the back.
    auto reversed = std::vector<std::string>(args.rbegin(), args.rend());
    try
    {
        app.parse(reversed);
    }
    catch (const CLI::ParseError& error)
    {
        throw std::runtime_error(error.what());
    }

    if (config.help || config.version)
    {
        return config;
    }

    if (config.runtimeDir.empty())
    {
        throw std::runtime_error("--runtime-dir is required (or runtime-dir in --config)");
    }

    // A relative runtime-dir from a config file is relative to that file.
    const auto* const configFile = app.get_config_ptr();
    if (config.runtimeDir.is_relative() && configFile->count() && std::ranges::none_of(args, [](const std::string& arg)
                                                                                       {
                                                                                           return arg.starts_with("--runtime-dir");
                                                                                       }))
    {
        config.runtimeDir = std::filesystem::absolute(configFile->as<std::string>()).parent_path() / config.runtimeDir;
    }

    config.runtimeDir = std::filesystem::absolute(config.runtimeDir).lexically_normal();
    config.game       = config.runtimeDir / "TM_native.dll";
    config.viewer     = config.runtimeDir / "polcore_native.dll";
    config.log        = config.runtimeDir / "logs/tmloader.log";
    validate(config);
    return config;
}

auto usage() -> std::string
{
    auto config = Config{};
    auto app    = CLI::App{ "tmloader " TMLOADER_VERSION " - 32-bit Tetra Master host", "tmloader" };
    defineOptions(app, config);
    return app.help();
}

} // namespace tmloader
