#include "config/config.h"
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace tmloader;

namespace
{

constexpr const char* kReadOnlyFiles[] = { "TM_native.dll", "polcore_native.dll", "data/gW201.dat", "pub/home00/option.bin" };

// Keep all fixture writes under the test binary's build directory.
auto fixturePath(const char* name) -> std::filesystem::path
{
    return std::filesystem::current_path() / name;
}

} // namespace

TEST_CASE("help and version need no runtime directory", "[config]")
{
    REQUIRE(parseConfig({ "--help" }).help);
    REQUIRE(parseConfig({ "--version" }).version);
}

TEST_CASE("a runtime directory is required", "[config]")
{
    REQUIRE_THROWS_AS(parseConfig({}), std::exception);
    REQUIRE_THROWS_AS(parseConfig({ "--runtime-dir" }), std::exception);
}

TEST_CASE("profile aliases match pp000 to pp999 only", "[config]")
{
    for (const auto* host : { "pp000.pol.com", "pp001.pol.com", "PP999.POL.COM" })
    {
        CAPTURE(host);
        REQUIRE(isProfileAlias(host));
    }

    for (const auto* host : { "pp00.pol.com", "pp0000.pol.com", "ppA01.pol.com", "pp001.pol.com.evil", "other.pol.com", "" })
    {
        CAPTURE(host);
        REQUIRE_FALSE(isProfileAlias(host));
    }

    REQUIRE_FALSE(isProfileAlias(nullptr));
}

TEST_CASE("IPv4 addresses must be dotted quads other than 0.0.0.0", "[config]")
{
    REQUIRE(isIpv4Address("127.0.0.1"));
    REQUIRE(isIpv4Address("192.0.2.1"));
    REQUIRE_FALSE(isIpv4Address("example.com"));
    REQUIRE_FALSE(isIpv4Address("999.0.0.1"));
    REQUIRE_FALSE(isIpv4Address("0.0.0.0"));
}

TEST_CASE("a runtime directory alone yields the local defaults", "[config]")
{
    const auto config = parseConfig({ "--runtime-dir", ".", "--check" });
    REQUIRE(config.checkOnly);
    REQUIRE(config.game.filename() == "TM_native.dll");
    REQUIRE(config.viewer.filename() == "polcore_native.dll");
    REQUIRE(config.polId == "EEEEEEEF");
    REQUIRE(config.gamePort == 51241);
    REQUIRE(config.profilePort == 51220);
}

TEST_CASE("the game port must be a number from 1 to 65535", "[config]")
{
    for (const auto* value : { "0", "65536", "-1", "x", "1x" })
    {
        CAPTURE(value);
        REQUIRE_THROWS_AS(parseConfig({ "--runtime-dir", ".", "--game-port", value }), std::exception);
    }
}

TEST_CASE("the profile port cannot share or take the game port", "[config]")
{
    REQUIRE_THROWS_AS(parseConfig({ "--runtime-dir", ".", "--game-port", "51220" }), std::exception);
    REQUIRE_THROWS_AS(parseConfig({ "--runtime-dir", ".", "--game-port", "52000", "--profile-port", "51241" }), std::exception);
}

TEST_CASE("unknown options are rejected", "[config]")
{
    REQUIRE_THROWS_AS(parseConfig({ "--runtime-dir", ".", "--unknown", "a" }), std::exception);
}

TEST_CASE("a malformed POL ID or an empty password is rejected", "[config]")
{
    REQUIRE_THROWS_AS(parseConfig({ "--runtime-dir", ".", "--pol-id", "invalid" }), std::exception);
    REQUIRE_THROWS_AS(parseConfig({ "--runtime-dir", ".", "--password", "" }), std::exception);
}

TEST_CASE("output paths outside the logs folder are refused", "[config]")
{
    const auto local = parseConfig({ "--runtime-dir", "." });
    for (const auto* path : { "../outside.log", "TM_native.dll", "data/gW201.dat", "pub/home00/option.bin", "logs/../data/gW201.dat", "logs-other/output.log" })
    {
        CAPTURE(path);
        REQUIRE_FALSE(outputPathAllowed(local, local.runtimeDir / path));
    }
}

TEST_CASE("command line options override the config file", "[config]")
{
    const auto fixture = fixturePath("config-override.ini");
    {
        std::ofstream f(fixture);
        f << "runtime-dir=.\ngame-port=52000\npassword=abc=xyz\ndiagnostics=false\n";
    }

    const auto config = parseConfig({ "--config", fixture.string(), "--game-port", "52001", "--diagnostics" });
    REQUIRE(config.gamePort == 52001);
    REQUIRE(config.password == "abc=xyz");
    REQUIRE(config.diagnostics);
    std::filesystem::remove(fixture);
}

TEST_CASE("a config file cannot repeat a key", "[config]")
{
    const auto fixture = fixturePath("config-duplicate.ini");
    {
        std::ofstream f(fixture);
        f << "runtime-dir=.\nruntime-dir=.\n";
    }

    REQUIRE_THROWS_AS(parseConfig({ "--config", fixture.string() }), std::exception);
    std::filesystem::remove(fixture);
}

TEST_CASE("sha256File hashes the file contents", "[config]")
{
    const auto fixture = fixturePath("config-sha256.bin");
    {
        std::ofstream f(fixture, std::ios::binary);
        f << "abc";
    }

    REQUIRE(sha256File(fixture) == "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    std::filesystem::remove(fixture);
}

TEST_CASE("only the logs and settings folders are writable", "[config]")
{
    const auto runtime = fixturePath("output-path-fixture");
    REQUIRE_FALSE(std::filesystem::exists(runtime)); // Never reuse/remove an unrelated existing directory.
    std::filesystem::create_directories(runtime / "data");
    std::filesystem::create_directories(runtime / "pub/home00");
    std::filesystem::create_directories(runtime / "logs/screenshots");
    const auto runtimeConfig = parseConfig({ "--runtime-dir", runtime.string() });
    for (const auto* relative : kReadOnlyFiles)
    {
        {
            std::ofstream f(runtime / relative);
            f << "read-only-fixture";
        }

        CAPTURE(relative);
        REQUIRE_FALSE(outputPathAllowed(runtimeConfig, runtime / relative));
    }

    const auto config = parseConfig({ "--runtime-dir", runtime.string(), "--diagnostics" });
    REQUIRE(outputPathAllowed(config, config.log));
    REQUIRE(outputPathAllowed(config, runtime / "logs/screenshots/shot.png"));
    REQUIRE(settingsPathAllowed(config, runtime / "settings/EEEEEEEF.json"));
    REQUIRE_FALSE(settingsPathAllowed(config, runtime / "data/gW201.dat"));
    REQUIRE_FALSE(settingsPathAllowed(config, runtime / "settings/../pub/home00/option.bin"));
    REQUIRE_FALSE(outputPathAllowed(config, runtime / "settings/EEEEEEEF.json"));

    // A hard link inside an allowed folder must not expose a protected file.
    std::filesystem::create_directories(runtime / "settings");
    const auto settingsAlias = runtime / "settings/EEEEEEEF.json";
    std::filesystem::create_hard_link(runtime / "pub/home00/option.bin", settingsAlias);
    REQUIRE_FALSE(settingsPathAllowed(config, settingsAlias));
    std::filesystem::remove(settingsAlias);
    const auto alias = runtime / "logs/screenshots/shot.png";
    std::filesystem::create_hard_link(runtime / "data/gW201.dat", alias);
    REQUIRE_FALSE(outputPathAllowed(config, alias));
    std::filesystem::remove(alias);

    for (const auto* relative : kReadOnlyFiles)
    {
        CAPTURE(relative);
        std::ifstream f(runtime / relative);
        std::string   text;
        std::getline(f, text);
        REQUIRE(text == "read-only-fixture"); // Validation never truncated any fixture.
    }

    std::filesystem::remove_all(runtime);
}

namespace
{

auto lowerHex(std::string_view text) -> std::string
{
    auto digest = sha256Hex(text.data(), text.size());
    std::ranges::transform(digest, digest.begin(), [](unsigned char c) -> char
                           {
                               return static_cast<char>(std::tolower(c));
                           });
    return digest;
}

// A runtime folder with a game DLL, a data file and Viewer settings, and a list of all three.
struct RetailFixture
{
    std::filesystem::path runtime = fixturePath("retail-fixture");
    std::filesystem::path list    = fixturePath("retail-fixture.txt");

    RetailFixture()
    {
        REQUIRE_FALSE(std::filesystem::exists(runtime));
        write("TM_native.dll", "game");
        write("data/gW201.dat", "art");
        write("pub/home00/option.bin", "factory");
        auto out = std::ofstream(list, std::ios::binary);
        out << "# comment\r\n\r\n";
        out << lowerHex("game") << "  TM_native.dll\r\n";
        out << lowerHex("art") << " *data/gW201.dat\n";
        out << lowerHex("factory") << "  pub/home00/option.bin\n";
    }

    ~RetailFixture()
    {
        std::filesystem::remove_all(runtime);
        std::filesystem::remove(list);
    }

    RetailFixture(const RetailFixture&)                    = delete;
    auto operator=(const RetailFixture&) -> RetailFixture& = delete;

    void write(const char* name, std::string_view text) const
    {
        std::filesystem::create_directories((runtime / name).parent_path());
        std::ofstream(runtime / name, std::ios::binary) << text;
    }
};

} // namespace

TEST_CASE("a runtime matching its retail list passes", "[config]")
{
    const auto fixture = RetailFixture{};
    const auto report  = checkRetailFiles(fixture.list, fixture.runtime);
    REQUIRE(report.checked == 3);
    REQUIRE(report.missing.empty());
    REQUIRE(report.mismatched.empty());
    REQUIRE(report.changed.empty());
    REQUIRE(describeRetailFailures(report).empty());
}

TEST_CASE("missing and changed retail files are reported", "[config]")
{
    const auto fixture = RetailFixture{};
    std::filesystem::remove(fixture.runtime / "TM_native.dll");
    fixture.write("data/gW201.dat", "other art");
    const auto report = checkRetailFiles(fixture.list, fixture.runtime);
    REQUIRE(report.missing == std::vector<std::string>{ "TM_native.dll" });
    REQUIRE(report.mismatched == std::vector<std::string>{ "data/gW201.dat" });
    const auto text = describeRetailFailures(report);
    REQUIRE(text.starts_with("2 of 3 retail files"));
    REQUIRE(text.contains("missing: TM_native.dll"));
    REQUIRE(text.contains("wrong hash: data/gW201.dat"));
}

TEST_CASE("saved Viewer settings are a warning, not a failure", "[config]")
{
    const auto fixture = RetailFixture{};
    fixture.write("pub/home00/option.bin", "saved");
    const auto report = checkRetailFiles(fixture.list, fixture.runtime);
    REQUIRE(report.changed == std::vector<std::string>{ "pub/home00/option.bin" });
    REQUIRE(report.mismatched.empty());
    REQUIRE(describeRetailFailures(report).empty());
}

TEST_CASE("a long failure list is cut short", "[config]")
{
    auto report     = RetailFileReport{};
    report.checked  = 30;
    report.missing  = std::vector<std::string>(25, "data/x.dat");
    const auto text = describeRetailFailures(report, 20);
    REQUIRE(text.contains("... and 5 more"));
}

TEST_CASE("malformed or escaping retail list lines are rejected", "[config]")
{
    const auto runtime = fixturePath("retail-bad-fixture");
    const auto list    = fixturePath("retail-bad-fixture.txt");
    const auto digest  = lowerHex("x");
    for (const auto& line : { std::string("not a hash  file"), digest + " file", digest.substr(1) + "  file", digest + "  ../outside.dll", digest + "  C:/outside.dll", digest + "  /outside.dll" })
    {
        CAPTURE(line);
        std::ofstream(list, std::ios::binary) << line << '\n';
        REQUIRE_THROWS_AS(checkRetailFiles(list, runtime), std::runtime_error);
    }

    std::filesystem::remove(list);
}

TEST_CASE("the shipped retail list parses", "[config]")
{
    const auto report = checkRetailFiles(TMLOADER_RETAIL_FILES, fixturePath("retail-empty-runtime"));
    REQUIRE(report.checked > 100);
    REQUIRE(report.missing.size() == report.checked);
}
