// The Microsoft C runtime's extensions over musl; file functions take Windows paths and pass them through kr_path.
#pragma once

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

using errno_t    = int;
using __time32_t = int32_t;
using __time64_t = int64_t;
#ifndef _TRUNCATE
#define _TRUNCATE ((size_t)-1)
#endif

extern "C"
{
    // Emscripten takes '/' only; a drive letter stays, as Node's file system takes it. Four static buffers rotate.
    const char* kr_path(const char* path);
}

inline int _stricmp(const char* a, const char* b)
{
    return strcasecmp(a, b);
}

inline int _strnicmp(const char* a, const char* b, size_t n)
{
    return strncasecmp(a, b, n);
}

inline int _vsnprintf(char* out, size_t size, const char* format, va_list args)
{
    return vsnprintf(out, size, format, args);
}

#define _snprintf snprintf
// mingw-w64's strsafe.h calls it
#define _vsnwprintf vswprintf

inline errno_t strcpy_s(char* dst, size_t size, const char* src)
{
    if (dst == nullptr || size == 0)
    {
        return EINVAL;
    }

    const auto length = strlen(src);
    if (length >= size)
    {
        dst[0] = '\0';
        return ERANGE;
    }

    memcpy(dst, src, length + 1);
    return 0;
}

template <size_t N>
inline errno_t strcpy_s(char (&dst)[N], const char* src)
{
    return strcpy_s(dst, N, src);
}

inline errno_t strcat_s(char* dst, size_t size, const char* src)
{
    const auto length = strnlen(dst, size);
    if (length == size)
    {
        return EINVAL;
    }

    return strcpy_s(dst + length, size - length, src);
}

template <size_t N>
inline errno_t strcat_s(char (&dst)[N], const char* src)
{
    return strcat_s(dst, N, src);
}

inline errno_t strncpy_s(char* dst, size_t size, const char* src, size_t count)
{
    if (dst == nullptr || size == 0)
    {
        return EINVAL;
    }

    const auto length = [&]() -> size_t
    {
        const auto found = strnlen(src, count);
        if (count == _TRUNCATE && found >= size)
        {
            return size - 1;
        }

        return found;
    }();

    if (length >= size)
    {
        dst[0] = '\0';
        return ERANGE;
    }

    memcpy(dst, src, length);
    dst[length] = '\0';
    return 0;
}

template <size_t N>
inline errno_t strncpy_s(char (&dst)[N], const char* src, size_t count)
{
    return strncpy_s(dst, N, src, count);
}

inline errno_t memcpy_s(void* dst, size_t size, const void* src, size_t count)
{
    if (count > size)
    {
        return ERANGE;
    }

    memcpy(dst, src, count);
    return 0;
}

inline int vsprintf_s(char* out, size_t size, const char* format, va_list args)
{
    return vsnprintf(out, size, format, args);
}

template <size_t N>
inline int vsprintf_s(char (&out)[N], const char* format, va_list args)
{
    return vsnprintf(out, N, format, args);
}

inline int sprintf_s(char* out, size_t size, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const auto result = vsnprintf(out, size, format, args);
    va_end(args);
    return result;
}

template <size_t N>
inline int sprintf_s(char (&out)[N], const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const auto result = vsnprintf(out, N, format, args);
    va_end(args);
    return result;
}

inline int _snprintf_s(char* out, size_t size, size_t, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const auto result = vsnprintf(out, size, format, args);
    va_end(args);
    return result;
}

template <size_t N>
inline int _snprintf_s(char (&out)[N], size_t, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    const auto result = vsnprintf(out, N, format, args);
    va_end(args);
    return result;
}

inline errno_t fopen_s(FILE** file, const char* path, const char* mode)
{
    *file = fopen(path, mode);
    if (*file == nullptr)
    {
        return errno;
    }

    return 0;
}

inline size_t fread_s(void* buffer, size_t size, size_t elementSize, size_t count, FILE* file)
{
    const auto fits = [&]() -> size_t
    {
        if (elementSize != 0 && count > size / elementSize)
        {
            return size / elementSize;
        }

        return count;
    }();

    return fread(buffer, elementSize, fits, file);
}

inline void _lock_file(FILE* file)
{
    flockfile(file);
}

inline void _unlock_file(FILE* file)
{
    funlockfile(file);
}

struct _stat : stat
{
};

inline int _stat(const char* path, struct _stat* out)
{
    return stat(kr_path(path), out);
}

// The replay's clock, and no time zone: local time is UTC, as GetLocalTime's (impl/time.cpp).
extern "C" __time32_t _time32(__time32_t* out);

inline errno_t _localtime32_s(struct tm* out, const __time32_t* time)
{
    const auto t = static_cast<time_t>(*time);
    if (gmtime_r(&t, out) == nullptr)
    {
        return EINVAL;
    }

    return 0;
}

inline struct tm* _localtime32(const __time32_t* time)
{
    const auto t = static_cast<time_t>(*time);
    return gmtime(&t);
}

inline errno_t localtime_s(struct tm* out, const time_t* time)
{
    if (gmtime_r(time, out) == nullptr)
    {
        return EINVAL;
    }

    return 0;
}

#define _FPCLASS_SNAN 0x0001
#define _FPCLASS_QNAN 0x0002
#define _FPCLASS_NINF 0x0004
#define _FPCLASS_NN   0x0008
#define _FPCLASS_ND   0x0010
#define _FPCLASS_NZ   0x0020
#define _FPCLASS_PZ   0x0040
#define _FPCLASS_PD   0x0080
#define _FPCLASS_PN   0x0100
#define _FPCLASS_PINF 0x0200

inline int _fpclass(double value)
{
    const auto bySign = [&](int negative, int positive) -> int
    {
        if (std::signbit(value))
        {
            return negative;
        }

        return positive;
    };

    switch (std::fpclassify(value))
    {
        case FP_NAN:
            return _FPCLASS_QNAN;
        case FP_INFINITE:
            return bySign(_FPCLASS_NINF, _FPCLASS_PINF);
        case FP_ZERO:
            return bySign(_FPCLASS_NZ, _FPCLASS_PZ);
        case FP_SUBNORMAL:
            return bySign(_FPCLASS_ND, _FPCLASS_PD);
        default:
            return bySign(_FPCLASS_NN, _FPCLASS_PN);
    }
}

// Wasm has no x87 control word or FP exceptions, and device recovery, which sets the precision, is off in the replay.
#define _MCW_EM 0x0008001f
#define _MCW_PC 0x00030000
#define _PC_24  0x00020000
#define _PC_53  0x00010000

inline unsigned int _control87(unsigned int, unsigned int)
{
    return _MCW_EM | _PC_53;
}

inline int __control87_2(unsigned int, unsigned int, unsigned int* x87, unsigned int* sse)
{
    if (x87 != nullptr)
    {
        *x87 = _MCW_EM | _PC_53;
    }

    if (sse != nullptr)
    {
        *sse = _MCW_EM;
    }

    return 1;
}

inline void* _aligned_malloc(size_t size, size_t alignment)
{
    void* memory = nullptr;
    if (posix_memalign(&memory, alignment, size) != 0)
    {
        return nullptr;
    }

    return memory;
}

inline void _aligned_free(void* memory)
{
    free(memory);
}

// The client asks for _SPACE and _DIGIT only.
#define _DIGIT 0x0004
#define _SPACE 0x0008

inline int _isctype(int c, int mask)
{
    return ((mask & _SPACE) && isspace(c)) || ((mask & _DIGIT) && isdigit(c));
}

inline unsigned long _byteswap_ulong(unsigned long v)
{
    return __builtin_bswap32(v);
}

// mingw-w64's winnt.h and stralign.h call them
#define __debugbreak() __builtin_trap()
#define _alloca        __builtin_alloca
// Keeps mingw-w64's headers from marking the CRT's unchecked calls deprecated
#define _CRT_SECURE_NO_WARNINGS

// winnt.h brings the spin-wait hint in through intrin.h; Wasm has none.
inline void _mm_pause()
{
}
