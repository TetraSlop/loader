#include "texture_hooks.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace tmloader;
namespace {
constexpr DWORD imageSize = 0x1ba000;
constexpr DWORD uploadRva = 0x1b9aa4;
constexpr std::array<DWORD, 7> sites{
    0x17ba39, 0x17c061, 0x17c1f5, 0x17c393, 0x17c528, 0x17ccfa, 0x17ce91};
unsigned calls = 0;

HRESULT WINAPI replacement(void* device, const void* source, UINT bytes,
    UINT width, UINT height, UINT levels, DWORD usage, DWORD format, DWORD pool,
    DWORD filter, DWORD mipFilter, DWORD key, void* info, void* palette, void** output) {
    assert(device == reinterpret_cast<void*>(1) && source == reinterpret_cast<void*>(2));
    assert(bytes == 3 && width == 4 && height == 5 && levels == 6 && usage == 7);
    assert(format == 8 && pool == 9 && filter == 10 && mipFilter == 11 && key == 12);
    assert(info == reinterpret_cast<void*>(13) && palette == reinterpret_cast<void*>(14));
    *output = reinterpret_cast<void*>(15);
    ++calls;
    return 12345;
}

void initialize(BYTE* image) {
    std::memset(image, 0xcc, imageSize);
    const BYTE prologue[]{0x55, 0x8b, 0xec, 0xff, 0x75, 0x40, 0x6a, 0x03};
    const BYTE end[]{0xc2, 0x3c, 0x00};
    std::memcpy(image + uploadRva, prologue, sizeof(prologue));
    std::memcpy(image + 0x1b9ae1, end, sizeof(end));
    for (DWORD rva : sites) {
        BYTE* entry = image + rva - 48;
        const BYTE frame[]{0x55, 0x8b, 0xec};
        std::memcpy(entry, frame, sizeof(frame));
        entry += sizeof(frame);
        // Synthetic stdcall caller: forward each of the fifteen arguments.
        for (BYTE offset = 0x40; offset >= 8; offset -= 4) {
            *entry++ = 0xff; *entry++ = 0x75; *entry++ = offset;
        }
        assert(entry == image + rva);
        *entry++ = 0xe8;
        const DWORD relative = uploadRva - rva - 5;
        std::memcpy(entry, &relative, sizeof(relative));
        entry += sizeof(relative);
        const BYTE finish[]{0x5d, 0xc2, 0x3c, 0x00};
        std::memcpy(entry, finish, sizeof(finish));
    }
}

struct Image {
    BYTE* data = static_cast<BYTE*>(VirtualAlloc(nullptr, imageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Image() { assert(data); initialize(data); }
    ~Image() { VirtualFree(data, 0, MEM_RELEASE); }
    void seal() {
        DWORD old{};
        assert(VirtualProtect(data, imageSize, PAGE_EXECUTE_READ, &old));
        assert(FlushInstructionCache(GetCurrentProcess(), data, imageSize));
    }
};

DWORD protection(const BYTE* address) {
    MEMORY_BASIC_INFORMATION memory{};
    assert(VirtualQuery(address, &memory, sizeof(memory)));
    return memory.Protect;
}

template<class F> void rejected(F&& action) {
    bool threw = false;
    try { action(); } catch (const std::exception&) { threw = true; }
    assert(threw);
}

void successfulDispatch() {
    Image image;
    image.seal();
    const auto original = installTextureCallHooks(image.data, replacement);
    assert(original == reinterpret_cast<TextureUploadFn>(image.data + uploadRva));
    for (DWORD rva : sites) {
        assert(protection(image.data + rva) == PAGE_EXECUTE_READ);
        std::uint32_t displacement{};
        std::memcpy(&displacement, image.data + rva + 1, sizeof(displacement));
        assert(reinterpret_cast<std::uintptr_t>(image.data + rva + 5) + displacement ==
               reinterpret_cast<std::uintptr_t>(replacement));
        auto caller = reinterpret_cast<TextureUploadFn>(image.data + rva - 48);
        void* output = nullptr;
        assert(caller(reinterpret_cast<void*>(1), reinterpret_cast<void*>(2), 3, 4, 5, 6,
                      7, 8, 9, 10, 11, 12, reinterpret_cast<void*>(13),
                      reinterpret_cast<void*>(14), &output) == 12345);
        assert(output == reinterpret_cast<void*>(15));
    }
    assert(calls == sites.size());
    // A second installation is rejected rather than creating recursive dispatch.
    rejected([&] { installTextureCallHooks(image.data, replacement); });
}

void unsupportedCodeIsUntouched() {
    std::vector<DWORD> corrupt{uploadRva, 0x1b9ae1};
    for (DWORD rva : sites) corrupt.push_back(rva + 1);
    for (DWORD rva : corrupt) {
        Image image;
        image.data[rva] ^= 1;
        std::vector<BYTE> before(image.data, image.data + imageSize);
        image.seal();
        rejected([&] { installTextureCallHooks(image.data, replacement); });
        assert(std::memcmp(image.data, before.data(), imageSize) == 0);
        for (DWORD site : sites) assert(protection(image.data + site) == PAGE_EXECUTE_READ);
    }
    rejected([&] { installTextureCallHooks(nullptr, replacement); });
    Image image;
    rejected([&] { installTextureCallHooks(image.data, nullptr); });
    DWORD old{};
    assert(VirtualProtect(image.data + 0x17c000, 0x1000, PAGE_NOACCESS, &old));
    const std::array<BYTE, 5> originalCall{0xe8, 0x66, 0xe0, 0x03, 0x00};
    rejected([&] { installTextureCallHooks(image.data, replacement); });
    assert(std::memcmp(image.data + sites[0], originalCall.data(), originalCall.size()) == 0);
}

void protectionFailureIsUntouched() {
    // This mapping supports reading/writing but not executable protection. A
    // read-only view remains readable for guards; the patch acquisition fails.
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, imageSize, nullptr);
    assert(mapping);
    BYTE* writable = static_cast<BYTE*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, imageSize));
    assert(writable);
    initialize(writable);
    std::vector<BYTE> before(writable, writable + imageSize);
    assert(UnmapViewOfFile(writable));
    BYTE* readonly = static_cast<BYTE*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, imageSize));
    assert(readonly && protection(readonly + sites[0]) == PAGE_READONLY);
    rejected([&] { installTextureCallHooks(readonly, replacement); });
    assert(std::memcmp(readonly, before.data(), imageSize) == 0);
    for (DWORD site : sites) assert(protection(readonly + site) == PAGE_READONLY);
    assert(UnmapViewOfFile(readonly));
    assert(CloseHandle(mapping));
}
}

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    successfulDispatch();
    unsupportedCodeIsUntouched();
    protectionFailureIsUntouched();
    std::cout << "Texture hook call guards, stdcall dispatch, and protection cleanup passed.\n";
}
