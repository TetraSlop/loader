#include "abi.h"
#include <stdexcept>

namespace tmloader {
namespace {
IUnknown* createObject(HMODULE module, REFCLSID clsid, REFIID iid) {
    const auto getClass = reinterpret_cast<abi::GetClass>(GetProcAddress(module, "DllGetClassObject"));
    if (!getClass) throw std::runtime_error("DLL does not export DllGetClassObject");
    IClassFactory* factory = nullptr;
    HRESULT hr = getClass(clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) throw std::runtime_error("Could not obtain DLL class factory");
    IUnknown* object = nullptr;
    hr = factory->CreateInstance(nullptr, iid, reinterpret_cast<void**>(&object));
    factory->Release();
    if (FAILED(hr) || !object) throw std::runtime_error("Could not create DLL COM object");
    return object;
}
void checked(HRESULT hr, const char* operation) {
    logf("%s=%08lX", operation, hr);
    if (FAILED(hr)) throw std::runtime_error(operation);
}
}

int launch(const Config& config) {
    options = config;
    openLog(config.log); // Open before dropping the integrity level.
    if (!SetCurrentDirectoryW(config.runtimeDir.c_str())) throw std::runtime_error("Cannot set runtime directory");
    if (!GetCurrentDirectoryA(MAX_PATH, runtimeRoot)) throw std::runtime_error("Cannot read runtime directory");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(crash);
    logf("tmloader 0.1.0 x86; pid=%lu; runtime=%s", GetCurrentProcessId(), runtimeRoot);
    BOOL dep = SetProcessDEPPolicy(0);
    logf("Legacy process DEP=%d error=%lu", dep, GetLastError());
    if (!lowerIntegrity()) throw std::runtime_error("Cannot initialize low-integrity runtime");
    checked(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "COM initialization");

    HMODULE game = LoadLibraryExW(config.game.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!game) throw std::runtime_error("Cannot load game DLL");
    gameBase = reinterpret_cast<BYTE*>(game);
    IUnknown* gameObject = createObject(game, abi::gameClass, abi::gameInterface);
    void** gameVtable = *reinterpret_cast<void***>(gameObject);
    if (gameVtable[9] != gameBase + 0x8210) throw std::runtime_error("Unsupported live GameStart layout");
    installFileHooks(game, true);
    installNetworkHooks(game);
    setSlot(game, 0x204260, reinterpret_cast<void*>(localD3D8));
    setSlot(game, 0x204014, reinterpret_cast<void*>(tracedInput));

    HMODULE coreModule = LoadLibraryExW(config.core.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!coreModule) throw std::runtime_error("Cannot load Viewer DLL");
    installFileHooks(coreModule, false);
    installNetworkHooks(coreModule);
    IUnknown* core = createObject(coreModule, abi::coreClass, abi::coreInterface);
    checked(abi::coreCall<abi::SetAreaCode>(core, 1UL), "Viewer area");
    static char cmd[] = " /game DdcMLbB";
    checked(abi::coreCall<abi::SetParamInit>(core, GetModuleHandleW(nullptr), cmd), "Viewer parameters");
    Command* table = nullptr;
    checked(abi::coreCall<abi::GetCommonTable>(core, &table), "Viewer command table");
    if (reinterpret_cast<BYTE*>(table) != reinterpret_cast<BYTE*>(coreModule) + 0x6fbe8)
        throw std::runtime_error("Unsupported live Viewer command-table layout");
    table[0x3c5](1); // POL_LANG / registry language.
    table[0x1a4](1); // Viewer UI language.
    table[0x16f]("SOFTWARE\\PlayOnlineUS");
    table[0x7d](runtimeRoot);
    logf("Viewer file layer=%08X", table[0x3a4]());
    initializeViewerSession(coreModule, table);

    abi::LaunchDescriptor descriptor{sizeof(descriptor), 0x200, abi::coreInterface, nullptr};
    logf("Entering original online GameStart");
    HRESULT result = reinterpret_cast<abi::GameStart>(gameVtable[9])(gameObject, core, &descriptor);
    logf("GameStart returned %08lX", result);
    gameObject->Release();
    // Preserve the working host's process-local exit until safe legacy DLL
    // teardown is verified. No watchdog, other-process termination or relaunch.
    TerminateProcess(GetCurrentProcess(), FAILED(result) ? 1 : 0);
    return FAILED(result) ? 1 : 0;
}
}
