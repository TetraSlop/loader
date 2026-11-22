// The small modules TM.dll imports: imm32, ole32, oleaut32 and gdi32. Natively, where compat's DirectX does not
// build, placeholders for the media factories: Direct3DCreate8 and DirectInput8Create halt as unhandled imports, the
// end of milestone 2, and DirectSoundCreate8 fails, so the game carries on without sound.
#include "hle/process.h"

#include <format>

namespace tmweb::hle
{
namespace
{

constexpr std::uint32_t kFail      = 0x80004005; // E_FAIL
constexpr std::uint32_t kImmHandle = 0x00006000;

auto ret(const std::uint32_t value = 0) -> Outcome
{
    return Return{ value };
}

} // namespace

void installSmallModules(Process& p)
{
    auto&      boundary = p.machine.boundary();
    auto&      memory   = p.machine.memory();
    const auto add      = [&](const char* module, const char* name, const std::uint8_t argc, Handler handler = {})
    {
        boundary.add(Function{ module, name, Convention::Stdcall, argc, std::move(handler) });
    };
    const auto constant = [&](const char* module, const char* name, const std::uint8_t argc, const std::uint32_t value)
    {
        add(module, name, argc, [value](Call&)
            {
                return ret(value);
            });
    };

    // The IME stays off
    constant("imm32.dll", "ImmDisableIME", 1, 1);
    constant("imm32.dll", "ImmGetContext", 1, kImmHandle);
    constant("imm32.dll", "ImmReleaseContext", 2, 1);
    constant("imm32.dll", "ImmGetOpenStatus", 1, 0);
    constant("imm32.dll", "ImmSetOpenStatus", 2, 1);
    constant("imm32.dll", "ImmNotifyIME", 4, 1);
    // polcore's text entry asks whether the IME is converting: it never is
    add("imm32.dll", "ImmGetConversionStatus", 3, [&memory](Call& call)
        {
            for (const auto index : { 1u, 2u })
            {
                if (call.arg(index))
                {
                    memory.write<std::uint32_t>(call.arg(index), 0);
                }
            }
            return ret(1);
        });

    constant("ole32.dll", "CoInitialize", 1, 0);
    constant("ole32.dll", "CoUninitialize", 0, 0);
    add("ole32.dll", "CoCreateInstance", 5);
    // polcore's Game IRC open: any distinct value serves
    add("ole32.dll", "CoCreateGuid", 1, [&p, &memory](Call& call)
        {
            const auto now = p.machine.scheduler().now();
            memory.write<std::uint64_t>(call.arg(0), 0x544D574542000000ULL ^ now);
            memory.write<std::uint64_t>(call.arg(0) + 8, ++p.guids);
            return ret(0);
        });

    add("winmm.dll", "timeGetTime", 0, [&p](Call&)
        {
            return ret(static_cast<std::uint32_t>(p.machine.scheduler().now() + 1000));
        });

    // BSTRs: a length prefix, then UTF-16 and a terminator; the pointer is past the prefix
    add("oleaut32.dll", "#2", 1, [&p, &memory](Call& call)
        {
            const auto text = call.argWideString(0);
            const auto size = static_cast<std::uint32_t>(2 * text.size());
            const auto bstr = p.allocate(size + 6) + 4;
            memory.write<std::uint32_t>(bstr - 4, size);
            memory.write(bstr, std::span{ reinterpret_cast<const std::uint8_t*>(text.data()), size });
            return ret(call.arg(0) ? bstr : 0);
        });
    add("oleaut32.dll", "#6", 1, [&p](Call& call)
        {
            if (call.arg(0))
            {
                p.free(call.arg(0) - 4);
            }
            return ret();
        });
    add("oleaut32.dll", "#7", 1, [&memory](Call& call)
        {
            return ret(call.arg(0) ? memory.read<std::uint32_t>(call.arg(0) - 4) / 2 : 0);
        });
    constant("oleaut32.dll", "#161", 2, kFail); // LoadTypeLib
    constant("oleaut32.dll", "#162", 5, kFail); // LoadRegTypeLib
    constant("oleaut32.dll", "#163", 3, kFail); // RegisterTypeLib

    add("gdi32.dll", "GetStockObject", 1, [](Call& call)
        {
            return ret(0x00007000 + call.arg(0));
        });

#ifndef __EMSCRIPTEN__
    // The Wasm build has com/directx.cpp's, which app/boot.cpp registers
    add("d3d8.dll", "Direct3DCreate8", 1);
    add("dinput8.dll", "DirectInput8Create", 5);
    add("dsound.dll", "#11", 3, [&p](Call&)
        {
            p.trace.push_back("DirectSoundCreate8");
            return ret(kFail);
        });
#endif
}

} // namespace tmweb::hle
