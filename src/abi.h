#pragma once
#include "runtime.h"

namespace tmloader::abi {
// Independently declared ABI facts; no proprietary code/data is bundled.
inline const CLSID gameClass = {0x72b2fe03, 0xba77, 0x4867, {0x84, 0xa2, 0x7b, 0xad, 0x0f, 0x5a, 0x8f, 0xbb}};
inline const IID gameInterface = {0xfa287029, 0x5ec4, 0x4938, {0xb8, 0xc5, 0x53, 0x28, 0x65, 0xdd, 0xe3, 0x69}};
inline const CLSID coreClass = {0x3501f5dd, 0x7894, 0x42df, {0x86, 0x6a, 0xa2, 0xb6, 0x52, 0x7d, 0x80, 0x49}};
inline const IID coreInterface = {0xe0516654, 0xef77, 0x435d, {0xaa, 0x7d, 0x50, 0xd2, 0xc0, 0x69, 0xce, 0x34}};
struct LaunchDescriptor { DWORD size, flags; IID core; void* message; };
static_assert(sizeof(LaunchDescriptor) == 28, "The game requires its 32-bit launch ABI");
using GetClass = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
using GameStart = HRESULT(__stdcall*)(IUnknown*, IUnknown*, LaunchDescriptor*);
// IUnknown slots0..2; these Viewer method positions match the observed ABI.
enum CoreMethod { SetParamInit = 5, GetCommonTable = 7, SetAreaCode = 25 };
template<unsigned Slot, typename... Args>
HRESULT coreCall(IUnknown* object, Args... args) {
    using Fn = HRESULT(__stdcall*)(IUnknown*, Args...);
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[Slot])(object, args...);
}
}
