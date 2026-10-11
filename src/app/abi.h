#pragma once
#include "app/com.h"
#include "app/runtime.h"
#include <objbase.h>

namespace tmloader::abi
{

inline const CLSID kGameClass     = { 0x72b2fe03, 0xba77, 0x4867, { 0x84, 0xa2, 0x7b, 0xad, 0x0f, 0x5a, 0x8f, 0xbb } };
inline const IID   kGameInterface = { 0xfa287029, 0x5ec4, 0x4938, { 0xb8, 0xc5, 0x53, 0x28, 0x65, 0xdd, 0xe3, 0x69 } };
inline const CLSID kCoreClass     = { 0x3501f5dd, 0x7894, 0x42df, { 0x86, 0x6a, 0xa2, 0xb6, 0x52, 0x7d, 0x80, 0x49 } };
inline const IID   kCoreInterface = { 0xe0516654, 0xef77, 0x435d, { 0xaa, 0x7d, 0x50, 0xd2, 0xc0, 0x69, 0xce, 0x34 } };

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

constexpr unsigned kGameStartSlot = 9;
// RVA of the game's windowed-mode flag.
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
namespace viewer
{

constexpr unsigned kSetInstallDirectory = 125;
constexpr unsigned kGameIrcOpen         = 132;
constexpr unsigned kGameIrcOpenCheck    = 133;
constexpr unsigned kLoadHandle          = 171;
constexpr unsigned kLoadCharacter       = 175;
constexpr unsigned kLoadStatus          = 193;
constexpr unsigned kProfileReadCheck    = 349;
constexpr unsigned kSetRegistryRoot     = 367;
constexpr unsigned kSetUiLanguage       = 420;
constexpr unsigned kSetAccount          = 851;
constexpr unsigned kEncodeAccount       = 854;
constexpr unsigned kFileLayerStatus     = 932;
constexpr unsigned kSetRegistryLanguage = 965;
constexpr unsigned kTableSize           = 1200;

} // namespace viewer

} // namespace tmloader::abi
