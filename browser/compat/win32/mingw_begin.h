// mingw-w64's headers see an x86 Windows target only while read (mingw_end.h), so libc++, spdlog and the client don't.

// musl's select and fd_set would clash with Winsock's, whose fd_set the client fills in.
#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H
#endif

#include "corecrt.h"
#include "msvcrt.h"

#include <climits>
// The client builds its non-Windows socket branch, which calls fcntl.
#include <fcntl.h>
// musl's gethostname takes a size_t and Winsock's an int; the client calls Winsock's.
#define gethostname musl_gethostname
#include <unistd.h>
#undef gethostname

// Emscripten's xmmintrin.h names a struct __unaligned, a keyword under -fms-extensions, and _mingw.h would empty it.
#ifndef __unaligned
#define __unaligned kr_unaligned
#endif

// Clang drops overrides in templates derived from __declspec(nothrow) methods, which the SDK stand-ins never had.
#define COM_STDMETHOD_CAN_THROW

// The COM headers would open ole2.h, which is not vendored; windows.h always comes first.
#ifndef COM_NO_WINDOWS_H
#define COM_NO_WINDOWS_H
#endif

#pragma push_macro("_WIN32")
#pragma push_macro("_X86_")
#pragma push_macro("__i386__")
#pragma push_macro("_M_IX86")
#pragma push_macro("WIN32")
#define _WIN32   1
#define _X86_    1
#define __i386__ 1
// The value _mingw_mac.h gives a plain i386. Left defined, it sends the client's X87Math.h to its inline assembly.
#define _M_IX86 300
