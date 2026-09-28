#include "config.h"
#include <fstream>
#include <set>
#include <stdexcept>

namespace tmloader
{
namespace
{

constexpr auto polIdAlphabet = "EFKAOYMJVNGTDSWBQLPCIRHZXU6328401795";

auto trim(const std::string& s) -> std::string
{
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
    {
        return {};
    }
    return s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
}

auto port(const std::string& value) -> unsigned short
{
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
    {
        throw std::runtime_error("Port must contain decimal digits");
    }
    const auto n = std::stoul(value);
    if (!n || n > 65535)
    {
        throw std::runtime_error("Port must be in 1..65535");
    }
    return static_cast<unsigned short>(n);
}

void assign(Config& c, const std::string& name, const std::string& value)
{
    if (name == "runtime-dir")
    {
        c.runtimeDir = value;
    }
    else if (name == "game")
    {
        c.game = value;
    }
    else if (name == "core")
    {
        c.core = value;
    }
    else if (name == "log")
    {
        c.log = value;
    }
    else if (name == "texture-pack")
    {
        c.texturePack = value;
    }
    else if (name == "server")
    {
        c.server = value;
    }
    else if (name == "game-port")
    {
        c.gamePort = port(value);
    }
    else if (name == "profile-port")
    {
        c.profilePort = port(value);
    }
    else if (name == "pol-id")
    {
        c.polId = value;
    }
    else if (name == "password")
    {
        c.password = value;
    }
    else if (name == "diagnostics" && (value == "true" || value == "false"))
    {
        c.diagnostics = value == "true";
    }
    else
    {
        throw std::runtime_error("Unknown/invalid setting: " + name);
    }
}

void readConfig(Config& c, const std::filesystem::path& file)
{
    std::ifstream in(file);
    if (!in)
    {
        throw std::runtime_error("Cannot read config: " + file.string());
    }
    std::set<std::string> seen;
    std::string           line;
    while (std::getline(in, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
        {
            continue;
        }
        const auto at = line.find('=');
        if (at == std::string::npos)
        {
            throw std::runtime_error("Config line requires key=value");
        }
        const auto name = trim(line.substr(0, at));
        if (!seen.insert(name).second)
        {
            throw std::runtime_error("Duplicate config key: " + name);
        }
        assign(c, name, trim(line.substr(at + 1)));
    }
    if (!c.runtimeDir.empty() && c.runtimeDir.is_relative())
    {
        c.runtimeDir = std::filesystem::absolute(file).parent_path() / c.runtimeDir;
    }
}

} // namespace

auto parseConfig(const std::vector<std::string>& args) -> Config
{
    Config                c;
    std::filesystem::path configFile;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == "--config")
        {
            if (++i == args.size() || !configFile.empty())
            {
                throw std::runtime_error("Specify --config once with a filename");
            }
            configFile = args[i];
        }
    }
    if (!configFile.empty())
    {
        readConfig(c, configFile);
    }
    for (size_t i = 0; i < args.size(); ++i)
    {
        const auto& key = args[i];
        if (key == "--help" || key == "-h")
        {
            c.help = true;
        }
        else if (key == "--version")
        {
            c.version = true;
        }
        else if (key == "--check")
        {
            c.checkOnly = true;
        }
        else if (key == "--diagnostics")
        {
            c.diagnostics = true;
        }
        else if (key == "--config")
        {
            ++i;
        }
        else
        {
            if (!key.starts_with("--") || ++i == args.size())
            {
                throw std::runtime_error("Expected --option value: " + key);
            }
            assign(c, key.substr(2), args[i]);
        }
    }
    if (c.help || c.version)
    {
        return c;
    }
    if (c.runtimeDir.empty())
    {
        throw std::runtime_error("--runtime-dir is required (or runtime-dir in --config)");
    }
    c.runtimeDir       = std::filesystem::absolute(c.runtimeDir).lexically_normal();
    const auto resolve = [&](std::filesystem::path& p, const char* fallback)
    {
        if (p.empty())
        {
            p = fallback;
        }
        if (p.is_relative())
        {
            p = c.runtimeDir / p;
        }
        p = std::filesystem::absolute(p).lexically_normal();
    };
    resolve(c.game, "TM_native.dll");
    resolve(c.core, "polcore_native.dll");
    resolve(c.log, "logs/tmloader.log");
    resolve(c.texturePack, "textures/pack.json");
    if (!isIpv4Address(c.server))
    {
        throw std::runtime_error("--server must be a numeric IPv4 address");
    }
    if (c.gamePort == c.profilePort)
    {
        throw std::runtime_error("Game and profile ports must differ");
    }
    if (c.profilePort == 51241)
    {
        throw std::runtime_error("Profile port cannot use the client's reserved GameIRC port 51241");
    }
    if (c.polId.size() != 8 || c.polId.find_first_not_of(polIdAlphabet) != std::string::npos)
    {
        throw std::runtime_error("--pol-id must be an eight-character PlayOnline ID");
    }
    if (c.password.empty() || c.password.size() > 15 || c.password.find_first_of("\r\n") != std::string::npos)
    {
        throw std::runtime_error("Password must contain 1..15 bytes without CR/LF");
    }
    if (!outputPathAllowed(c, c.log))
    {
        throw std::runtime_error("Log must be in runtime/logs and cannot alias an input DLL or asset");
    }
    if (c.diagnostics && !outputPathAllowed(c, c.runtimeDir / "logs/captures/frame_00060.bmp"))
    {
        throw std::runtime_error("Diagnostic capture directory escapes the runtime/logs output tree");
    }
    return c;
}

auto usage() -> const char*
{
    return "tmloader 0.1.0 - 32-bit Tetra Master host\n"
           "Usage: tmloader --runtime-dir DIR [options]\n"
           "  --config FILE          Read key=value configuration; CLI overrides it\n"
           "  --game FILE            Game DLL (default TM_native.dll in runtime dir)\n"
           "  --core FILE            Viewer DLL (default polcore_native.dll)\n"
           "  --server IPV4          Service IPv4 address (default 127.0.0.1)\n"
           "  --game-port PORT       Game service (default 51241)\n"
           "  --profile-port PORT    Profile service (default 51220)\n"
           "  --pol-id ID            Synthetic account (default EEEEEEEF)\n"
           "  --password VALUE       Synthetic password (default tm-local-test)\n"
           "  --log FILE             Log inside runtime/logs (default logs/tmloader.log)\n"
           "  --texture-pack FILE    HD manifest (default runtime/textures/pack.json)\n"
           "  --diagnostics          Extra renderer logs and frame captures\n"
           "  --check                Read-only files/hash/config checks; no game launch\n"
           "  --help | --version\n";
}

} // namespace tmloader
