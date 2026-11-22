# High-level stand-ins for the Win32 imports polcore reaches during start-up, the boot entries and a Game IRC
# open. Sockets go to a Python peer (see run_polcore.py) instead of the network: that is the shape a browser
# build has, with a WebSocket in place of the peer. Each stand-in is (function, stdcall argument count).
import struct

from emu import BLOCK, Stop

WSAEWOULDBLOCK = 10035
FD_READ, FD_WRITE, FD_CONNECT, FD_CLOSE = 1, 2, 16, 32


def install(m, registry=None, files=None, network=None):
    uc = m.uc
    st = dict(
        tls=[0] * 64,
        tls_next=1,
        handles={},
        next=0x1000,
        last_error=0,
        events={},
        sockets={},
        tick=1000,
        files=[],
        keys=[],
        values=[],
        select_timeouts={},
    )
    registry = registry if registry is not None else {}

    def handle(kind, **info):
        h = st["next"]
        st["next"] += 4
        st["handles"][h] = dict(kind=kind, **info)
        return h

    def tls_alloc():
        st["tls_next"] += 1
        return st["tls_next"] - 1

    def tls_set(i, v):
        st["tls"][i] = v
        return 1

    def heap_alloc(h, flags, n):
        return m.alloc(n)

    def heap_realloc(h, flags, p, n):
        q = m.alloc(n)
        if p:
            old = m.blocks.get(p, 0)
            uc.mem_write(q, bytes(uc.mem_read(p, min(old, n))))
        return q

    def get_version_ex(info):
        uc.mem_write(info + 4, struct.pack("<4I", 5, 1, 2600, 2))
        return 1

    def module_file_name(module, buf, n):
        path = b"C:\\PlayOnline\\SquareEnix\\PlayOnlineViewer\\polcore.dll"
        uc.mem_write(buf, path + b"\0")
        return len(path)

    def get_module_handle(name):
        return m.base if name == 0 else 0x7A000000

    def startup_info(p):
        uc.mem_write(p, struct.pack("<I", 68) + bytes(64))
        return 0

    def env_strings():
        return m.string("\0")

    def tick():
        st["tick"] += 16
        return st["tick"]

    def system_time(p):
        uc.mem_write(p, struct.pack("<8H", 2004, 7, 3, 14, 12, 0, 0, 0))
        return 0

    def filetime(p):
        st["tick"] += 1
        uc.mem_write(p, struct.pack("<Q", 127332000000000000 + st["tick"] * 10000))
        return 0

    def perf(p):
        uc.mem_write(p, struct.pack("<Q", st["tick"] * 1000))
        return 1

    def cpinfo(cp, p):
        uc.mem_write(p, struct.pack("<I", 1) + b"?\0" + bytes(12))
        return 1

    def mb2wc(cp, flags, s, n, out, outn):
        text = (m.cstr(s, 4096) + "\0") if n == 0xFFFFFFFF else bytes(uc.mem_read(s, n)).decode("latin1")
        if outn:
            uc.mem_write(out, text.encode("utf-16-le")[: 2 * outn])
        return len(text)

    def wc2mb(cp, flags, s, n, out, outn, d, u):
        if n == 0xFFFFFFFF:
            n = 0
            while uc.mem_read(s + 2 * n, 2) != b"\0\0":
                n += 1
            n += 1
        text = bytes(uc.mem_read(s, 2 * n)).decode("utf-16-le").encode("latin1", "replace")
        if outn:
            uc.mem_write(out, text[:outn])
        return len(text)

    def string_type(*a):
        return 1

    def lcmap(lcid, flags, src, n, dst, dn):
        if n == 0xFFFFFFFF:
            n = len(m.cstr(src, 4096)) + 1
        if dn:
            uc.mem_write(dst, bytes(uc.mem_read(src, min(n, dn))))
        return n

    def virtual_alloc(address, size, kind, protect):
        size = (size + 0xFFFF) & ~0xFFFF
        if address:
            return address
        return m.alloc(size)

    def last_error():
        return st["last_error"]

    def set_last_error(e):
        st["last_error"] = e
        return 0

    def current_dir(n, buf):
        path = b"C:\\PlayOnline\\SquareEnix\\TetraMaster"
        uc.mem_write(buf, path + b"\0")
        return len(path)

    # Files: report every path; serve only what the caller supplies.
    def create_file(name, access, share, sec, disposition, flags, template):
        path = m.cstr(name)
        st["files"].append((path, hex(access), disposition))
        data = files(path) if files else None
        if data is None:
            st["last_error"] = 2
            return 0xFFFFFFFF
        return handle("file", data=bytearray(data), pos=0, path=path)

    def read_file(h, buf, n, done, ov):
        f = st["handles"][h]
        chunk = bytes(f["data"][f["pos"] : f["pos"] + n])
        f["pos"] += len(chunk)
        uc.mem_write(buf, chunk)
        if done:
            m.w32(done, len(chunk))
        return 1

    def write_file(h, buf, n, done, ov):
        if done:
            m.w32(done, n)
        return 1

    def file_size(h, high):
        if high:
            m.w32(high, 0)
        return len(st["handles"][h]["data"])

    def set_file_pointer(h, dist, high, method):
        f = st["handles"][h]
        f["pos"] = (0, f["pos"], len(f["data"]))[method] + struct.unpack("<i", struct.pack("<I", dist))[0]
        return f["pos"]

    # Registry: a dict of 'key\\value' -> bytes or int; every query is logged by the call log.
    def reg_open(root, sub, *rest):
        key = m.cstr(sub) or ""
        st["keys"].append((hex(root), key))
        out = rest[-1]
        m.w32(out, handle("key", path=key))
        return 0

    def reg_query(h, name, reserved, kind, data, size):
        k = st["handles"].get(h, {}).get("path", "") + "\\" + (m.cstr(name) or "")
        value = registry.get(k)
        if value is None:
            return 2
        raw = struct.pack("<I", value) if isinstance(value, int) else value.encode("latin1") + b"\0"
        if kind:
            m.w32(kind, 4 if isinstance(value, int) else 1)
        if data and size:
            uc.mem_write(data, raw[: m.r32(size)])
        if size:
            m.w32(size, len(raw))
        return 0

    # Synchronisation objects: no contention in a cooperative run, so waits on signalled objects return at once.
    def create_event(sec, manual, initial, name):
        return handle("event", signalled=bool(initial), manual=bool(manual))

    def create_semaphore(sec, initial, maximum, name):
        return handle("semaphore", count=initial)

    def create_mutex(sec, owner, name):
        return handle("mutex")

    def set_event(h):
        st["handles"][h]["signalled"] = True
        return 1

    def reset_event(h):
        st["handles"][h]["signalled"] = False
        return 1

    def release_semaphore(h, n, prev):
        st["handles"][h]["count"] += n
        return 1

    def wait(h, ms):
        o = st["handles"].get(h, {})
        if o.get("kind") == "event" and not o["signalled"]:
            return BLOCK if ms else 0x102
        if o.get("kind") == "semaphore":
            if o["count"] == 0:
                return BLOCK if ms else 0x102
            o["count"] -= 1
        if o.get("kind") == "event" and not o["manual"]:
            o["signalled"] = False
        if o.get("kind") == "thread" and m.threads[o["thread"]]["state"] != "exited":
            return BLOCK if ms else 0x102
        return 0

    def create_thread(sec, stack, entry, param, flags, tid):
        t = dict(
            id=len(m.threads) + 1, entry=entry, param=param, context=None, state="suspended" if flags & 4 else "new"
        )
        m.threads.append(t)
        if tid:
            m.w32(tid, t["id"])
        return handle("thread", thread=len(m.threads) - 1)

    def resume_thread(h):
        t = m.threads[st["handles"][h]["thread"]]
        if t["state"] == "suspended":
            t["state"] = "new"
        return 1

    def exit_thread(code):
        raise Stop("exited")

    def sleep(ms):
        st["tick"] += ms
        return BLOCK if m.current else 0

    # Winsock: the peer object supplies connect/send/recv; readiness is reported through WSAEventSelect events.
    def wsa_startup(version, data):
        uc.mem_write(data, struct.pack("<HH", 0x202, 0x202) + bytes(390))
        return 0

    def socket_(af, kind, proto):
        return handle("socket", af=af, type=kind, proto=proto, event=None, mask=0, peer=None, pending=0)

    def gethostbyname(name):
        host = m.cstr(name)
        if network is None or not network.resolve(host):
            return 0
        h = m.alloc(16 + 4 + 8)
        addr = h + 16
        uc.mem_write(addr, bytes([127, 0, 0, 1]))
        m.w32(h + 20, addr)
        m.w32(h + 24, 0)
        uc.mem_write(h, struct.pack("<IIHHI", m.string(host), h + 28, 2, 4, h + 20))
        return h

    def connect(s, addr, n):
        sock = st["handles"][s]
        port = struct.unpack(">H", bytes(uc.mem_read(addr + 2, 2)))[0]
        ip = ".".join(str(b) for b in uc.mem_read(addr + 4, 4))
        sock["peer"] = network.connect(ip, port) if network else None
        if sock["peer"] is None:
            st["last_error"] = 10061
            return 0xFFFFFFFF
        sock["pending"] |= FD_CONNECT | FD_WRITE
        st["last_error"] = WSAEWOULDBLOCK
        return 0xFFFFFFFF  # non-blocking: completion arrives as FD_CONNECT

    def send(s, buf, n, flags):
        st["handles"][s]["peer"].send(bytes(uc.mem_read(buf, n)))
        return n

    def recv(s, buf, n, flags):
        data = st["handles"][s]["peer"].recv(n)
        if not data:
            st["last_error"] = WSAEWOULDBLOCK
            return 0xFFFFFFFF
        uc.mem_write(buf, data)
        return len(data)

    def fd_set(p):
        n = m.r32(p) if p else 0
        return [m.r32(p + 4 + 4 * i) for i in range(n)]

    def put_fd_set(p, fds):
        if p:
            uc.mem_write(p, struct.pack("<I%dI" % len(fds), len(fds), *fds))

    def select_(n, rd, wr, ex, timeout):
        wait = None if not timeout else struct.unpack("<II", bytes(uc.mem_read(timeout, 8)))
        st["select_timeouts"][str(wait)] = st["select_timeouts"].get(str(wait), 0) + 1
        r = [s for s in fd_set(rd) if st["handles"][s]["peer"] and st["handles"][s]["peer"].readable()]
        w = [s for s in fd_set(wr) if st["handles"][s]["peer"]]
        put_fd_set(rd, r)
        put_fd_set(wr, w)
        put_fd_set(ex, [])
        return len(r) + len(w)

    def is_set(s, p):
        return int(s in fd_set(p))

    def event_select(s, ev, mask):
        st["handles"][s].update(event=ev, mask=mask)
        return 0

    def ready(sock):
        bits = sock["pending"]
        if sock["peer"] is not None and sock["peer"].readable():
            bits |= FD_READ
        return bits & sock["mask"]

    def wait_events(count, events, all_, ms, alertable):
        handles = [m.r32(events + 4 * i) for i in range(count)]
        for i, h in enumerate(handles):
            o = st["handles"].get(h, {})
            if o.get("signalled") or any(
                s["event"] == h and ready(s) for s in st["handles"].values() if s["kind"] == "socket"
            ):
                return i
        return BLOCK if ms else 0x102

    def enum_events(s, ev, out):
        sock = st["handles"][s]
        bits = ready(sock)
        sock["pending"] &= ~bits
        uc.mem_write(out, struct.pack("<I", bits) + bytes(40))
        if ev:
            st["handles"][ev]["signalled"] = False
        return 0

    def ws_error():
        return st["last_error"]

    zero = lambda *a: 0
    one = lambda *a: 1
    api = {
        # CRT start-up and plumbing
        "GetVersionExA": (get_version_ex, 1),
        "HeapCreate": (lambda *a: 0x7B000000, 3),
        "HeapAlloc": (heap_alloc, 3),
        "HeapFree": (one, 3),
        "HeapReAlloc": (heap_realloc, 4),
        "HeapSize": (lambda h, f, p: m.blocks.get(p, 0), 3),
        "HeapDestroy": (one, 1),
        "GetProcessHeap": (lambda: 0x7B000000, 0),
        "VirtualAlloc": (virtual_alloc, 4),
        "VirtualFree": (one, 3),
        "TlsAlloc": (tls_alloc, 0),
        "TlsGetValue": (lambda i: st["tls"][i], 1),
        "TlsSetValue": (tls_set, 2),
        "TlsFree": (one, 1),
        "GetCurrentThreadId": (lambda: m.current + 1, 0),
        "GetCurrentProcessId": (lambda: 0x100, 0),
        "GetCurrentProcess": (lambda: 0xFFFFFFFF, 0),
        "GetLastError": (last_error, 0),
        "SetLastError": (set_last_error, 1),
        "InitializeCriticalSection": (zero, 1),
        "EnterCriticalSection": (zero, 1),
        "LeaveCriticalSection": (zero, 1),
        "DeleteCriticalSection": (zero, 1),
        "InterlockedIncrement": (lambda p: (m.w32(p, m.r32(p) + 1), m.r32(p))[1], 1),
        "InterlockedDecrement": (lambda p: (m.w32(p, m.r32(p) - 1), m.r32(p))[1], 1),
        "InterlockedExchange": (lambda p, v: (m.r32(p), m.w32(p, v))[0], 2),
        "GetModuleFileNameA": (module_file_name, 3),
        "GetModuleHandleA": (get_module_handle, 1),
        "DisableThreadLibraryCalls": (one, 1),
        "GetCommandLineA": (lambda: m.string("pol.exe"), 0),
        "GetStartupInfoA": (startup_info, 1),
        "GetStdHandle": (lambda n: 0xFFFFFFFF, 1),
        "GetFileType": (lambda h: 1 if st["handles"].get(h, {}).get("kind") == "file" else 0, 1),
        "SetHandleCount": (lambda n: n, 1),
        "GetEnvironmentStrings": (env_strings, 0),
        "GetEnvironmentStringsW": (env_strings, 0),
        "FreeEnvironmentStringsA": (one, 1),
        "FreeEnvironmentStringsW": (one, 1),
        "GetACP": (lambda: 1252, 0),
        "GetOEMCP": (lambda: 437, 0),
        "GetCPInfo": (cpinfo, 2),
        "IsValidCodePage": (one, 1),
        "MultiByteToWideChar": (mb2wc, 6),
        "WideCharToMultiByte": (wc2mb, 8),
        "GetStringTypeA": (string_type, 5),
        "GetStringTypeW": (string_type, 4),
        "LCMapStringA": (lcmap, 6),
        "LCMapStringW": (lcmap, 6),
        "GetThreadLocale": (lambda: 0x409, 0),
        "GetLocaleInfoA": (zero, 4),
        "GetSystemInfo": (lambda p: uc.mem_write(p + 4, struct.pack("<I", 0x1000)), 1),
        "SetUnhandledExceptionFilter": (zero, 1),
        "IsDebuggerPresent": (zero, 0),
        "GetTickCount": (tick, 0),
        "timeGetTime": (tick, 0),
        "QueryPerformanceCounter": (perf, 1),
        "GetSystemTimeAsFileTime": (filetime, 1),
        "GetTimeZoneInformation": (lambda p: 0, 1),
        "GetCurrentDirectoryA": (current_dir, 2),
        "SetCurrentDirectoryA": (one, 1),
        "SetErrorMode": (zero, 1),
        "OutputDebugStringA": (zero, 1),
        "lstrlenA": (lambda p: len(m.cstr(p, 1 << 16)), 1),
        "CompareStringA": (lambda *a: 2, 6),
        "CompareStringW": (lambda *a: 2, 6),
        "LoadLibraryA": (lambda p: 0, 1),
        "LoadLibraryExA": (lambda p, f, g: 0, 3),
        "FreeLibrary": (one, 1),
        "GetProcAddress": (zero, 2),
        "GetCurrentHwProfileA": (zero, 1),
        "GetWindowsDirectoryA": (lambda b, n: (uc.mem_write(b, b"C:\\WINDOWS\0"), 10)[1], 2),
        # Files and registry
        "CreateFileA": (create_file, 7),
        "ReadFile": (read_file, 5),
        "WriteFile": (write_file, 5),
        "GetFileSize": (file_size, 2),
        "SetFilePointer": (set_file_pointer, 4),
        "CloseHandle": (one, 1),
        "FlushFileBuffers": (one, 1),
        "FindFirstFileA": (lambda *a: 0xFFFFFFFF, 2),
        "FindClose": (one, 1),
        "CreateDirectoryExA": (one, 3),
        "GetDiskFreeSpaceExA": (one, 4),
        "GetDriveTypeA": (lambda p: 3, 1),
        "WritePrivateProfileStringA": (one, 4),
        "RegOpenKeyExA": (reg_open, 5),
        "RegCreateKeyExA": (lambda r, s, a, b, c, d, e, out, disp: reg_open(r, s, out), 9),
        "RegQueryValueExA": (reg_query, 6),
        "RegSetValueExA": (zero, 6),
        "RegCloseKey": (zero, 1),
        # Threads and synchronisation
        "CreateEventA": (create_event, 4),
        "CreateSemaphoreA": (create_semaphore, 4),
        "CreateMutexA": (create_mutex, 3),
        "SetEvent": (set_event, 1),
        "ResetEvent": (reset_event, 1),
        "ReleaseSemaphore": (release_semaphore, 3),
        "ReleaseMutex": (one, 1),
        "WaitForSingleObject": (wait, 2),
        "CreateThread": (create_thread, 6),
        "ResumeThread": (resume_thread, 1),
        "ExitThread": (exit_thread, 1),
        "SetThreadPriority": (one, 2),
        "Sleep": (sleep, 1),
        # Winsock
        "WSAStartup": (wsa_startup, 2),
        "WSACleanup": (zero, 0),
        "socket": (socket_, 3),
        "gethostbyname": (gethostbyname, 1),
        "connect": (connect, 3),
        "send": (send, 4),
        "recv": (recv, 4),
        "closesocket": (zero, 1),
        "shutdown": (zero, 2),
        "ioctlsocket": (zero, 3),
        "setsockopt": (zero, 5),
        "getsockopt": (lambda s, level, o, v, n: (m.w32(v, 0), 0)[1], 5),
        "htons": (lambda v: ((v & 0xFF) << 8) | (v >> 8 & 0xFF), 1),
        "WSAGetLastError": (ws_error, 0),
        "WSACreateEvent": (lambda: create_event(0, 1, 0, 0), 0),
        "WSACloseEvent": (one, 1),
        "WSASetEvent": (set_event, 1),
        "WSAResetEvent": (reset_event, 1),
        "WSAEventSelect": (event_select, 3),
        "WSAWaitForMultipleEvents": (wait_events, 5),
        "WSAEnumNetworkEvents": (enum_events, 3),
        "bind": (zero, 3),
        "select": (select_, 5),
        "__WSAFDIsSet": (is_set, 2),
        "getsockname": (zero, 3),
        "getpeername": (zero, 3),
        # COM and the rest
        "CoCreateGuid": (lambda p: (uc.mem_write(p, bytes(range(16))), 0)[1], 1),
        "LoadRegTypeLib": (lambda *a: 0x80029C4A, 5),
        "LoadTypeLib": (lambda *a: 0x80029C4A, 2),
        "SysAllocString": (lambda p: m.alloc(64), 1),
        "SysFreeString": (zero, 1),
        "StringFromGUID2": (lambda g, b, n: 39, 3),
        "CoTaskMemAlloc": (lambda n: m.alloc(n), 1),
        "CoTaskMemFree": (zero, 1),
    }
    m.api.update(api)
    return st
