// user32 for TM.dll and its protector: the game's window and its message queue, the low-level keyboard hook, the
// cursor and the keyboard state, and no other desktop windows. Messages reach the window procedure through
// Machine::callGuest: sent ones at once, posted ones when the game dispatches them. The host's keyboard and mouse
// arrive through Process::input and go through the hooks and into the queue when the window's thread pumps.
#include "hle/process.h"

#include "hle/text.h"

#include <algorithm>
#include <array>
#include <format>

namespace tmweb::hle
{
namespace
{

constexpr std::uint32_t kFirstWindow = 0x00010010;
constexpr std::uint32_t kUseDefault  = 0x80000000;
constexpr std::uint32_t kVisible     = 0x10000000; // WS_VISIBLE
constexpr std::uint32_t kKeyboardLL  = 13;         // WH_KEYBOARD_LL
constexpr std::size_t   kInputLimit  = 1024;       // events kept while the game does not pump

enum Wm : std::uint32_t
{
    kCreate           = 0x0001,
    kDestroy          = 0x0002,
    kMove             = 0x0003,
    kSize             = 0x0005,
    kActivate         = 0x0006,
    kSetFocus         = 0x0007,
    kPaint            = 0x000F,
    kClose            = 0x0010,
    kQuit             = 0x0012,
    kEraseBackground  = 0x0014,
    kShowWindow       = 0x0018,
    kActivateApp      = 0x001C,
    kSetCursor        = 0x0020,
    kMouseActivate    = 0x0021,
    kGetMinMaxInfo    = 0x0024,
    kPosChanging      = 0x0046,
    kPosChanged       = 0x0047,
    kNcCreate         = 0x0081,
    kNcDestroy        = 0x0082,
    kNcCalcSize       = 0x0083,
    kNcHitTest        = 0x0084,
    kNcActivate       = 0x0086,
    kKeyDown          = 0x0100,
    kKeyUp            = 0x0101,
    kChar             = 0x0102,
    kSysKeyDown       = 0x0104,
    kSysKeyUp         = 0x0105,
    kSysChar          = 0x0106,
    kSysCommand       = 0x0112,
    kMouseMove        = 0x0200,
    kLeftButtonDown   = 0x0201,
    kRightButtonDown  = 0x0204,
    kMiddleButtonDown = 0x0207,
    kMouseWheel       = 0x020A,
};

constexpr std::uint32_t kSysClose  = 0xF060; // SC_CLOSE
constexpr std::uint32_t kRemove    = 1;      // PM_REMOVE
constexpr std::uint32_t kHide      = 0;      // SW_HIDE
constexpr std::uint32_t kShowNoAct = 4;      // SW_SHOWNOACTIVATE
constexpr std::uint32_t kShowNA    = 8;      // SW_SHOWNA

auto ret(const std::uint32_t value = 0) -> Outcome
{
    return Return{ value };
}

// DirectInput key codes (scan codes, 0x80 for the extended ones) to virtual keys, for a US keyboard. Left and right
// modifiers are VK_SHIFT, VK_CONTROL and VK_MENU here; the low-level hook sees the sided ones (sided()).
constexpr auto kVirtualKeys = []()
{
    auto       table = std::array<std::uint8_t, 256>{};
    const auto set   = [&table](const unsigned dik, const unsigned vk)
    {
        table[dik] = static_cast<std::uint8_t>(vk);
    };
    set(0x01, 0x1B);
    for (auto digit = 1u; digit <= 9; ++digit)
    {
        set(0x01 + digit, '0' + digit);
    }
    set(0x0B, '0');
    set(0x0C, 0xBD);
    set(0x0D, 0xBB);
    set(0x0E, 0x08);
    set(0x0F, 0x09);
    const auto row = [&set](const unsigned first, const char* letters)
    {
        for (auto i = 0u; letters[i] != 0; ++i)
        {
            set(first + i, static_cast<unsigned>(letters[i]));
        }
    };
    row(0x10, "QWERTYUIOP");
    row(0x1E, "ASDFGHJKL");
    row(0x2C, "ZXCVBNM");
    set(0x1A, 0xDB);
    set(0x1B, 0xDD);
    set(0x1C, 0x0D);
    set(0x1D, 0x11);
    set(0x27, 0xBA);
    set(0x28, 0xDE);
    set(0x29, 0xC0);
    set(0x2A, 0x10);
    set(0x2B, 0xDC);
    set(0x33, 0xBC);
    set(0x34, 0xBE);
    set(0x35, 0xBF);
    set(0x36, 0x10);
    set(0x37, 0x6A);
    set(0x38, 0x12);
    set(0x39, 0x20);
    set(0x3A, 0x14);
    for (auto key = 0u; key < 10; ++key)
    {
        set(0x3B + key, 0x70 + key); // F1-F10
    }
    set(0x45, 0x90);
    set(0x46, 0x91);
    const auto pad = std::array<std::pair<unsigned, unsigned>, 13>{ { { 0x47, 0x67 }, { 0x48, 0x68 }, { 0x49, 0x69 }, { 0x4A, 0x6D }, { 0x4B, 0x64 }, { 0x4C, 0x65 }, { 0x4D, 0x66 }, { 0x4E, 0x6B }, { 0x4F, 0x61 }, { 0x50, 0x62 }, { 0x51, 0x63 }, { 0x52, 0x60 }, { 0x53, 0x6E } } };
    for (const auto& [dik, vk] : pad)
    {
        set(dik, vk);
    }
    set(0x57, 0x7A);
    set(0x58, 0x7B);
    set(0x9C, 0x0D);
    set(0x9D, 0x11);
    set(0xB5, 0x6F);
    set(0xB7, 0x2C);
    set(0xB8, 0x12);
    set(0xC5, 0x13);
    const auto navigation = std::array<std::pair<unsigned, unsigned>, 14>{ { { 0xC7, 0x24 }, { 0xC8, 0x26 }, { 0xC9, 0x21 }, { 0xCB, 0x25 }, { 0xCD, 0x27 }, { 0xCF, 0x23 }, { 0xD0, 0x28 }, { 0xD1, 0x22 }, { 0xD2, 0x2D }, { 0xD3, 0x2E }, { 0xDB, 0x5B }, { 0xDC, 0x5C }, { 0xDD, 0x5D }, { 0x00, 0x00 } } };
    for (const auto& [dik, vk] : navigation)
    {
        set(dik, vk);
    }
    return table;
}();

// The sided virtual key the low-level hook and GetAsyncKeyState(VK_LSHIFT...) use
auto sided(const unsigned dik) -> unsigned
{
    switch (dik)
    {
        case 0x2A:
            return 0xA0;
        case 0x36:
            return 0xA1;
        case 0x1D:
            return 0xA2;
        case 0x9D:
            return 0xA3;
        case 0x38:
            return 0xA4;
        case 0xB8:
            return 0xA5;
        default:
            return kVirtualKeys[dik];
    }
}

auto keyFor(const unsigned vk) -> unsigned
{
    for (auto dik = 1u; dik < 256; ++dik)
    {
        if (kVirtualKeys[dik] == vk || sided(dik) == vk)
        {
            return dik;
        }
    }
    return 0;
}

auto down(const Process& p, const unsigned vk) -> bool
{
    switch (vk)
    {
        case 0x01:
            return p.buttons[0];
        case 0x02:
            return p.buttons[1];
        case 0x04:
            return p.buttons[2];
        default:
            break;
    }
    for (auto dik = 1u; dik < 256; ++dik)
    {
        if (p.keys[dik] && (kVirtualKeys[dik] == vk || sided(dik) == vk))
        {
            return true;
        }
    }
    return false;
}

// The character TranslateMessage makes of a key on a US keyboard, or 0
auto character(const Process& p, const unsigned vk) -> std::uint32_t
{
    const auto shift   = down(p, 0x10);
    const auto control = down(p, 0x11);
    if (vk >= 'A' && vk <= 'Z')
    {
        return control ? vk - 'A' + 1 : shift ? vk
                                              : vk + 32;
    }
    if (vk >= '0' && vk <= '9')
    {
        return shift ? static_cast<std::uint8_t>(")!@#$%^&*("[vk - '0']) : vk;
    }
    if (vk >= 0x60 && vk <= 0x69)
    {
        return '0' + vk - 0x60;
    }
    constexpr auto punctuation = std::array<std::array<std::uint8_t, 3>, 11>{ { { 0xBA, ';', ':' }, { 0xBB, '=', '+' }, { 0xBC, ',', '<' }, { 0xBD, '-', '_' }, { 0xBE, '.', '>' }, { 0xBF, '/', '?' }, { 0xC0, '`', '~' }, { 0xDB, '[', '{' }, { 0xDC, '\\', '|' }, { 0xDD, ']', '}' }, { 0xDE, '\'', '"' } } };
    for (const auto& key : punctuation)
    {
        if (key[0] == vk)
        {
            return shift ? key[2] : key[1];
        }
    }
    switch (vk)
    {
        case 0x08:
        case 0x09:
        case 0x0D:
        case 0x1B:
        case 0x20:
            return vk;
        default:
            return 0;
    }
}

auto words(const std::int32_t low, const std::int32_t high) -> std::uint32_t
{
    return (static_cast<std::uint32_t>(high) << 16) | (static_cast<std::uint32_t>(low) & 0xFFFF);
}

class Desktop final
{
public:
    explicit Desktop(Process& process)
    : p_(process)
    , memory_(process.machine.memory())
    {
    }

    // A message to the window procedure, now, on this thread
    auto send(const std::uint32_t handle, const std::uint32_t message, const std::uint32_t wParam, const std::uint32_t lParam) -> std::uint32_t
    {
        const auto* window = p_.window(handle);
        if (!window || window->procedure == 0)
        {
            return 0;
        }
        const auto args = std::array<std::uint32_t, 4>{ handle, message, wParam, lParam };
        return p_.machine.callGuest(window->procedure, args, Convention::Stdcall);
    }

    void post(const std::uint32_t handle, const std::uint32_t message, const std::uint32_t wParam, const std::uint32_t lParam)
    {
        p_.messages.push_back(Message{ handle, message, wParam, lParam, time(), p_.cursorX, p_.cursorY });
    }

    auto time() const -> std::uint32_t
    {
        return static_cast<std::uint32_t>(p_.machine.scheduler().now() + 1000);
    }

    auto scratch(const std::uint32_t size) -> std::uint32_t
    {
        return p_.allocate(size);
    }

    auto create(Call& call, const std::string& className) -> std::uint32_t
    {
        const auto found = std::ranges::find_if(p_.classes, [&](const WindowClass& entry)
                                                {
                                                    return equalText(entry.name, className) || std::format("#{}", entry.atom) == className;
                                                });
        if (found == p_.classes.end())
        {
            p_.setLastError(1407); // ERROR_CANNOT_FIND_WND_CLASS
            return 0;
        }
        const auto at = [&call](const unsigned index)
        {
            return call.arg(index) == kUseDefault ? 0 : static_cast<std::int32_t>(call.arg(index));
        };
        auto       window = Window{ kFirstWindow + 4 * static_cast<std::uint32_t>(p_.windows.size()), found->procedure, className, call.argString(2), call.arg(3), at(4), at(5), at(6), at(7), p_.thread().id };
        const auto style  = window.style;
        p_.trace.push_back(std::format("CreateWindowExA {} \"{}\" {:#x} {}x{}", window.className, window.title, window.style, window.width, window.height));
        p_.windows.push_back(std::move(window));
        const auto handle = p_.windows.back().handle;

        // CREATESTRUCTA: params, instance, menu, parent, cy, cx, y, x, style, name, class, exStyle
        const auto createStruct = scratch(48);
        const auto fields       = std::array<std::uint32_t, 12>{ call.arg(11), call.arg(10), call.arg(9), call.arg(8), call.arg(7), call.arg(6), call.arg(5), call.arg(4), call.arg(3), call.arg(2), call.arg(1), call.arg(0) };
        for (auto i = 0u; i < fields.size(); ++i)
        {
            memory_.write<std::uint32_t>(createStruct + 4 * i, fields[i]);
        }
        const auto minMax = scratch(40);
        const auto rect   = scratch(16);
        send(handle, kGetMinMaxInfo, 0, minMax);
        const auto created = send(handle, kNcCreate, 0, createStruct) != 0;
        if (created)
        {
            writeRect(rect, *p_.window(handle));
            send(handle, kNcCalcSize, 0, rect);
        }
        if (!created || send(handle, kCreate, 0, createStruct) == 0xFFFFFFFF)
        {
            destroy(handle);
            p_.free(createStruct);
            p_.free(minMax);
            p_.free(rect);
            return 0;
        }
        const auto& made = *p_.window(handle);
        send(handle, kSize, 0, words(made.width, made.height));
        send(handle, kMove, 0, words(made.x, made.y));
        p_.free(createStruct);
        p_.free(minMax);
        p_.free(rect);
        if (style & kVisible)
        {
            p_.window(handle)->visible = false;
            show(handle, true);
        }
        return handle;
    }

    void destroy(const std::uint32_t handle)
    {
        if (!p_.window(handle))
        {
            return;
        }
        send(handle, kDestroy, 0, 0);
        send(handle, kNcDestroy, 0, 0);
        std::erase_if(p_.windows, [handle](const Window& window)
                      {
                          return window.handle == handle;
                      });
        std::erase_if(p_.messages, [handle](const Message& message)
                      {
                          return message.window == handle;
                      });
        if (p_.active == handle)
        {
            p_.active = 0;
        }
    }

    // ShowWindow: returns whether it was visible
    auto show(const std::uint32_t handle, const bool activate) -> bool
    {
        auto* window = p_.window(handle);
        if (!window || window->visible)
        {
            return window && window->visible;
        }
        send(handle, kShowWindow, 1, 0);
        const auto position = scratch(28);
        writePosition(position, *window, 0x43); // SWP_NOSIZE | SWP_NOMOVE | SWP_SHOWWINDOW
        send(handle, kPosChanging, 0, position);
        window          = p_.window(handle);
        window->visible = true;
        window->invalid = true;
        if (activate)
        {
            this->activate(handle);
        }
        send(handle, kPosChanged, 0, position);
        p_.free(position);
        return false;
    }

    void hide(const std::uint32_t handle)
    {
        if (auto* window = p_.window(handle); window && window->visible)
        {
            send(handle, kShowWindow, 0, 0);
            p_.window(handle)->visible = false;
        }
    }

    // The window becomes active and takes the focus; the first activation also activates the application
    void activate(const std::uint32_t handle)
    {
        if (!p_.window(handle) || p_.active == handle)
        {
            return;
        }
        const auto previous = p_.active;
        if (!p_.appActive)
        {
            p_.appActive = true;
            send(handle, kActivateApp, 1, p_.thread().id);
        }
        send(handle, kNcActivate, 1, 0);
        p_.active = handle;
        send(handle, kActivate, 1, previous); // WA_ACTIVE
        send(handle, kSetFocus, previous, 0);
    }

    // What DefWindowProcA does for the messages the game leaves to it
    auto defaultProcedure(const std::uint32_t handle, const std::uint32_t message, const std::uint32_t wParam, const std::uint32_t) -> std::uint32_t
    {
        switch (message)
        {
            case kNcCreate:
            case kNcActivate:
            case kEraseBackground:
            case kMouseActivate: // MA_ACTIVATE
            case kNcHitTest:     // HTCLIENT
                return 1;
            case kPaint:
                if (auto* window = p_.window(handle))
                {
                    window->invalid = false;
                }
                return 0;
            case kClose:
                destroy(handle);
                return 0;
            case kSysCommand:
                if ((wParam & 0xFFF0) == kSysClose)
                {
                    send(handle, kClose, 0, 0);
                }
                return 0;
            default:
                return 0;
        }
    }

    // Host input through the low-level keyboard hooks and into the queue. Hooks run only where guest code may be
    // called: hooks is false for a GetMessageA that woke up, which posts the keys unhooked.
    void pump(const bool hooks)
    {
        while (!p_.pending.empty())
        {
            const auto event = p_.pending.front();
            p_.pending.pop_front();
            deliver(event, hooks);
        }
    }

    // The next message for GetMessageA or PeekMessageA: posted messages and input first, then a paint
    auto next(const std::uint32_t handle, const std::uint32_t first, const std::uint32_t last, const bool remove) -> std::optional<Message>
    {
        const auto wanted = [&](const Message& message)
        {
            const auto window = handle == 0 || message.window == handle || (handle == 0xFFFFFFFF && message.window == 0);
            const auto range  = (first == 0 && last == 0) || (message.message >= first && message.message <= last);
            return window && range;
        };
        if (const auto found = std::ranges::find_if(p_.messages, wanted); found != p_.messages.end())
        {
            const auto message = *found;
            if (remove)
            {
                p_.messages.erase(found);
            }
            return message;
        }
        if (p_.quit && (first == 0 || (kQuit >= first && kQuit <= last)))
        {
            if (remove)
            {
                p_.quit = false;
            }
            return Message{ 0, kQuit, p_.quitCode, 0, time(), p_.cursorX, p_.cursorY };
        }
        for (const auto& window : p_.windows)
        {
            if (window.visible && window.invalid && (handle == 0 || handle == window.handle) && ((first == 0 && last == 0) || (kPaint >= first && kPaint <= last)))
            {
                return Message{ window.handle, kPaint, 0, 0, time(), p_.cursorX, p_.cursorY };
            }
        }
        return std::nullopt;
    }

    void writeMessage(const std::uint32_t address, const Message& message)
    {
        const auto fields = std::array<std::uint32_t, 7>{ message.window, message.message, message.wParam, message.lParam, message.time, static_cast<std::uint32_t>(message.x), static_cast<std::uint32_t>(message.y) };
        for (auto i = 0u; i < fields.size(); ++i)
        {
            memory_.write<std::uint32_t>(address + 4 * i, fields[i]);
        }
    }

    auto readMessage(const std::uint32_t address) const -> Message
    {
        return Message{ memory_.read<std::uint32_t>(address), memory_.read<std::uint32_t>(address + 4), memory_.read<std::uint32_t>(address + 8), memory_.read<std::uint32_t>(address + 12) };
    }

    auto screenOrigin() const -> std::pair<std::int32_t, std::int32_t>
    {
        if (p_.windows.empty())
        {
            return { 0, 0 };
        }
        return { p_.windows.front().x, p_.windows.front().y };
    }

private:
    static auto equalText(const std::string_view a, const std::string_view b) -> bool
    {
        return std::ranges::equal(a, b, [](const char x, const char y)
                                  {
                                      return (x >= 'A' && x <= 'Z' ? x + 32 : x) == (y >= 'A' && y <= 'Z' ? y + 32 : y);
                                  });
    }

    void writeRect(const std::uint32_t address, const Window& window)
    {
        memory_.write<std::int32_t>(address, window.x);
        memory_.write<std::int32_t>(address + 4, window.y);
        memory_.write<std::int32_t>(address + 8, window.x + window.width);
        memory_.write<std::int32_t>(address + 12, window.y + window.height);
    }

    // WINDOWPOS: hwnd, insertAfter, x, y, cx, cy, flags
    void writePosition(const std::uint32_t address, const Window& window, const std::uint32_t flags)
    {
        const auto fields = std::array<std::uint32_t, 7>{ window.handle, 0, static_cast<std::uint32_t>(window.x), static_cast<std::uint32_t>(window.y), static_cast<std::uint32_t>(window.width), static_cast<std::uint32_t>(window.height), flags };
        for (auto i = 0u; i < fields.size(); ++i)
        {
            memory_.write<std::uint32_t>(address + 4 * i, fields[i]);
        }
    }

    void deliver(const InputEvent& event, const bool hooks)
    {
        const auto target = p_.active ? p_.active : p_.windows.empty() ? 0
                                                                       : p_.windows.front().handle;
        const auto mouse  = (p_.buttons[0] ? 0x01u : 0u) | (p_.buttons[1] ? 0x02u : 0u) | (p_.buttons[2] ? 0x10u : 0u) | (down(p_, 0x10) ? 0x04u : 0u) | (down(p_, 0x11) ? 0x08u : 0u);
        switch (event.kind)
        {
            case InputKind::Key:
            {
                const auto dik     = static_cast<unsigned>(event.a) & 0xFF;
                const auto pressed = event.b != 0;
                const auto alt     = p_.keys[0x38] || p_.keys[0xB8];
                const auto system  = (alt && dik != 0x38 && dik != 0xB8) || dik == 0x44 || (!pressed && (dik == 0x38 || dik == 0xB8));
                const auto message = system ? (pressed ? kSysKeyDown : kSysKeyUp) : (pressed ? kKeyDown : kKeyUp);
                if (hooks && runHooks(dik, message, alt, event.injected))
                {
                    return;
                }
                if (target == 0)
                {
                    return;
                }
                const auto lParam = 1u | ((dik & 0x7F) << 16) | (dik >= 0x80 ? 1u << 24 : 0u) | (alt ? 1u << 29 : 0u) | (pressed ? (event.repeat ? 1u << 30 : 0u) : 3u << 30);
                post(target, message, kVirtualKeys[dik], lParam);
                return;
            }
            case InputKind::Move:
                if (target == 0)
                {
                    return;
                }
                // Windows keeps one WM_MOUSEMOVE in the queue: a newer position replaces it
                if (!p_.messages.empty() && p_.messages.back().message == kMouseMove)
                {
                    p_.messages.pop_back();
                }
                post(target, kMouseMove, mouse, words(event.a, event.b));
                return;
            case InputKind::Button:
            {
                if (target == 0 || event.a < 0 || event.a > 2)
                {
                    return;
                }
                constexpr auto downs = std::array<std::uint32_t, 3>{ kLeftButtonDown, kRightButtonDown, kMiddleButtonDown };
                post(target, downs[static_cast<std::size_t>(event.a)] + (event.b ? 0 : 1), mouse, words(p_.cursorX, p_.cursorY));
                return;
            }
            case InputKind::Wheel:
            {
                if (target == 0)
                {
                    return;
                }
                const auto [x, y] = screenOrigin();
                post(target, kMouseWheel, words(static_cast<std::int32_t>(mouse), event.a), words(p_.cursorX + x, p_.cursorY + y));
                return;
            }
        }
    }

    // True when a hook swallowed the key
    auto runHooks(const unsigned dik, const std::uint32_t message, const bool alt, const bool injected) -> bool
    {
        for (auto it = p_.hooks.rbegin(); it != p_.hooks.rend(); ++it)
        {
            if (it->type != kKeyboardLL)
            {
                continue;
            }
            // KBDLLHOOKSTRUCT: vkCode, scanCode, flags, time, dwExtraInfo
            const auto up     = message == kKeyUp || message == kSysKeyUp;
            const auto flags  = (dik >= 0x80 ? 0x01u : 0u) | (injected ? 0x10u : 0u) | (alt ? 0x20u : 0u) | (up ? 0x80u : 0u);
            const auto info   = scratch(20);
            const auto fields = std::array<std::uint32_t, 5>{ sided(dik), dik & 0x7F, flags, time(), 0 };
            for (auto i = 0u; i < fields.size(); ++i)
            {
                memory_.write<std::uint32_t>(info + 4 * i, fields[i]);
            }
            const auto args   = std::array<std::uint32_t, 3>{ 0, message, info }; // HC_ACTION
            const auto result = p_.machine.callGuest(it->procedure, args, Convention::Stdcall);
            p_.free(info);
            return result != 0; // the most recent hook decides; CallNextHookEx goes no further
        }
        return false;
    }

    Process& p_;
    Memory&  memory_;
};

// GetMessageA's wait: a message, input or a paint is there
struct QueueReady final : Waitable
{
    explicit QueueReady(Process& process)
    : p(process)
    {
    }

    auto signaled(ThreadId) const -> bool override
    {
        return !p.messages.empty() || !p.pending.empty() || p.quit || std::ranges::any_of(p.windows, [](const Window& window)
                                                                                          {
                                                                                              return window.visible && window.invalid;
                                                                                          });
    }

    Process& p;
};

} // namespace

void Process::input(const InputKind kind, const std::int32_t a, const std::int32_t b, const bool injected)
{
    auto event = InputEvent{ kind, a, b, false, injected };
    switch (kind)
    {
        case InputKind::Key:
            event.repeat                             = b != 0 && keys[static_cast<std::size_t>(a) & 0xFF];
            keys[static_cast<std::size_t>(a) & 0xFF] = b != 0;
            break;
        case InputKind::Move:
            cursorX = a;
            cursorY = b;
            break;
        case InputKind::Button:
            if (a >= 0 && a < 3)
            {
                buttons[static_cast<std::size_t>(a)] = b != 0;
            }
            break;
        case InputKind::Wheel:
            break;
    }
    if (pending.size() >= kInputLimit)
    {
        pending.pop_front();
    }
    pending.push_back(event);
}

auto Process::window(const std::uint32_t handle) -> Window*
{
    const auto found = std::ranges::find(windows, handle, &Window::handle);
    return found == windows.end() ? nullptr : &*found;
}

void installUser32(Process& p)
{
    auto&      boundary = p.machine.boundary();
    auto&      memory   = p.machine.memory();
    const auto desktop  = std::make_shared<Desktop>(p);
    p.queueReady        = std::make_shared<QueueReady>(p);
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

    // Classes and windows
    add("RegisterClassExA", 1, [&p, &memory](Call& call)
        {
            const auto info  = call.arg(0);
            const auto text  = memory.read<std::uint32_t>(info + 40);
            const auto atom  = 0xC000 + static_cast<std::uint32_t>(p.classes.size()) + 1;
            auto       entry = WindowClass{ text < 0x10000 ? std::format("#{}", text) : memory.readString(text), memory.read<std::uint32_t>(info + 8), atom };
            p.trace.push_back(std::format("RegisterClassExA {} {:#x}", entry.name, entry.procedure));
            p.classes.push_back(std::move(entry));
            return ret(atom);
        });
    constant("UnregisterClassA", 2, 1);
    add("CreateWindowExA", 12, [desktop, name](Call& call)
        {
            return ret(desktop->create(call, name(call, 1)));
        });
    add("DestroyWindow", 1, [desktop](Call& call)
        {
            desktop->destroy(call.arg(0));
            return ret(1);
        });
    add("ShowWindow", 2, [desktop](Call& call)
        {
            const auto command = call.arg(1);
            if (command == kHide)
            {
                desktop->hide(call.arg(0));
                return ret(1);
            }
            return ret(desktop->show(call.arg(0), command != kShowNoAct && command != kShowNA) ? 1 : 0);
        });
    add("UpdateWindow", 1, [&p, desktop](Call& call)
        {
            if (const auto* window = p.window(call.arg(0)); window && window->visible && window->invalid)
            {
                desktop->send(call.arg(0), kPaint, 0, 0);
            }
            return ret(1);
        });
    add("DefWindowProcA", 4, [desktop](Call& call)
        {
            return ret(desktop->defaultProcedure(call.arg(0), call.arg(1), call.arg(2), call.arg(3)));
        });

    // The queue
    add("PeekMessageA", 5, [desktop](Call& call)
        {
            desktop->pump(true);
            const auto message = desktop->next(call.arg(1), call.arg(2), call.arg(3), (call.arg(4) & kRemove) != 0);
            if (!message)
            {
                return ret(0);
            }
            desktop->writeMessage(call.arg(0), *message);
            return ret(1);
        });
    add("GetMessageA", 4, [&p, desktop](Call& call) -> Outcome
        {
            desktop->pump(true);
            const auto out    = call.arg(0);
            const auto filter = std::array<std::uint32_t, 3>{ call.arg(1), call.arg(2), call.arg(3) };
            const auto take   = [desktop, out, filter]() -> std::optional<Return>
            {
                const auto message = desktop->next(filter[0], filter[1], filter[2], true);
                if (!message)
                {
                    return std::nullopt;
                }
                desktop->writeMessage(out, *message);
                return Return{ message->message == kQuit ? 0u : 1u };
            };
            if (const auto result = take())
            {
                return *result;
            }
            // ponytail: what arrives while it waits skips the low-level hooks, which need guest code on this thread
            return Block{ Wait{ { p.queueReady }, false, kInfinite }, [desktop, take](const WaitResult&)
                          {
                              desktop->pump(false);
                              return take().value_or(Return{ 1 });
                          } };
        });
    add("TranslateMessage", 1, [&p, desktop](Call& call)
        {
            const auto message = desktop->readMessage(call.arg(0));
            if (message.message != kKeyDown && message.message != kSysKeyDown)
            {
                return ret(0);
            }
            if (const auto c = character(p, message.wParam))
            {
                // The character comes next, ahead of anything posted after the key
                p.messages.push_front(Message{ message.window, message.message == kKeyDown ? kChar : kSysChar, c, message.lParam, desktop->time(), p.cursorX, p.cursorY });
            }
            return ret(1);
        });
    add("DispatchMessageA", 1, [desktop](Call& call)
        {
            const auto message = desktop->readMessage(call.arg(0));
            return ret(desktop->send(message.window, message.message, message.wParam, message.lParam));
        });
    add("SendMessageA", 4, [desktop](Call& call)
        {
            return ret(desktop->send(call.arg(0), call.arg(1), call.arg(2), call.arg(3)));
        });
    add("PostMessageA", 4, [&p, desktop](Call& call)
        {
            if (call.arg(0) != 0 && !p.window(call.arg(0)))
            {
                p.setLastError(1400); // ERROR_INVALID_WINDOW_HANDLE
                return ret(0);
            }
            desktop->post(call.arg(0), call.arg(1), call.arg(2), call.arg(3));
            return ret(1);
        });
    add("PostQuitMessage", 1, [&p](Call& call)
        {
            p.quit     = true;
            p.quitCode = call.arg(0);
            return ret();
        });
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
            return ret(p.active);
        });
    add("GetWindowThreadProcessId", 2, [&p, &memory](Call& call)
        {
            if (call.arg(1))
            {
                memory.write<std::uint32_t>(call.arg(1), memory.read<std::uint32_t>(p.thread().teb + 0x20));
            }
            const auto* window = p.window(call.arg(0));
            return ret(window ? window->thread : p.machine.scheduler().threads().front()->id);
        });
    constant("AttachThreadInput", 3, 1);
    add("SetForegroundWindow", 1, [&p, desktop](Call& call)
        {
            desktop->activate(call.arg(0));
            return ret(p.window(call.arg(0)) ? 1 : 0);
        });
    add("SetActiveWindow", 1, [&p, desktop](Call& call)
        {
            const auto previous = p.active;
            desktop->activate(call.arg(0));
            return ret(previous);
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
            const auto at   = call.arg(0);
            const auto lead = memory.read<std::uint8_t>(at);
            if (lead == 0)
            {
                return ret(at);
            }
            const auto wide = text::isLeadByte(text::kShiftJis, lead) && memory.read<std::uint8_t>(at + 1) != 0;
            return ret(at + (wide ? 2 : 1));
        });

    // Hooks, cursor and keyboard
    add("SetWindowsHookExA", 4, [&p](Call& call)
        {
            auto hook = Hook{ 0x00005000 + 4 * static_cast<std::uint32_t>(p.hooks.size()), call.arg(0), call.arg(1) };
            p.trace.push_back(std::format("SetWindowsHookExA {} {:#x}", hook.type, hook.procedure));
            p.hooks.push_back(hook);
            return ret(hook.handle);
        });
    add("UnhookWindowsHookEx", 1, [&p](Call& call)
        {
            return ret(std::erase_if(p.hooks, [&call](const Hook& hook)
                                     {
                                         return hook.handle == call.arg(0);
                                     }) != 0);
        });
    constant("CallNextHookEx", 4, 0);
    add("ShowCursor", 1, [&p](Call& call)
        {
            p.cursorCount += call.arg(0) ? 1 : -1;
            return ret(static_cast<std::uint32_t>(p.cursorCount));
        });
    add("GetCursorPos", 1, [&p, &memory, desktop](Call& call)
        {
            const auto [x, y] = desktop->screenOrigin();
            memory.write<std::int32_t>(call.arg(0), p.cursorX + x);
            memory.write<std::int32_t>(call.arg(0) + 4, p.cursorY + y);
            return ret(1);
        });
    // The host's pointer does not move; the guest sees where it put it until the next host move
    add("SetCursorPos", 2, [&p, desktop](Call& call)
        {
            const auto [x, y] = desktop->screenOrigin();
            p.cursorX         = static_cast<std::int32_t>(call.arg(0)) - x;
            p.cursorY         = static_cast<std::int32_t>(call.arg(1)) - y;
            return ret(1);
        });
    const auto shift = [&memory](const std::uint32_t point, const std::int32_t sign, const std::pair<std::int32_t, std::int32_t> origin)
    {
        memory.write<std::int32_t>(point, memory.read<std::int32_t>(point) + sign * origin.first);
        memory.write<std::int32_t>(point + 4, memory.read<std::int32_t>(point + 4) + sign * origin.second);
    };
    add("ClientToScreen", 2, [&p, shift](Call& call)
        {
            const auto* window = p.window(call.arg(0));
            if (window)
            {
                shift(call.arg(1), 1, { window->x, window->y });
            }
            return ret(window ? 1 : 0);
        });
    add("ScreenToClient", 2, [&p, shift](Call& call)
        {
            const auto* window = p.window(call.arg(0));
            if (window)
            {
                shift(call.arg(1), -1, { window->x, window->y });
            }
            return ret(window ? 1 : 0);
        });
    add("GetAsyncKeyState", 1, [&p](Call& call)
        {
            return ret(down(p, call.arg(0) & 0xFF) ? 0x8000 : 0);
        });
    // keybd_event(vk, scan, flags, extra): an injected key, as if typed
    add("keybd_event", 4, [&p](Call& call)
        {
            if (const auto dik = keyFor(call.arg(0) & 0xFF))
            {
                p.input(InputKind::Key, static_cast<std::int32_t>(dik), (call.arg(2) & 2) ? 0 : 1, true); // KEYEVENTF_KEYUP
            }
            return ret();
        });
}

} // namespace tmweb::hle
