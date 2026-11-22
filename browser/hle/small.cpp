// The small modules TM.dll imports: imm32, ole32, oleaut32 and gdi32, and placeholders for the media factories
// until the COM stand-ins land. Direct3DCreate8 and DirectInput8Create halt as unhandled imports; that halt is the
// end of milestone 2. DirectSoundCreate8 fails, and the game carries on without sound.
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

    constant("ole32.dll", "CoInitialize", 1, 0);
    constant("ole32.dll", "CoUninitialize", 0, 0);
    add("ole32.dll", "CoCreateInstance", 5);

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

    add("d3d8.dll", "Direct3DCreate8", 1);
    add("dinput8.dll", "DirectInput8Create", 5);
    add("dsound.dll", "#11", 3, [&p](Call&)
        {
            p.trace.push_back("DirectSoundCreate8");
            return ret(kFail);
        });
}

} // namespace tmweb::hle
