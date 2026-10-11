#include "app/abi.h"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <iostream>

namespace
{

constexpr DWORD   kLaunchFlags     = 0x200;
constexpr HRESULT kGameStartResult = 123;

// Compiler-built vtable in IPOLCoreCom declaration order, independent of the loader's slot indices.
// A virtual destructor would shift the COM vtable.
// NOLINTNEXTLINE(cppcoreguidelines-virtual-class-destructor)
class MockCore : public IUnknown
{
public:
    unsigned                     areaCalls  = 0;
    unsigned                     paramCalls = 0;
    unsigned                     tableCalls = 0;
    unsigned                     wrongCalls = 0;
    LONG                         area       = -1;
    HINSTANCE                    instance   = nullptr;
    LPSTR                        command    = nullptr;
    std::array<unsigned long, 4> table{ 11, 22, 33, 44 };

    auto STDMETHODCALLTYPE QueryInterface(REFIID, void**) -> HRESULT override
    {
        return E_NOINTERFACE;
    }

    auto STDMETHODCALLTYPE AddRef() -> ULONG override
    {
        return 1;
    }

    auto STDMETHODCALLTYPE Release() -> ULONG override
    {
        return 1;
    }

    virtual auto __stdcall GethInstance(HINSTANCE*) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall GetlpCmdLine(LPSTR*) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall SetParamInit(HINSTANCE h, LPSTR text) -> HRESULT
    {
        ++paramCalls;
        instance = h;
        command  = text;
        return S_OK;
    }

    virtual auto __stdcall GetWindowsType(unsigned long*) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall GetCommonFunctionTable(unsigned long** out) -> HRESULT
    {
        ++tableCalls;
        *out = table.data();
        return S_OK;
    }

    virtual auto __stdcall PolViewerExec(unsigned long*) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall GetWindowsVersion(unsigned long*, unsigned long*, unsigned long*, unsigned long*, BSTR*, unsigned long) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall PressAnyKey(unsigned long) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall PolconSetEnableWakeupFuncFlag(unsigned long, unsigned long) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall CreateInput(void*) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall UpdateInputState() -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall GetPadRepeat(unsigned long, unsigned long*) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall GetPadOn(unsigned long, unsigned long*) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall FinalCleanup() -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall SetParamInitW(HINSTANCE, LPWSTR) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall GetlpCmdLineW(LPWSTR*) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall PaintFriendList(void*) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall CreateFriendList() -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall DestroyFriendList() -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall SetMaskWindowHandle(void*) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall GetPlayOnlineRegKeyNameW(LPWSTR*) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall GetPlayOnlineRegKeyNameA(LPSTR*) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall GetSquareEnixRegKeyNameW(LPWSTR*) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall GetSquareEnixRegKeyNameA(LPSTR*) -> HRESULT
    {
        return unexpected();
    }

    virtual auto __stdcall SetAreaCode(long value) -> HRESULT
    {
        ++areaCalls;
        area = value;
        return S_OK;
    }

private:
    auto unexpected() -> HRESULT
    {
        ++wrongCalls;
        return E_UNEXPECTED;
    }
};

auto __stdcall mockGameStart(IUnknown* game, IUnknown* core, tmloader::abi::LaunchDescriptor* launch) -> HRESULT
{
    REQUIRE(game);
    REQUIRE(core);
    REQUIRE(launch->size == 28);
    REQUIRE(launch->flags == kLaunchFlags);
    REQUIRE(launch->core == tmloader::abi::kCoreInterface);
    REQUIRE_FALSE(launch->message);
    return kGameStartResult;
}

} // namespace

using namespace tmloader;

TEST_CASE("SetAreaCode reaches its own slot with a signed argument", "[abi]")
{
    MockCore        core;
    IUnknown* const object = &core;

    REQUIRE(callMethod(object, abi::SetAreaCode, 1L) == S_OK);
    REQUIRE(core.areaCalls == 1);
    REQUIRE(core.area == 1);

    REQUIRE(callMethod(object, abi::SetAreaCode, -7L) == S_OK);
    REQUIRE(core.area == -7);
    REQUIRE(core.wrongCalls == 0);
}

TEST_CASE("SetParamInit forwards the instance and command line", "[abi]")
{
    MockCore        core;
    IUnknown* const object    = &core;
    char            command[] = " /game DdcMLbB";
    const auto      instance  = reinterpret_cast<HINSTANCE>(0x12345678);

    REQUIRE(callMethod(object, abi::SetParamInit, instance, command) == S_OK);
    REQUIRE(core.paramCalls == 1);
    REQUIRE(core.instance == instance);
    REQUIRE(core.command == command);
    REQUIRE(core.wrongCalls == 0);
}

TEST_CASE("GetCommonTable returns the core's function table", "[abi]")
{
    MockCore        core;
    IUnknown* const object = &core;
    Command*        table  = nullptr;

    REQUIRE(callMethod(object, abi::GetCommonTable, reinterpret_cast<unsigned long**>(&table)) == S_OK);
    REQUIRE(core.tableCalls == 1);
    REQUIRE(reinterpret_cast<unsigned long*>(table) == core.table.data());
    REQUIRE(core.wrongCalls == 0);
}

TEST_CASE("The slot next to SetAreaCode does not reach it", "[abi]")
{
    MockCore        core;
    IUnknown* const object = &core;

    // Slot 25 is GetSquareEnixRegKeyNameW.
    REQUIRE(callMethod(object, 25, static_cast<LPWSTR*>(nullptr)) == E_UNEXPECTED);
    REQUIRE(core.wrongCalls == 1);
    REQUIRE(core.areaCalls == 0);
}

TEST_CASE("GameStart receives the launch descriptor", "[abi]")
{
    MockCore        core;
    IUnknown* const object     = &core;
    auto            descriptor = abi::LaunchDescriptor{ sizeof(abi::LaunchDescriptor), kLaunchFlags, abi::kCoreInterface, nullptr };
    const auto      start      = abi::GameStart{ mockGameStart };

    REQUIRE(start(object, object, &descriptor) == kGameStartResult);
}
