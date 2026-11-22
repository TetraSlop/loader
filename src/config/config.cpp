#include <windows.h>

#include "config/config.h"
#include <CLI/CLI.hpp>
#include <algorithm>
#include <fstream>
#include <regex>
#include <stdexcept>

namespace tmloader
{
namespace
{

constexpr auto   kPolIdAlphabet     = "EFKAOYMJVNGTDSWBQLPCIRHZXU6328401795";
constexpr auto   kLocalPolId        = "EEEEEEEF";
constexpr auto   kLocalPassword     = "tm-local-test";
constexpr size_t kMaxPasswordLength = 15;
constexpr size_t kMaxHostNameLength = 253;

const auto kUsername = std::regex(R"([A-Za-z0-9_.-]{1,15})");
const auto kHostName = std::regex(R"(([A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*)");

void defineOptions(CLI::App& app, Config& config)
{
    app.set_help_flag();
    app.get_formatter()->column_width(40);
    app.set_config("--config", "", "Read key=value settings; command-line options override them");
    app.get_config_ptr()->check(CLI::ExistingFile.description(""))->type_name("FILE");
    app.allow_config_extras(false);
    app.add_option("--runtime-dir", config.runtimeDir, "Directory holding the game DLLs and data")->type_name("DIR");
    app.add_option("--server", config.server, "Game server address or host name")->type_name("HOST")->capture_default_str();
    app.add_option("--game-port", config.gamePort, "Game IRC port")->type_name("PORT")->capture_default_str()->check(CLI::Range(1, 65535).description(""));
    app.add_option("--profile-port", config.profilePort, "Profile port")->type_name("PORT")->capture_default_str()->check(CLI::Range(1, 65535).description(""));
    app.add_option("--username", config.username, "Account username; asked for when unset")->type_name("NAME");
    app.add_option("--pol-id", config.polId, "POL ID of a built-in test account, with --local-account")->type_name("ID");
    app.add_option("--password", config.password, "Account password, 1 to 15 characters; asked for when unset")->type_name("TEXT");
    app.add_flag("--local-account", config.localAccount, "Log in to a loopback server with a built-in test account, without LOGIN");
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
    if (!isValidServer(config.server))
    {
        throw std::runtime_error("--server must be an IPv4 address or a host name");
    }

    if (config.gamePort == config.profilePort)
    {
        throw std::runtime_error("Game and profile ports must differ");
    }

    if (config.profilePort == kClientGamePort)
    {
        throw std::runtime_error("Profile port cannot use the client's reserved Game IRC port 51241");
    }

    if (!config.username.empty() && !isValidUsername(config.username))
    {
        throw std::runtime_error("--username must be 1 to 15 letters, digits, '_', '.' or '-'");
    }

    if (!config.polId.empty() && !config.localAccount)
    {
        throw std::runtime_error("--pol-id is only for --local-account; the server assigns your POL ID, so set --username");
    }

    if (!config.polId.empty() && !isValidPolId(config.polId))
    {
        throw std::runtime_error("--pol-id must be an eight-character PlayOnline ID");
    }

    if (config.localAccount && (config.polId.empty() || config.password.empty()))
    {
        throw std::runtime_error("--local-account needs both --pol-id and --password, or neither");
    }

    if (!config.password.empty() && !isValidPassword(config.password))
    {
        throw std::runtime_error("--password must be 1 to 15 printable ASCII characters");
    }

    if (config.localAccount && !isLoopbackServer(config.server))
    {
        throw std::runtime_error("--local-account needs a loopback --server such as 127.0.0.1");
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
    if (configFile->count())
    {
        config.configFile = std::filesystem::absolute(configFile->as<std::string>());
    }

    if (config.localAccount && config.polId.empty() && config.password.empty())
    {
        config.polId    = kLocalPolId;
        config.password = kLocalPassword;
    }

    validate(config);
    return config;
}

auto isValidPolId(const std::string& polId) -> bool
{
    return polId.size() == 8 && polId.find_first_not_of(kPolIdAlphabet) == std::string::npos;
}

auto isValidUsername(const std::string& username) -> bool
{
    return std::regex_match(username, kUsername);
}

auto isValidPassword(const std::string& password) -> bool
{
    return !password.empty() && password.size() <= kMaxPasswordLength && std::ranges::all_of(password, [](char c) -> bool
                                                                                             {
                                                                                                 return c >= 0x20 && c <= 0x7e;
                                                                                             });
}

auto isValidServer(const std::string& server) -> bool
{
    // A numeric name is an address, never a host name.
    if (server.find_first_not_of("0123456789.") == std::string::npos)
    {
        return isIpv4Address(server);
    }

    return server.size() <= kMaxHostNameLength && std::regex_match(server, kHostName);
}

auto isLoopbackServer(const std::string& server) -> bool
{
    return server == "localhost" || (isIpv4Address(server) && server.starts_with("127."));
}

void saveConfigValues(const std::filesystem::path& file, const std::vector<std::pair<std::string, std::string>>& values)
{
    auto lines = std::vector<std::string>{};
    if (auto input = std::ifstream(file); input)
    {
        for (auto line = std::string{}; std::getline(input, line);)
        {
            if (line.ends_with('\r'))
            {
                line.pop_back();
            }

            lines.push_back(line);
        }
    }

    for (const auto& [key, value] : values)
    {
        // CLI11's own quoting, so its parser reads the value back unchanged.
        const auto entry   = key + "=" + CLI::detail::convert_arg_for_ini(value, '"', '\'', true);
        const auto matches = [&key](const std::string& line) -> bool
        {
            const auto start = line.find_first_not_of(" \t");
            if (start == std::string::npos || line.compare(start, key.size(), key) != 0)
            {
                return false;
            }

            const auto next = line.find_first_not_of(" \t", start + key.size());
            return next != std::string::npos && line[next] == '=';
        };
        if (const auto found = std::ranges::find_if(lines, matches); found != lines.end())
        {
            *found = entry;
        }
        else
        {
            lines.push_back(entry);
        }
    }

    auto partial = file;
    partial += ".partial";
    {
        auto output = std::ofstream(partial, std::ios::binary | std::ios::trunc);
        for (const auto& line : lines)
        {
            output << line << '\n';
        }

        if (!output)
        {
            throw std::runtime_error("Cannot write " + file.string());
        }
    }

    if (!MoveFileExW(partial.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        std::filesystem::remove(partial);
        throw std::runtime_error("Cannot replace " + file.string());
    }
}

auto usage() -> std::string
{
    auto config = Config{};
    auto app    = CLI::App{ "tmloader " TMLOADER_VERSION " - 32-bit Tetra Master host", "tmloader" };
    defineOptions(app, config);
    return app.help();
}

} // namespace tmloader
