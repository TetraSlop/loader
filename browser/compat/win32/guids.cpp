// From compat/win32/impl/com.cpp: the GUIDs unknwnbase.h and cguid.h declare and uuid.lib defines.
#include <objbase.h>

extern "C"
{
    const IID GUID_NULL    = {};
    const IID IID_IUnknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
}
