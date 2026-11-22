#pragma once
#include "app/com.h"
#include "app/runtime.h"
#include <objbase.h>

namespace tmloader::abi
{

inline const IID kCoreInterface = { 0xe0516654, 0xef77, 0x435d, { 0xaa, 0x7d, 0x50, 0xd2, 0xc0, 0x69, 0xce, 0x34 } };

struct LaunchDescriptor
{
    DWORD size;
    DWORD flags;
    IID   core;
    void* message;
};

static_assert(sizeof(LaunchDescriptor) == 28, "The game requires its 32-bit launch ABI");

using GetClass  = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
using GameStart = HRESULT(__stdcall*)(IUnknown*, IUnknown*, LaunchDescriptor*);

constexpr DWORD kWindowedFlag = 0x2ff248;

// IPOLCoreCom vtable slots. 23/24 and 25/26 are the PlayOnline and SquareEnix
// registry-name getters; both pairs precede SetAreaCode.
enum CoreMethod : unsigned
{
    SetParamInit   = 5,
    GetCommonTable = 7,
    SetAreaCode    = 27,
};

// Indices into the Viewer's cdecl common-function table.
enum ViewerCommand : unsigned
{
    SetInstallDirectory = 125,
    GameIrcOpen         = 132,
    GameIrcOpenCheck    = 133,
    LoadHandle          = 171,
    LoadCharacter       = 175,
    LoadStatus          = 193,
    ProfileReadCheck    = 349,
    SetRegistryRoot     = 367,
    SetUiLanguage       = 420,
    SetAccount          = 851,
    EncodeAccount       = 854,
    FileLayerStatus     = 932,
    SetRegistryLanguage = 965,
    ViewerCommandCount  = 1200,
};

} // namespace tmloader::abi
