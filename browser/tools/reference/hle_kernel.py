# High-level stand-ins for the Win32 calls TM.dll's protector makes while it unpacks. Each takes the
# emulator context and the call's stdcall arguments; install() returns {name: (fn, argc)}.
import struct


def install(ctx):
    uc, cstr, calls, heap = ctx['uc'], ctx['cstr'], ctx['calls'], ctx['heap']
    module_path = ctx['module_path']
    file_bytes = ctx['file_bytes']
    st = dict(tls=[0] * 64, tls_next=1, files={}, next_handle=0x100, local_top=0x30000000)
    uc.mem_map(0x30000000, 0x1000000)

    def w32(address, value):
        uc.mem_write(address, struct.pack('<I', value))

    def local_alloc(flags, size):
        address = st['local_top']
        st['local_top'] += (size + 15) & ~15
        uc.mem_write(address, bytes(size))
        return address

    def tls_alloc():
        i = st['tls_next']
        st['tls_next'] += 1
        return i

    def tls_set(i, v):
        st['tls'][i] = v
        return 1

    def get_version_ex(info):
        uc.mem_write(info + 4, struct.pack('<4I', 5, 1, 2600, 2))  # Windows XP SP-less
        return 1

    def get_module_file_name(module, buf, n):
        uc.mem_write(buf, module_path + b'\0')
        return len(module_path)

    def system_time(buf):
        uc.mem_write(buf, struct.pack('<8H', 2004, 7, 3, 14, 12, 0, 0, 0))
        return 0

    def mb2wc(cp, flags, s, n, out, outn):
        if n == 0xFFFFFFFF:
            text = cstr(s).encode('latin1') + b'\0'
        else:
            text = bytes(uc.mem_read(s, n))
        if outn:
            uc.mem_write(out, text.decode('latin1').encode('utf-16-le'))
        return len(text)

    def wc2mb(cp, flags, s, n, out, outn, d, u):
        if n == 0xFFFFFFFF:
            n = 0
            while uc.mem_read(s + 2 * n, 2) != b'\0\0':
                n += 1
            n += 1
        text = bytes(uc.mem_read(s, 2 * n)).decode('utf-16-le').encode('latin1', 'replace')
        if outn:
            uc.mem_write(out, text)
        return len(text)

    def new_handle(value):
        h = st['next_handle']
        st['next_handle'] += 4
        st['files'][h] = value
        return h

    def create_file(name, access, share, sec, disposition, flags, template):
        path = cstr(name)
        calls.append(('CreateFileA', path, hex(access), disposition))
        if path.encode('latin1').lower() != module_path.lower():
            return 0xFFFFFFFF
        return new_handle([file_bytes, 0])

    def read_file(h, buf, n, read, ov):
        f = st['files'][h]
        chunk = f[0][f[1]:f[1] + n]
        f[1] += len(chunk)
        uc.mem_write(buf, chunk)
        if read:
            w32(read, len(chunk))
        return 1

    def set_file_pointer(h, distance, high, method):
        f = st['files'][h]
        f[1] = (0, f[1], len(f[0]))[method] + struct.unpack('<i', struct.pack('<I', distance))[0]
        return f[1]

    def get_file_size(h, high):
        if high:
            w32(high, 0)
        return len(st['files'][h][0])

    def create_mapping(h, sec, protect, high, low, name):
        calls.append(('CreateFileMappingA', hex(h)))
        return new_handle(st['files'][h])

    def map_view(m, access, high, low, n):
        data = st['files'][m][0]
        address = heap(len(data))
        uc.mem_write(address, data)
        calls.append(('MapViewOfFile', hex(address)))
        return address

    def stop(message):
        def fn(*a):
            print(message, [hex(x) for x in a])
            if message == 'MessageBoxA':
                print('   ', cstr(a[2]) if a[2] else '', '|', cstr(a[1]) if a[1] else '')
            uc.emu_stop()
            return 0
        return fn

    def reg(name, result):
        def fn(*a):
            sub = (cstr(a[1]) if 0x10000 < a[1] < 0x80000000 else hex(a[1])) if len(a) > 1 else ""
            calls.append((name, hex(a[0]), sub))
            if name == 'RegCreateKeyExA':
                w32(a[7], 0x80001000)
            return result
        return fn

    return {
        'TlsAlloc': (tls_alloc, 0),
        'TlsGetValue': (lambda i: st['tls'][i], 1),
        'TlsSetValue': (tls_set, 2),
        'TlsFree': (lambda i: 1, 1),
        'InitializeCriticalSection': (lambda c: 0, 1),
        'EnterCriticalSection': (lambda c: 0, 1),
        'LeaveCriticalSection': (lambda c: 0, 1),
        'DeleteCriticalSection': (lambda c: 0, 1),
        'LocalAlloc': (local_alloc, 2),
        'LocalFree': (lambda p: 0, 1),
        'GetLastError': (lambda: 0, 0),
        'GetVersionExA': (get_version_ex, 1),
        'GetSystemDefaultLCID': (lambda: 0x411, 0),
        'GetModuleFileNameA': (get_module_file_name, 3),
        'GetLocalTime': (system_time, 1),
        'GetSystemTime': (system_time, 1),
        'MultiByteToWideChar': (mb2wc, 6),
        'WideCharToMultiByte': (wc2mb, 8),
        'GetVolumeInformationA': (lambda *a: (w32(a[4], 0x1234ABCD) if a[4] else None, 1)[1], 8),
        'GetDiskFreeSpaceA': (lambda *a: 1, 5),
        'GetLocaleInfoA': (lambda *a: 0, 4),
        'CreateFileA': (create_file, 7),
        'ReadFile': (read_file, 5),
        'SetFilePointer': (set_file_pointer, 4),
        'GetFileSize': (get_file_size, 2),
        'CreateFileMappingA': (create_mapping, 6),
        'MapViewOfFile': (map_view, 5),
        'UnmapViewOfFile': (lambda p: 1, 1),
        'CloseHandle': (lambda h: 1, 1),
        'GetStdHandle': (lambda n: 0xFFFFFFFF, 1),
        'GetFileType': (lambda h: 0, 1),
        'IsDebuggerPresent': (lambda: 0, 0),
        'GetTickCount': (lambda: 1000, 0),
        'Sleep': (lambda n: 0, 1),
        'ExitProcess': (stop('ExitProcess'), 1),
        'RaiseException': (stop('RaiseException'), 4),
        'MessageBoxA': (stop('MessageBoxA'), 4),
        'EnumWindows': (lambda f, l: 1, 2),
        'FindWindowExA': (lambda *a: 0, 4),
        'FindWindowA': (lambda *a: 0, 2),
        'GetClassNameA': (lambda *a: 0, 3),
        'GetWindowTextA': (lambda *a: 0, 3),
        'SendMessageA': (lambda *a: 0, 4),
        'LoadStringA': (lambda *a: 0, 4),
        'RegOpenKeyExA': (reg('RegOpenKeyExA', 2), 5),
        'RegCreateKeyExA': (reg('RegCreateKeyExA', 0), 9),
        'RegQueryValueExA': (reg('RegQueryValueExA', 2), 6),
        'RegSetValueExA': (reg('RegSetValueExA', 0), 6),
        'RegCloseKey': (reg('RegCloseKey', 0), 1),
        'RegFlushKey': (reg('RegFlushKey', 0), 1),
    }
