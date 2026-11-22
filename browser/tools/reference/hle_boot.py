# Stand-ins for what TM.dll calls after unpacking: CRT start-up, window, registry and COM, plus a fake
# PlayOnline core object whose methods and common-function table log every call. Each entry is
# name: (fn, stdcall argc). The point is to count what the boot path needs before its first Direct3D call.
import struct

RUNTIME = b'C:\\PlayOnline\\SquareEnix\\TetraMaster'


def install(ctx):
    uc, cstr, calls, heap, stop = ctx['uc'], ctx['cstr'], ctx['calls'], ctx['heap'], ctx['stop']
    st = dict(blocks={}, top=0x40000000, handle=0x200, windows={}, last_error=0)
    uc.mem_map(0x40000000, 0x4000000)

    def w32(a, v):
        uc.mem_write(a, struct.pack('<I', v & 0xFFFFFFFF))

    def r32(a):
        return struct.unpack('<I', uc.mem_read(a, 4))[0]

    def alloc(n, zero=True):
        a = st['top']
        st['top'] += (max(n, 1) + 15) & ~15
        st['blocks'][a] = n
        if zero:
            uc.mem_write(a, bytes(n))
        return a

    def cstring(text):
        a = alloc(len(text) + 1)
        uc.mem_write(a, text + b'\0')
        return a

    command_line = cstring(b'"' + RUNTIME + b'\\TM.dll"')
    environment = alloc(4)

    def handle():
        st['handle'] += 4
        return st['handle']

    def heap_realloc(h, flags, p, n):
        old = st['blocks'].get(p, 0)
        a = alloc(n)
        uc.mem_write(a, bytes(uc.mem_read(p, min(old, n))))
        return a

    def startup_info(p):
        uc.mem_write(p, bytes(68))
        w32(p, 68)
        return 0

    def cp_info(cp, p):
        uc.mem_write(p, bytes(20))
        w32(p, 2)  # MaxCharSize
        uc.mem_write(p + 4, b'?\0')
        uc.mem_write(p + 6, bytes((0x81, 0x9F, 0xE0, 0xFC)))  # Shift-JIS lead bytes
        return 1

    def interlocked(delta):
        def fn(p):
            v = (r32(p) + delta) & 0xFFFFFFFF
            w32(p, v)
            return v
        return fn

    def exchange(p, v):
        old = r32(p)
        w32(p, v)
        return old

    def note(name, result=0):
        def fn(*a):
            calls.append((name,) + tuple(hex(x) for x in a))
            return result
        return fn

    def register_class(p):
        proc = r32(p + 8)
        calls.append(('RegisterClassExA', cstr(r32(p + 40)) if r32(p + 40) > 0x10000 else hex(r32(p + 40)), 'wndproc %#x' % proc))
        st['wndproc'] = proc
        return 0xC001

    def create_window(ex, cls, title, style, x, y, w, h, parent, menu, inst, param):
        hwnd = 0x10010
        calls.append(('CreateWindowExA', cstr(cls) if cls > 0x10000 else hex(cls), cstr(title) if title else '', hex(style), w, h))
        return hwnd

    def message_box(hwnd, text, caption, kind):
        calls.append(('MessageBoxA', cstr(caption) if caption else '', cstr(text) if text else ''))
        print('MessageBoxA:', cstr(caption) if caption else '', '|', cstr(text) if text else '')
        stop('MessageBoxA')
        return 1

    def reg_open(key, sub, out):
        calls.append(('RegOpenKeyA', hex(key), cstr(sub) if sub else ''))
        w32(out, 0x80002000)
        return 0

    def reg_create(key, sub, r, cls, opts, sam, sec, out, disp):
        calls.append(('RegCreateKeyExA', hex(key), cstr(sub) if sub else ''))
        w32(out, 0x80002000)
        if disp:
            w32(disp, 2)
        return 0

    def reg_query(key, name, r, kind, data, length):
        n = cstr(name) if name else ''
        calls.append(('RegQueryValueExA', n))
        if n in ('SOUND', 'FULLSCREEN'):
            value, t = struct.pack('<I', int(n == 'SOUND')), 4
        elif n == 'LOADDIR':
            value, t = b'\0', 1
        elif n in ('0002', 'Path', 'InstallFolder'):
            value, t = RUNTIME + b'\0', 1
        else:
            return 2
        if kind:
            w32(kind, t)
        if data:
            uc.mem_write(data, value)
        if length:
            w32(length, len(value))
        return 0

    def co_create(clsid, outer, ctxflags, iid, out):
        g = bytes(uc.mem_read(clsid, 16))
        calls.append(('CoCreateInstance', g.hex()))
        print('CoCreateInstance', g.hex())
        stop('CoCreateInstance')
        return 0x80004005

    def create_thread(sec, stack, start, param, flags, tid):
        calls.append(('CreateThread', hex(start), hex(param)))
        return handle()

    def direct3d(version):
        calls.append(('Direct3DCreate8', version))
        print('Direct3DCreate8(%d) reached' % version)
        stop('Direct3DCreate8')
        return 0

    def debug_string(p):
        calls.append(('OutputDebugStringA', cstr(p)))
        return 0

    def set_last_error(v):
        st['last_error'] = v
        return 0

    def cursor(p):
        uc.mem_write(p, struct.pack('<ii', 320, 240))
        return 1

    def show_cursor(show):
        st['cursor'] = st.get('cursor', 0) + (1 if show else -1)
        return st['cursor'] & 0xFFFFFFFF

    def char_next(p):
        return p + 1 if uc.mem_read(p, 1) != b'\0' else p

    def lstrlen(p):
        return len(cstr(p))

    def lstrcpy(d, s):
        uc.mem_write(d, cstr(s).encode('latin1') + b'\0')
        return d

    def lstrcat(d, s):
        uc.mem_write(d + len(cstr(d)), cstr(s).encode('latin1') + b'\0')
        return d

    def lstrlenw(p):
        n = 0
        while uc.mem_read(p + 2 * n, 2) != b'\0\0':
            n += 1
        return n

    return {
        'GetVersion': (lambda: 0x0A280105, 0),
        'HeapCreate': (lambda *a: 0x00500000, 3),
        'HeapDestroy': (lambda h: 1, 1),
        'HeapAlloc': (lambda h, f, n: alloc(n), 3),
        'HeapFree': (lambda h, f, p: 1, 3),
        'HeapReAlloc': (heap_realloc, 4),
        'HeapSize': (lambda h, f, p: st['blocks'].get(p, 0), 3),
        'GetCommandLineA': (lambda: command_line, 0),
        'GetEnvironmentStrings': (lambda: environment, 0),
        'GetEnvironmentStringsW': (lambda: environment, 0),
        'FreeEnvironmentStringsA': (lambda p: 1, 1),
        'FreeEnvironmentStringsW': (lambda p: 1, 1),
        'GetEnvironmentVariableA': (lambda *a: 0, 3),
        'SetEnvironmentVariableA': (lambda *a: 1, 2),
        'GetStartupInfoA': (startup_info, 1),
        'SetHandleCount': (lambda n: n, 1),
        'SetStdHandle': (lambda *a: 1, 2),
        'GetACP': (lambda: 932, 0),
        'GetOEMCP': (lambda: 932, 0),
        'GetCPInfo': (cp_info, 2),
        'IsValidCodePage': (lambda cp: 1, 1),
        'LCMapStringA': (lambda *a: 0, 6),
        'LCMapStringW': (lambda *a: 0, 6),
        'GetStringTypeA': (lambda *a: 0, 5),
        'GetStringTypeW': (lambda *a: 0, 4),
        'CompareStringA': (lambda *a: 2, 6),
        'CompareStringW': (lambda *a: 2, 6),
        'SetUnhandledExceptionFilter': (lambda f: 0, 1),
        'GetCurrentProcess': (lambda: 0xFFFFFFFF, 0),
        'GetCurrentThreadId': (lambda: 1, 0),
        'InterlockedIncrement': (interlocked(1), 1),
        'InterlockedDecrement': (interlocked(-1), 1),
        'InterlockedExchange': (exchange, 2),
        'DisableThreadLibraryCalls': (lambda m: 1, 1),
        'IsProcessorFeaturePresent': (lambda f: 0, 1),
        'SetLastError': (set_last_error, 1),
        'GetLastError': (lambda: st['last_error'], 0),
        'CreateMutexA': (lambda *a: handle(), 3),
        'WaitForSingleObject': (lambda h, t: 0, 2),
        'ReleaseMutex': (lambda h: 1, 1),
        'CreateThread': (create_thread, 6),
        'SetThreadPriority': (lambda h, p: 1, 2),
        'SetThreadExecutionState': (lambda f: 0, 1),
        'OutputDebugStringA': (debug_string, 1),
        'FreeLibrary': (lambda m: 1, 1),
        'lstrlenA': (lstrlen, 1),
        'lstrlenW': (lstrlenw, 1),
        'lstrcpyA': (lstrcpy, 2),
        'lstrcatA': (lstrcat, 2),
        'IsBadReadPtr': (lambda p, n: 0, 2),
        'IsBadWritePtr': (lambda p, n: 0, 2),
        'IsBadCodePtr': (lambda p: 0, 1),
        'GetTimeZoneInformation': (lambda p: (uc.mem_write(p, bytes(172)), 0)[1], 1),
        'FlushFileBuffers': (lambda h: 1, 1),
        'CoInitialize': (note('CoInitialize'), 1),
        'CoUninitialize': (note('CoUninitialize'), 0),
        'CoCreateInstance': (co_create, 5),
        'RegisterClassExA': (register_class, 1),
        'CreateWindowExA': (create_window, 12),
        'ShowWindow': (note('ShowWindow', 1), 2),
        'UpdateWindow': (note('UpdateWindow', 1), 1),
        'GetSystemMetrics': (lambda i: {0: 1920, 1: 1080}.get(i, 0), 1),
        'LoadIconA': (lambda *a: 0x3000, 2),
        'LoadCursorA': (lambda *a: 0x3004, 2),
        'SetWindowsHookExA': (note('SetWindowsHookExA', 0x5000), 4),
        'UnhookWindowsHookEx': (lambda h: 1, 1),
        'CallNextHookEx': (lambda *a: 0, 4),
        'SystemParametersInfoA': (note('SystemParametersInfoA', 1), 4),
        'ShowCursor': (show_cursor, 1),
        'PeekMessageA': (lambda *a: 0, 5),
        'GetForegroundWindow': (lambda: 0x10010, 0),
        'GetWindowThreadProcessId': (lambda *a: 1, 2),
        'AttachThreadInput': (lambda *a: 1, 3),
        'SetForegroundWindow': (lambda h: 1, 1),
        'SetActiveWindow': (lambda h: h, 1),
        'DefWindowProcA': (lambda *a: 0, 4),
        'PostMessageA': (note('PostMessageA', 1), 4),
        'MessageBoxA': (message_box, 4),
        'GetAsyncKeyState': (lambda k: 0, 1),
        'ClientToScreen': (lambda *a: 1, 2),
        'ScreenToClient': (lambda *a: 1, 2),
        'GetCursorPos': (cursor, 1),
        'SetCursorPos': (lambda *a: 1, 2),
        'keybd_event': (lambda *a: 0, 4),
        'DestroyWindow': (note('DestroyWindow', 1), 1),
        'PostQuitMessage': (note('PostQuitMessage'), 1),
        'DispatchMessageA': (lambda m: 0, 1),
        'TranslateMessage': (lambda m: 0, 1),
        'UnregisterClassA': (lambda *a: 1, 2),
        'CharNextA': (char_next, 1),
        'ImmDisableIME': (lambda t: 1, 1),
        'ImmGetContext': (lambda h: 0x6000, 1),
        'ImmReleaseContext': (lambda *a: 1, 2),
        'ImmGetOpenStatus': (lambda h: 0, 1),
        'ImmSetOpenStatus': (lambda *a: 1, 2),
        'ImmNotifyIME': (lambda *a: 1, 4),
        'GetStockObject': (lambda i: 0x7000 + i, 1),
        'RegOpenKeyA': (reg_open, 3),
        'RegCreateKeyExA': (reg_create, 9),
        'RegQueryValueExA': (reg_query, 6),
        'RegCloseKey': (lambda h: 0, 1),
        'Direct3DCreate8': (direct3d, 1),
        'DirectInput8Create': (note('DirectInput8Create', 0x80004005), 5),
        '#11': (note('DirectSoundCreate8', 0x80004005), 3),
        '#2': (lambda p: cstring(b'BSTR'), 1),
        '#6': (lambda p: 0, 1),
        '#7': (lambda p: 0, 1),
        '#161': (note('LoadTypeLib', 0x80004005), 2),
        '#162': (note('LoadRegTypeLib', 0x80004005), 5),
        '#163': (note('RegisterTypeLib', 0x80004005), 3),
    }, alloc
