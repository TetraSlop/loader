#include "config.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace tmloader;
template<typename F> void rejects(F fn) {
    bool failed = false;
    try { fn(); } catch (const std::exception&) { failed = true; }
    assert(failed);
}
int main() {
    assert(parseConfig({"--help"}).help);
    assert(parseConfig({"--version"}).version);
    rejects([] { parseConfig({}); });
    rejects([] { parseConfig({"--runtime-dir"}); });
    for (const auto* host : {"pp000.pol.com", "pp001.pol.com", "PP999.POL.COM"}) assert(isProfileAlias(host));
    for (const auto* host : {"pp00.pol.com", "pp0000.pol.com", "ppA01.pol.com", "pp001.pol.com.evil", "other.pol.com", ""}) assert(!isProfileAlias(host));
    assert(!isProfileAlias(nullptr));
    assert(isIpv4Address("127.0.0.1")); assert(isIpv4Address("192.0.2.1"));
    assert(!isIpv4Address("example.com")); assert(!isIpv4Address("999.0.0.1"));
    assert(!isIpv4Address("0.0.0.0"));
    auto c = parseConfig({"--runtime-dir", ".", "--check"});
    assert(c.game.filename() == "TM_native.dll" && c.core.filename() == "polcore_native.dll");
    assert(c.checkOnly && c.polId == "EEEEEEEF" && c.gamePort == 51241 && c.profilePort == 51220);
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
    assert(c.gamePort == 52001 && c.password == "abc=xyz" && c.diagnostics);
    { std::ofstream f(fixture); f << "runtime-dir=.\nruntime-dir=.\n"; }
    rejects([&] { parseConfig({"--config", fixture.string()}); });
    { std::ofstream f(fixture, std::ios::binary); f << "abc"; }
    assert(sha256File(fixture) == "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    std::filesystem::remove(fixture);

    const auto runtime = std::filesystem::current_path() / "output-path-fixture";
    assert(!std::filesystem::exists(runtime)); // Never reuse/remove an unrelated existing directory.
    std::filesystem::create_directories(runtime / "data");
    std::filesystem::create_directories(runtime / "pub/home00");
    std::filesystem::create_directories(runtime / "logs/captures");
    for (const auto* relative : {"TM_native.dll", "polcore_native.dll", "data/gW201.dat", "pub/home00/option.bin"}) {
        { std::ofstream f(runtime / relative); f << "read-only-fixture"; }
        rejects([&] { parseConfig({"--runtime-dir", runtime.string(), "--log", relative}); });
    }
    c = parseConfig({"--runtime-dir", runtime.string(), "--diagnostics"});
    assert(outputPathAllowed(c, c.log));
    assert(outputPathAllowed(c, runtime / "logs/captures/frame_00060.bmp"));
    assert(settingsPathAllowed(c, runtime / "settings/EEEEEEEF.window"));
    assert(!settingsPathAllowed(c, runtime / "data/gW201.dat"));
    assert(!settingsPathAllowed(c, runtime / "settings/../pub/home00/option.bin"));
    assert(!outputPathAllowed(c, runtime / "settings/EEEEEEEF.window"));
    std::filesystem::create_directories(runtime / "settings");
    const auto settingsAlias = runtime / "settings/EEEEEEEF.window";
    std::filesystem::create_hard_link(runtime / "pub/home00/option.bin", settingsAlias);
    assert(!settingsPathAllowed(c, settingsAlias));
    std::filesystem::remove(settingsAlias);
    const auto alias = runtime / "logs/captures/frame_00060.bmp";
    std::filesystem::create_hard_link(runtime / "data/gW201.dat", alias);
    assert(!outputPathAllowed(c, alias));
    rejects([&] { parseConfig({"--runtime-dir", runtime.string(), "--log", "logs/captures/frame_00060.bmp"}); });
    rejects([&] { parseConfig({"--runtime-dir", runtime.string(), "--diagnostics"}); });
    std::filesystem::remove(alias);
    for (const auto* relative : {"TM_native.dll", "polcore_native.dll", "data/gW201.dat", "pub/home00/option.bin"}) {
        std::ifstream f(runtime / relative); std::string text; std::getline(f, text);
        assert(text == "read-only-fixture"); // Validation never truncated any fixture.
    }
    std::filesystem::remove_all(runtime);
    std::cout << "Config, endpoint aliases, path boundaries and SHA256 passed.\n";
}
