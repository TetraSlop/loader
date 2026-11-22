// Command-line entry for Node and native runs. The boot to Direct3DCreate8 lands here with milestone 2.
#include "core/machine.h"

#include <cstdio>

auto main() -> int
{
    auto machine = tmweb::Machine{};
    std::printf("tmweb: guest memory base %#llx\n", static_cast<unsigned long long>(machine.memory().base()));
    return 0;
}
