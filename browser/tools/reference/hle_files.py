# Read-only file stand-ins for the boot run: the game's paths are served from the local tmloader runtime
# folder, writes are refused. install() returns {name: (fn, argc)} to merge over the other stand-ins.
import fnmatch
import os
import struct

# The user's runtime folder (TM_native.dll, data/), from the environment
FILES = os.environ.get('TM_RUNTIME', '')
INSTALL = 'C:/PlayOnline/SquareEnix/TetraMaster'


def install(ctx):
    uc, cstr, calls = ctx['uc'], ctx['cstr'], ctx['calls']
    st = dict(handle=0x1000, finds={}, open={})

    def w32(a, v):
        uc.mem_write(a, struct.pack('<I', v & 0xFFFFFFFF))

    def handle():
        st['handle'] += 4
        return st['handle']

    def local(path):
        p = path.replace(chr(92), '/')
        if p.lower().startswith(INSTALL.lower()):
            p = p[len(INSTALL):]
        return os.path.join(FILES, p.lstrip('/'))

    def find_data(out, name, full):
        uc.mem_write(out, bytes(320))
        w32(out, 0x10 if os.path.isdir(full) else 0x80)
        w32(out + 32, os.path.getsize(full) if os.path.isfile(full) else 0)
        uc.mem_write(out + 44, name.encode('latin1')[:259] + bytes(1))

    def find_first(pattern, out):
        path = local(cstr(pattern))
        folder, mask = os.path.split(path)
        names = sorted(n for n in os.listdir(folder) if fnmatch.fnmatch(n.lower(), mask.lower())) if os.path.isdir(folder) else []
        calls.append(('FindFirstFileA', cstr(pattern), len(names)))
        if not names:
            return 0xFFFFFFFF
        h = handle()
        st['finds'][h] = (folder, names[1:])
        find_data(out, names[0], os.path.join(folder, names[0]))
        return h

    def find_next(h, out):
        folder, names = st['finds'][h]
        if not names:
            return 0
        st['finds'][h] = (folder, names[1:])
        find_data(out, names[0], os.path.join(folder, names[0]))
        return 1

    def create_file(name, access, share, sec, disposition, flags, template):
        path = cstr(name)
        full = local(path)
        writing = bool(access & 0x40000000) or disposition in (1, 2, 4, 5)
        calls.append(('CreateFileA', path, 'write' if writing else 'read', os.path.isfile(full)))
        if writing or not os.path.isfile(full):
            return 0xFFFFFFFF
        h = handle()
        st['open'][h] = [open(full, 'rb').read(), 0]
        return h

    def read_file(h, buf, n, done, ov):
        f = st['open'][h]
        chunk = f[0][f[1]:f[1] + n]
        f[1] += len(chunk)
        uc.mem_write(buf, chunk)
        if done:
            w32(done, len(chunk))
        return 1

    def seek(h, distance, high, method):
        f = st['open'][h]
        f[1] = (0, f[1], len(f[0]))[method] + struct.unpack('<i', struct.pack('<I', distance))[0]
        return f[1]

    def file_size(h, high):
        if high:
            w32(high, 0)
        return len(st['open'][h][0])

    return {
        'FindFirstFileA': (find_first, 2),
        'FindNextFileA': (find_next, 2),
        'FindClose': (lambda h: 1, 1),
        'CreateFileA': (create_file, 7),
        'ReadFile': (read_file, 5),
        'SetFilePointer': (seek, 4),
        'GetFileSize': (file_size, 2),
        'GetFileType': (lambda h: 1, 1),
        'CloseHandle': (lambda h: 1, 1),
    }
