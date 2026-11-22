// INITGUID keeps combaseapi.h from declaring cguid.h's GUIDs, which uuid.lib defines; com.cpp defines GUID_NULL.
#if defined(_COMBASEAPI_H_) && !defined(__CGUID_H__)
#include <cguid.h>
#endif

// minwindef.h defines WIN32, which would turn on toml++'s Windows code.
#pragma pop_macro("WIN32")
#pragma pop_macro("_M_IX86")
#pragma pop_macro("__i386__")
#pragma pop_macro("_X86_")
#pragma pop_macro("_WIN32")
