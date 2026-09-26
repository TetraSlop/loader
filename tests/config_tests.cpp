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
    std::cout << "Config, endpoint aliases, path boundaries and SHA256 passed.\n";
}
