#include "app/abi.h"
#include "app/hooks.h"
#include "app/log.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "textures/texture_replacements.h"
#include "ui/window.h"
#include <stdexcept>

namespace tmloader
{
namespace
{

constexpr DWORD gameStartRva   = 0x8210;
constexpr DWORD commonTableRva = 0x6fbe8;

auto createObject(HMODULE module, REFCLSID clsid, REFIID iid) -> IUnknown*
{
    const auto getClass = reinterpret_cast<abi::GetClass>(GetProcAddress(module, "DllGetClassObject"));
    if (!getClass)
    {
        throw std::runtime_error("DLL does not export DllGetClassObject");
    }

    IClassFactory* factory = nullptr;
    if (FAILED(getClass(clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory))) || !factory)
    {
        throw std::runtime_error("Could not obtain DLL class factory");
    }

    IUnknown*  object = nullptr;
    const auto hr     = factory->CreateInstance(nullptr, iid, reinterpret_cast<void**>(&object));
    factory->Release();
    if (FAILED(hr) || !object)
    {
        throw std::runtime_error("Could not create DLL COM object");
    }

    return object;
}

void checked(HRESULT hr, const char* operation)
{
    logLine("{}={:08X}", operation, hex(hr));
    if (FAILED(hr))
    {
        throw std::runtime_error(operation);
    }
}

auto loadModule(const std::filesystem::path& path, const char* error) -> HMODULE
{
    const auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module)
    {
        throw std::runtime_error(error);
    }

    return module;
}

void initializeProcess(const Config& config)
{
    if (!SetCurrentDirectoryW(config.runtimeDir.c_str()) || !GetCurrentDirectoryA(MAX_PATH, runtimeRoot))
    {
        throw std::runtime_error("Cannot enter runtime directory");
    }

    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(logUnhandledException);
    logLine("tmloader " TMLOADER_VERSION " x86; pid={}; runtime={}", GetCurrentProcessId(), runtimeRoot);
    // The game DLL executes generated code from data pages.
    const auto dep = SetProcessDEPPolicy(0);
    logLine("Legacy process DEP={} error={}", dep, GetLastError());
    allowLowIntegrityWrites(config.runtimeDir);
    if (!lowerIntegrity())
    {
        throw std::runtime_error("Cannot initialize low-integrity runtime");
    }

    checked(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "COM initialization");
    ImGui_ImplWin32_EnableDpiAwareness();
}

auto loadGame(const Config& config) -> IUnknown*
{
    const auto game    = loadModule(config.game, "Cannot load game DLL");
    gameBase           = reinterpret_cast<BYTE*>(game);
    auto* const object = createObject(game, abi::gameClass, abi::gameInterface);
    if ((*reinterpret_cast<void***>(object))[abi::gameStartSlot] != gameBase + gameStartRva)
    {
        throw std::runtime_error("Unsupported live GameStart layout");
    }

    installSystemHooks(game, true);
    installNetworkHooks(game);
    installDesktopHooks(game);
    installGraphicsHooks(game);
    installInputHooks(game);
    installAudioHooks(game);
    initializeTextureReplacements(gameBase, config.texturePack);
    return object;
}

auto loadViewer(const Config& config) -> IUnknown*
{
    const auto module = loadModule(config.core, "Cannot load Viewer DLL");
    installSystemHooks(module, false);
    installNetworkHooks(module);
    auto* const core = createObject(module, abi::coreClass, abi::coreInterface);
    checked(callMethod(core, abi::SetAreaCode, 1L), "Viewer area");
    static char commandLine[] = " /game DdcMLbB";
    checked(callMethod(core, abi::SetParamInit, GetModuleHandleW(nullptr), commandLine), "Viewer parameters");

    Command* table = nullptr;
    checked(callMethod(core, abi::GetCommonTable, reinterpret_cast<unsigned long**>(&table)), "Viewer command table");
    if (reinterpret_cast<BYTE*>(table) != reinterpret_cast<BYTE*>(module) + commonTableRva)
    {
        throw std::runtime_error("Unsupported live Viewer command-table layout");
    }

    table[abi::viewer::setRegistryLanguage](1);
    table[abi::viewer::setUiLanguage](1);
    table[abi::viewer::setRegistryRoot]("SOFTWARE\\PlayOnlineUS");
    table[abi::viewer::setInstallDirectory](runtimeRoot);
    logLine("Viewer file layer={:08X}", hex(table[abi::viewer::fileLayerStatus]()));
    initializeViewerSession(module, table);
    return core;
}

} // namespace

auto launch(const Config& config) -> int
{
    options = config;
    initializeProcess(config);
    auto* const game = loadGame(config);
    auto* const core = loadViewer(config);

    auto descriptor = abi::LaunchDescriptor{ sizeof(abi::LaunchDescriptor), 0x200, abi::coreInterface, nullptr };
    logLine("Entering original online GameStart");
    const auto gameStart = reinterpret_cast<abi::GameStart>((*reinterpret_cast<void***>(game))[abi::gameStartSlot]);
    const auto result    = gameStart(game, core, &descriptor);
    logLine("GameStart returned {:08X}", hex(result));
    stopSettingsPanel();
    game->Release();
    return static_cast<int>(FAILED(result));
}

} // namespace tmloader
