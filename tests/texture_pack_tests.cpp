#ifdef NDEBUG
#undef NDEBUG
#endif
#include "texture_pack.h"
#include "texture_replacements.h"
#include <nlohmann/json.hpp>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace tmloader;
namespace fs = std::filesystem;
namespace tmloader { void logf(const char*, ...) {} }

// Structural PNG fixtures: decoding is deliberately left to the upload callback.
// This checks bounded matching independently of any PNG codec.
std::vector<uint8_t> png(unsigned width, unsigned height) {
    std::vector<uint8_t> bytes{137,80,78,71,13,10,26,10, 0,0,0,13, 'I','H','D','R'};
    for (unsigned value : {width, height})
        for (int shift : {24,16,8,0}) bytes.push_back(static_cast<uint8_t>(value >> shift));
    bytes.insert(bytes.end(), {8,6,0,0,0, 0,0,0,0, 0,0,0,0, 'I','E','N','D', 0,0,0,0});
    return bytes;
}
void write(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
template<class F> void rejects(F fn) {
    bool failed = false;
    try { fn(); } catch (const std::exception&) { failed = true; }
    assert(failed);
}
struct FakeTexture {
    void** table;
    unsigned width, height, references = 1;
    FakeTexture* hd = nullptr;
};
ULONG WINAPI addRef(FakeTexture* self) { return ++self->references; }
ULONG WINAPI release(FakeTexture* self) { return --self->references; }
HRESULT WINAPI description(FakeTexture* self, UINT level, DWORD* desc) {
    assert(level == 0); desc[0] = 21; desc[6] = self->width; desc[7] = self->height; return S_OK;
}
HRESULT WINAPI privateData(FakeTexture* self, REFGUID, const void* object, DWORD size, DWORD flags) {
    assert(size == sizeof(void*) && flags == 1);
    self->hd = const_cast<FakeTexture*>(static_cast<const FakeTexture*>(object));
    addRef(self->hd);
    return S_OK;
}
void* vtable[19]{};
FakeTexture original{vtable,128,128}, replacement{vtable,512,512};
unsigned uploads = 0;
bool failOriginal = false, failHd = false;
HRESULT WINAPI upload(void*, const void*, UINT, UINT width, UINT height, UINT levels,
    DWORD usage, DWORD format, DWORD pool, DWORD filter, DWORD mipFilter, DWORD color,
    void* info, void* palette, void** output) {
    ++uploads;
    assert(levels == 1 && usage == 0 && format == 21 && filter == 0x80001 && mipFilter == 0xffffffff);
    assert(!color && !info && !palette);
    if (width == 0xffffffff) {
        assert(height == 0xffffffff && pool == 0);
        *output = failOriginal ? nullptr : &original;
        return failOriginal ? E_FAIL : S_OK;
    }
    assert(width == 512 && height == 512 && pool == 1); // Not 288x384: padding must also scale.
    *output = failHd ? nullptr : &replacement;
    return failHd ? E_FAIL : S_OK;
}
HRESULT invoke(const std::vector<uint8_t>& bytes, void** output) {
    return uploadTextureWithReplacement(nullptr, bytes.data(), static_cast<UINT>(bytes.size()),
        0xffffffff,0xffffffff,1,0,21,0,0x80001,0xffffffff,0,nullptr,nullptr,output);
}
int main() {
    const auto root = fs::current_path() / "texture-pack-fixture";
    assert(!fs::exists(root));
    fs::create_directory(root);
    auto source = png(72,96), hd = png(288,384);
    auto record = nlohmann::json{
        {"id", "test/card"}, {"file", "card.png"}, {"source_sha256", sha256Bytes(source.data(), source.size())},
        {"sha256", sha256Bytes(hd.data(), hd.size())}, {"logical_size", {72,96}}, {"physical_size", {288,384}}
    };
    const auto manifest = root / "pack.json";
    auto save = [&](const nlohmann::json& value) {
        std::ofstream out(manifest); out << nlohmann::json{{"version",1},{"textures",nlohmann::json::array({value})}};
    };
    write(root / "card.png", hd); save(record);
    TexturePack pack;
    pack.load(manifest);
    assert(pack.size() == 1);
    auto* match = pack.match(source.data(), source.size());
    assert(match && match->scale == 4 && pack.read(*match) == hd);
    for (unsigned pad = 0; pad <= 15; ++pad) {
        auto aligned = source; aligned.insert(aligned.end(), pad, 0xcd);
        assert(pack.match(aligned.data(), aligned.size()) == match);
    }
    auto wrong = source; wrong[25] ^= 1;
    assert(!pack.match(wrong.data(), wrong.size()));
    assert(!pngInfo(source.data(), 20).bytes);
    wrong = source; wrong[8] = 255;
    assert(!pngInfo(wrong.data(), wrong.size()).bytes);
    for (const auto* path : {"../card.png", "C:/card.png", "card.png:stream", "/card.png"}) {
        auto bad = record; bad["file"] = path; save(bad); rejects([&] { pack.load(manifest); });
        assert(pack.size() == 1); // A failed reload leaves the original index usable.
    }
    for (const auto& sizes : {nlohmann::json{288,383}, nlohmann::json{0,384}, nlohmann::json{8193,8193}, nlohmann::json{288}}) {
        auto bad = record; bad["physical_size"] = sizes; save(bad); rejects([&] { pack.load(manifest); });
    }
    save(record);
    vtable[1] = reinterpret_cast<void*>(addRef); vtable[2] = reinterpret_cast<void*>(release);
    vtable[4] = reinterpret_cast<void*>(privateData); vtable[14] = reinterpret_cast<void*>(description);
    configureTextureReplacements(manifest, upload);
    void* out = nullptr;
    assert(invoke(source, &out) == S_OK && out == &original && original.hd == &replacement);
    assert(uploads == 2 && replacement.references == 1); // Attachment owns the one remaining HD ref.
    assert(original.width == 128 && original.height == 128);
    original.hd = nullptr; release(&replacement);
    assert(replacement.references == 0);
    uploads = 0; failHd = true;
    assert(invoke(source, &out) == S_OK && out == &original && !original.hd && uploads == 2);
    failHd = false; failOriginal = true; uploads = 0;
    assert(FAILED(invoke(source, &out)) && !out && uploads == 1);
    failOriginal = false;
    write(root / "card.png", png(284,384)); uploads = 0;
    assert(invoke(source, &out) == S_OK && out == &original && !original.hd && uploads == 1);
    fs::remove(root / "card.png"); uploads = 0;
    assert(invoke(source, &out) == S_OK && out == &original && uploads == 1);
    fs::remove(manifest); fs::remove(root);
    std::cout << "PASS: source alignment, path/dimension/hash validation, padded upload and original fallback\n";
}
