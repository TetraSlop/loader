#include "app/abi.h"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <iostream>

// Compiler-built vtable in IPOLCoreCom declaration order, independent of the loader's slot indices.
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

static auto __stdcall mockGameStart(IUnknown* game, IUnknown* core, tmloader::abi::LaunchDescriptor* launch) -> HRESULT
{
    REQUIRE((game && core));
    REQUIRE((launch->size == 28 && launch->flags == 0x200));
    REQUIRE((launch->core == tmloader::abi::coreInterface && !launch->message));
    return 123;
}

TEST_CASE("COM dispatch preserves native signatures and slots", "[abi]")
{
    using namespace tmloader;
    MockCore        core;
    IUnknown* const object    = &core;
    char            command[] = " /game DdcMLbB";
    const auto      instance  = reinterpret_cast<HINSTANCE>(0x12345678);
    REQUIRE(callMethod(object, abi::SetAreaCode, 1L) == S_OK);
    REQUIRE((core.areaCalls == 1 && core.area == 1 && core.wrongCalls == 0));
    // Signed long matches the declared ABI as well as the usual value1.
    REQUIRE((callMethod(object, abi::SetAreaCode, -7L) == S_OK && core.area == -7));
    REQUIRE(callMethod(object, abi::SetParamInit, instance, command) == S_OK);
    REQUIRE((core.paramCalls == 1 && core.instance == instance && core.command == command));
    Command* table = nullptr;
    REQUIRE(callMethod(object, abi::GetCommonTable, reinterpret_cast<unsigned long**>(&table)) == S_OK);
    REQUIRE((core.tableCalls == 1 && reinterpret_cast<unsigned long*>(table) == core.table.data()));
    REQUIRE(core.wrongCalls == 0);
    // Neighbouring slot 25 (GetSquareEnixRegKeyNameW) must not reach SetAreaCode.
    REQUIRE(callMethod(object, 25, static_cast<LPWSTR*>(nullptr)) == E_UNEXPECTED);
    REQUIRE((core.wrongCalls == 1 && core.areaCalls == 2));
    auto       descriptor = abi::LaunchDescriptor{ sizeof(abi::LaunchDescriptor), 0x200, abi::coreInterface, nullptr };
    const auto start      = abi::GameStart{ mockGameStart };
    REQUIRE(start(object, object, &descriptor) == 123);
    std::cout << "COM slot/signature and launch descriptor regression checks passed.\n";
}
