// user32 for TM.dll and its protector: one window, the class and hook it registers, the cursor, and no other
// desktop windows. Calls that would send messages to a window procedure (CreateWindowExA's WM_CREATE,
// DispatchMessageA, SendMessageA, EnumWindows' callback) send nothing until Machine::callGuest exists.
#include "hle/process.h"

#include <format>

namespace tmweb::hle
{
namespace
{

constexpr std::uint32_t kFirstWindow = 0x00010010;

auto ret(const std::uint32_t value = 0) -> Outcome
{
    return Return{ value };
}

} // namespace

void installUser32(Process& p)
{
    auto&      boundary = p.machine.boundary();
    auto&      memory   = p.machine.memory();
    const auto add      = [&](const char* name, const std::uint8_t argc, Handler handler = {})
    {
        boundary.add(Function{ "user32.dll", name, Convention::Stdcall, argc, std::move(handler) });
    };
    const auto constant = [&](const char* name, const std::uint8_t argc, const std::uint32_t value)
    {
        add(name, argc, [value](Call&)
            {
                return ret(value);
            });
    };
    // A class or window name argument: a string, or an atom below 0x10000
    const auto name = [](Call& call, const unsigned index)
    {
        return call.arg(index) < 0x10000 ? std::format("#{}", call.arg(index)) : call.argString(index);
    };

    add("RegisterClassExA", 1, [&p, &memory](Call& call)
        {
            const auto info  = call.arg(0);
            const auto text  = memory.read<std::uint32_t>(info + 40);
            auto       entry = WindowClass{ text < 0x10000 ? std::format("#{}", text) : memory.readString(text), memory.read<std::uint32_t>(info + 8) };
            p.trace.push_back(std::format("RegisterClassExA {} {:#x}", entry.name, entry.procedure));
            p.classes.push_back(std::move(entry));
            return ret(0xC000 + static_cast<std::uint32_t>(p.classes.size()));
        });
    constant("UnregisterClassA", 2, 1);
    add("CreateWindowExA", 12, [&p, name](Call& call)
        {
            auto window = Window{ kFirstWindow + 4 * static_cast<std::uint32_t>(p.windows.size()), name(call, 1), call.argString(2), call.arg(3), static_cast<std::int32_t>(call.arg(6)), static_cast<std::int32_t>(call.arg(7)) };
            p.trace.push_back(std::format("CreateWindowExA {} \"{}\" {:#x} {}x{}", window.className, window.title, window.style, window.width, window.height));
            p.windows.push_back(std::move(window));
            return ret(p.windows.back().handle);
        });
    constant("DestroyWindow", 1, 1);
    constant("ShowWindow", 2, 0);
    constant("UpdateWindow", 1, 1);
    constant("DefWindowProcA", 4, 0);
    constant("PeekMessageA", 5, 0);
    constant("TranslateMessage", 1, 0);
    constant("DispatchMessageA", 1, 0);
    constant("SendMessageA", 4, 0);
    constant("PostMessageA", 4, 1);
    constant("PostQuitMessage", 1, 0);
    add("MessageBoxA", 4, [&p](Call& call)
        {
            // As tmloader does: log it and answer OK
            p.trace.push_back(std::format("MessageBoxA [{}] {}", call.argString(2), call.argString(1)));
            return ret(1);
        });

    // The desktop: no other windows, so the protector finds no debugger
    constant("EnumWindows", 2, 1);
    constant("FindWindowA", 2, 0);
    constant("FindWindowExA", 4, 0);
    constant("GetClassNameA", 3, 0);
    constant("GetWindowTextA", 3, 0);
    add("GetForegroundWindow", 0, [&p](Call&)
        {
            return ret(p.windows.empty() ? 0 : p.windows.front().handle);
        });
    add("GetWindowThreadProcessId", 2, [&p, &memory](Call& call)
        {
            if (call.arg(1))
            {
                memory.write<std::uint32_t>(call.arg(1), memory.read<std::uint32_t>(p.thread().teb + 0x20));
            }
            return ret(p.machine.scheduler().threads().front()->id);
        });
    constant("AttachThreadInput", 3, 1);
    constant("SetForegroundWindow", 1, 1);
    add("SetActiveWindow", 1, [](Call& call)
        {
            return ret(call.arg(0));
        });
    add("GetSystemMetrics", 1, [](Call& call)
        {
            switch (call.arg(0))
            {
                case 0: // SM_CXSCREEN
                    return ret(1920);
                case 1: // SM_CYSCREEN
                    return ret(1080);
                default:
                    return ret(0);
            }
        });
    constant("SystemParametersInfoA", 4, 1);
    constant("LoadIconA", 2, 0x00003000);
    constant("LoadCursorA", 2, 0x00003004);
    constant("LoadStringA", 4, 0);
    add("CharNextA", 1, [&memory](Call& call)
        {
            const auto text = call.arg(0);
            const auto lead = memory.read<std::uint8_t>(text);
            if (lead == 0)
            {
                return ret(text);
            }
            const auto wide = ((lead >= 0x81 && lead <= 0x9F) || (lead >= 0xE0 && lead <= 0xFC)) && memory.read<std::uint8_t>(text + 1) != 0;
            return ret(text + (wide ? 2 : 1));
        });

    // Hooks, cursor and keyboard
    add("SetWindowsHookExA", 4, [&p](Call& call)
        {
            auto hook = Hook{ 0x00005000 + 4 * static_cast<std::uint32_t>(p.hooks.size()), call.arg(0), call.arg(1) };
            p.trace.push_back(std::format("SetWindowsHookExA {} {:#x}", hook.type, hook.procedure));
            p.hooks.push_back(hook);
            return ret(hook.handle);
        });
    constant("UnhookWindowsHookEx", 1, 1);
    constant("CallNextHookEx", 4, 0);
    add("ShowCursor", 1, [&p](Call& call)
        {
            p.cursorCount += call.arg(0) ? 1 : -1;
            return ret(static_cast<std::uint32_t>(p.cursorCount));
        });
    add("GetCursorPos", 1, [&memory](Call& call)
        {
            memory.write<std::int32_t>(call.arg(0), 320);
            memory.write<std::int32_t>(call.arg(0) + 4, 240);
            return ret(1);
        });
    constant("SetCursorPos", 2, 1);
    constant("ClientToScreen", 2, 1);
    constant("ScreenToClient", 2, 1);
    constant("GetAsyncKeyState", 1, 0);
    constant("keybd_event", 4, 0);
}

} // namespace tmweb::hle
