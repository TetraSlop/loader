#include <catch2/catch_test_macros.hpp>
#include "config.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace tmloader;
template<typename F> void rejects(F fn) {
    REQUIRE_THROWS_AS(fn(), std::exception);
}
TEST_CASE("Configuration rejects unsafe paths and invalid endpoints", "[config]") {
    REQUIRE(parseConfig({"--help"}).help);
    REQUIRE(parseConfig({"--version"}).version);
    rejects([] { parseConfig({}); });
    rejects([] { parseConfig({"--runtime-dir"}); });
    for (const auto* host : {"pp000.pol.com", "pp001.pol.com", "PP999.POL.COM"}) REQUIRE(isProfileAlias(host));
    for (const auto* host : {"pp00.pol.com", "pp0000.pol.com", "ppA01.pol.com", "pp001.pol.com.evil", "other.pol.com", ""}) REQUIRE(!isProfileAlias(host));
    REQUIRE(!isProfileAlias(nullptr));
    REQUIRE(isIpv4Address("127.0.0.1")); REQUIRE(isIpv4Address("192.0.2.1"));
    REQUIRE(!isIpv4Address("example.com")); REQUIRE(!isIpv4Address("999.0.0.1"));
    REQUIRE(!isIpv4Address("0.0.0.0"));
    auto c = parseConfig({"--runtime-dir", ".", "--check"});
    REQUIRE((c.game.filename() == "TM_native.dll" && c.core.filename() == "polcore_native.dll"));
    REQUIRE(c.texturePack == c.runtimeDir / "textures/pack.json");
    REQUIRE(parseConfig({"--runtime-dir", ".", "--texture-pack", "custom/pack.json"}).texturePack == c.runtimeDir / "custom/pack.json");
    REQUIRE((c.checkOnly && c.polId == "EEEEEEEF" && c.gamePort == 51241 && c.profilePort == 51220));
    for (const auto* value : {"0", "65536", "-1", "x", "1x"})
        rejects([&] { parseConfig({"--runtime-dir", ".", "--game-port", value}); });
    rejects([] { parseConfig({"--runtime-dir", ".", "--game-port", "51220"}); });
    rejects([] { parseConfig({"--runtime-dir", ".", "--game-port", "52000", "--profile-port", "51241"}); });
    rejects([] { parseConfig({"--runtime-dir", ".", "--unknown", "a"}); });
    rejects([] { parseConfig({"--runtime-dir", ".", "--log", "../outside.log"}); });
    rejects([] { parseConfig({"--runtime-dir", ".", "--log", "TM_native.dll"}); });
    for (const auto* path : {"data/gW201.dat", "pub/home00/option.bin", "logs/../data/gW201.dat", "logs-other/output.log"})
        rejects([&] { parseConfig({"--runtime-dir", ".", "--log", path}); });
    rejects([] { parseConfig({"--runtime-dir", ".", "--pol-id", "invalid"}); });
    rejects([] { parseConfig({"--runtime-dir", ".", "--password", ""}); });

    // Keep all fixture writes under the test binary's build directory.
    const auto fixture = std::filesystem::current_path() / "config-test.ini";
    { std::ofstream f(fixture); f << "runtime-dir=.\ngame-port=52000\npassword=abc=xyz\ndiagnostics=false\n"; }
    c = parseConfig({"--config", fixture.string(), "--game-port", "52001", "--diagnostics"});
    REQUIRE((c.gamePort == 52001 && c.password == "abc=xyz" && c.diagnostics));
    { std::ofstream f(fixture); f << "runtime-dir=.\nruntime-dir=.\n"; }
    rejects([&] { parseConfig({"--config", fixture.string()}); });
    { std::ofstream f(fixture, std::ios::binary); f << "abc"; }
    REQUIRE(sha256File(fixture) == "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    std::filesystem::remove(fixture);

    const auto runtime = std::filesystem::current_path() / "output-path-fixture";
    REQUIRE(!std::filesystem::exists(runtime)); // Never reuse/remove an unrelated existing directory.
    std::filesystem::create_directories(runtime / "data");
    std::filesystem::create_directories(runtime / "pub/home00");
    std::filesystem::create_directories(runtime / "logs/captures");
    for (const auto* relative : {"TM_native.dll", "polcore_native.dll", "data/gW201.dat", "pub/home00/option.bin"}) {
        { std::ofstream f(runtime / relative); f << "read-only-fixture"; }
        rejects([&] { parseConfig({"--runtime-dir", runtime.string(), "--log", relative}); });
    }
    c = parseConfig({"--runtime-dir", runtime.string(), "--diagnostics"});
    REQUIRE(outputPathAllowed(c, c.log));
    REQUIRE(outputPathAllowed(c, runtime / "logs/captures/frame_00060.bmp"));
    REQUIRE(settingsPathAllowed(c, runtime / "settings/EEEEEEEF.window"));
    REQUIRE(!settingsPathAllowed(c, runtime / "data/gW201.dat"));
    REQUIRE(!settingsPathAllowed(c, runtime / "settings/../pub/home00/option.bin"));
    REQUIRE(!outputPathAllowed(c, runtime / "settings/EEEEEEEF.window"));
    std::filesystem::create_directories(runtime / "settings");
    const auto settingsAlias = runtime / "settings/EEEEEEEF.window";
    std::filesystem::create_hard_link(runtime / "pub/home00/option.bin", settingsAlias);
    REQUIRE(!settingsPathAllowed(c, settingsAlias));
    std::filesystem::remove(settingsAlias);
    const auto alias = runtime / "logs/captures/frame_00060.bmp";
    std::filesystem::create_hard_link(runtime / "data/gW201.dat", alias);
    REQUIRE(!outputPathAllowed(c, alias));
    rejects([&] { parseConfig({"--runtime-dir", runtime.string(), "--log", "logs/captures/frame_00060.bmp"}); });
    rejects([&] { parseConfig({"--runtime-dir", runtime.string(), "--diagnostics"}); });
    std::filesystem::remove(alias);
    for (const auto* relative : {"TM_native.dll", "polcore_native.dll", "data/gW201.dat", "pub/home00/option.bin"}) {
        std::ifstream f(runtime / relative); std::string text; std::getline(f, text);
        REQUIRE(text == "read-only-fixture"); // Validation never truncated any fixture.
    }
    std::filesystem::remove_all(runtime);
    std::cout << "Config, endpoint aliases, path boundaries and SHA256 passed.\n";
}
