#include "runtime.h"
#include <exception>

int main(int argc, char** argv) {
    try {
        const auto config = tmloader::parseConfig(std::vector<std::string>(argv + 1, argv + argc));
        if (config.help) { std::fputs(tmloader::usage(), stdout); return 0; }
        if (config.version) { std::puts("tmloader 0.1.0"); return 0; }
        tmloader::validateRuntime(config);
        if (config.checkOnly) {
            std::puts("Runtime and supported DLL hashes verified. No DLL loaded or game launched.");
            return 0;
        }
        return tmloader::launch(config);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "tmloader: %s\n", e.what());
        return 2;
    }
}
