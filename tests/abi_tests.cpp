#include "abi.h"
#include <catch2/catch_test_macros.hpp>
#include <iostream>

// A compiler-built mock follows the complete IPOLCoreCom method order from
// the original C and C++ declarations. This independently tests the loader's
// numerical dispatch; neither indices nor its helper construct this vtable.
class MockCore : public IUnknown
{
public:
    unsigned      areaCalls = 0, paramCalls = 0, tableCalls = 0, wrongCalls = 0;
    LONG          area     = -1;
    HINSTANCE     instance = nullptr;
    LPSTR         command  = nullptr;
    unsigned long table[4]{ 11, 22, 33, 44 };

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override
    {
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return 1;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        return 1;
    }

    virtual HRESULT __stdcall GethInstance(HINSTANCE*)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall GetlpCmdLine(LPSTR*)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall SetParamInit(HINSTANCE h, LPSTR text)
    {
        ++paramCalls;
        instance = h;
        command  = text;
        return S_OK;
    }

    virtual HRESULT __stdcall GetWindowsType(unsigned long*)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall GetCommonFunctionTable(unsigned long** out)
    {
        ++tableCalls;
        *out = table;
        return S_OK;
    }

    virtual HRESULT __stdcall PolViewerExec(unsigned long*)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall GetWindowsVersion(unsigned long*, unsigned long*, unsigned long*, unsigned long*, BSTR*, unsigned long)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall PressAnyKey(unsigned long)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall PolconSetEnableWakeupFuncFlag(unsigned long, unsigned long)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall CreateInput(void*)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall UpdateInputState()
    {
        return unexpected();
    }

    virtual HRESULT __stdcall GetPadRepeat(unsigned long, unsigned long*)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall GetPadOn(unsigned long, unsigned long*)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall FinalCleanup()
    {
        return unexpected();
    }

    virtual HRESULT __stdcall SetParamInitW(HINSTANCE, LPWSTR)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall GetlpCmdLineW(LPWSTR*)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall PaintFriendList(void*)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall CreateFriendList()
    {
        return unexpected();
    }

    virtual HRESULT __stdcall DestroyFriendList()
    {
        return unexpected();
    }

    virtual HRESULT __stdcall SetMaskWindowHandle(void*)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall GetPlayOnlineRegKeyNameW(LPWSTR*)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall GetPlayOnlineRegKeyNameA(LPSTR*)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall GetSquareEnixRegKeyNameW(LPWSTR*)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall GetSquareEnixRegKeyNameA(LPSTR*)
    {
        return unexpected();
    }

    virtual HRESULT __stdcall SetAreaCode(long value)
    {
        ++areaCalls;
        area = value;
        return S_OK;
    }

private:
    HRESULT unexpected()
    {
        ++wrongCalls;
        return E_UNEXPECTED;
    }
};

static HRESULT __stdcall mockGameStart(IUnknown* game, IUnknown* core, tmloader::abi::LaunchDescriptor* launch)
{
    REQUIRE((game && core));
    REQUIRE((launch->size == 28 && launch->flags == 0x200));
    REQUIRE((launch->core == tmloader::abi::coreInterface && !launch->message));
    return 123;
}

TEST_CASE("COM dispatch preserves native signatures and slots", "[abi]")
{
    using namespace tmloader;
    MockCore  core;
    IUnknown* object    = &core;
    char      command[] = " /game DdcMLbB";
    HINSTANCE instance  = reinterpret_cast<HINSTANCE>(0x12345678);
    REQUIRE(abi::coreCall<abi::SetAreaCode>(object, 1L) == S_OK);
    REQUIRE((core.areaCalls == 1 && core.area == 1 && core.wrongCalls == 0));
    // Signed long matches the declared ABI as well as the usual value1.
    REQUIRE((abi::coreCall<abi::SetAreaCode>(object, -7L) == S_OK && core.area == -7));
    REQUIRE(abi::coreCall<abi::SetParamInit>(object, instance, command) == S_OK);
    REQUIRE((core.paramCalls == 1 && core.instance == instance && core.command == command));
    Command* table = nullptr;
    REQUIRE(abi::coreCall<abi::GetCommonTable>(object, reinterpret_cast<unsigned long**>(&table)) == S_OK);
    REQUIRE((core.tableCalls == 1 && reinterpret_cast<unsigned long*>(table) == core.table));
    REQUIRE(core.wrongCalls == 0);
    // Demonstrate that the old erroneous slot reaches a different method.
    REQUIRE(abi::coreCall<25>(object, static_cast<LPWSTR*>(nullptr)) == E_UNEXPECTED);
    REQUIRE((core.wrongCalls == 1 && core.areaCalls == 2));
    abi::LaunchDescriptor descriptor{ sizeof(descriptor), 0x200, abi::coreInterface, nullptr };
    abi::GameStart        start = mockGameStart;
    REQUIRE(start(object, object, &descriptor) == 123);
    std::cout << "COM slot/signature and launch descriptor regression checks passed.\n";
}
