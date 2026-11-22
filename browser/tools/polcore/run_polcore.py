# Run the user's unmodified polcore_native.dll under Unicorn at the browser build's Viewer base (0x10400000),
# relocated by our loader and unpacked by its own POL1 stub, then drive it the way tmloader and TM.dll do:
# DllMain, the COM core, tmloader's Viewer set-up, the TM boot entries and, with --irc HOST:GAME:PROFILE, a
# Game IRC open and the profile bootstrap against a running tmserver. Sockets are bridged to real TCP
# sockets on loopback, which is the role a WebSocket plays in the browser.
# Prints a JSON report of every import call per phase; it contains no polcore bytes.
# Usage: POLCORE_DLL=... python run_polcore.py out/run.json [--irc 127.0.0.1:51241:51220]
import collections
import json
import os
import select
import socket
import struct
import sys

import common
import emu
import hle
from unicorn import UC_HOOK_BLOCK, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_ESP

VIEWER_BASE = 0x10400000
CALLBACKS = emu.SENTINEL + 0x100  # cdecl 'ret' stubs standing in for TM's callbacks, logged
CORE_CLSID = bytes.fromhex("ddf501359478df42866aa2b6527d8049")  # {3501F5DD-7894-42DF-866A-A2B6527D8049}
IID_CLASSFACTORY = bytes.fromhex("0100000000000000c000000000000046")
IID_CORE = bytes.fromhex("546651e077ef5d43aa7d50d2c069ce34")  # {E0516654-EF77-435D-AA7D-50D2C069CE34}


class Network:
    """Loopback TCP standing in for the tunnel: every pol.com name resolves, ports map to tmserver's."""

    def __init__(self, host, game, profile):
        self.host, self.ports = host, {51241: game, 51220: profile}
        self.log = []

    def resolve(self, name):
        self.log.append(("resolve", name))
        return True

    def connect(self, ip, port):
        target = self.ports.get(port, port)
        self.log.append(("connect", ip, port, target))
        s = socket.create_connection((self.host, target), timeout=5)
        s.setblocking(False)
        return Peer(s, self.log, port)


class Peer:
    def __init__(self, s, log, port):
        self.s, self.log, self.port, self.buffer = s, log, port, b""

    def readable(self):
        if not self.buffer:
            r, _, _ = select.select([self.s], [], [], 0.05)
            if r:
                self.buffer += self.s.recv(65536)
        return bool(self.buffer)

    def send(self, data):
        self.log.append(("send", self.port, len(data)))
        self.s.sendall(data)

    def recv(self, n):
        self.readable()
        data, self.buffer = self.buffer[:n], self.buffer[n:]
        if data:
            self.log.append(("recv", self.port, len(data)))
        return data


def main():
    out = sys.argv[1]
    irc = sys.argv[sys.argv.index("--irc") + 1].split(":") if "--irc" in sys.argv else None
    m = emu.Machine(common.need("POLCORE_DLL"), VIEWER_BASE)
    network = Network(irc[0], int(irc[1]), int(irc[2])) if irc else None
    registry = {"SOFTWARE\\PlayOnlineUS\\InstallFolder\\0001": "C:\\PlayOnline\\SquareEnix\\PlayOnlineViewer"}
    hst = hle.install(m, registry=registry, network=network, files=runtime_file)
    delta = VIEWER_BASE - common.BASE
    report = dict(base=hex(VIEWER_BASE), phases=[])
    callback_log = []
    m.uc.mem_write(CALLBACKS, b"\xc3" * 0x100)

    def on_callback(uc, address, size, _):
        esp = uc.reg_read(UC_X86_REG_ESP)
        callback_log.append(
            dict(
                callback=(address - CALLBACKS) // 16,
                args=[hex(m.r32(esp + 4 + 4 * i)) for i in range(4)],
                thread=m.current,
            )
        )

    m.uc.hook_add(UC_HOOK_CODE, on_callback, begin=CALLBACKS, end=CALLBACKS + 0xFF)

    def phase(name, fn):
        start = len(m.calls)
        m.executed = 0
        result, error = None, None
        try:
            result = fn()
        except emu.Stop as e:
            error = str(e)
        calls = m.calls[start:]
        used = collections.Counter(c[1] for c in calls)
        report["phases"].append(
            dict(
                name=name,
                result=None if result is None else hex(result),
                error=error,
                million_instructions=getattr(m, "executed", 0) // 1_000_000,
                imports=dict(sorted(used.items())),
                callers=sorted({hex(c[3] - delta) for c in calls})[:40],
            )
        )
        print(
            "%-28s %-12s %5dM %s %s"
            % (
                name,
                "" if result is None else hex(result),
                m.executed // 1_000_000,
                error or "",
                " ".join("%s:%d" % kv for kv in sorted(used.items()))[:200],
            )
        )
        return result

    # 1. Loader + POL1 stub, then check its unpack and relocation against our own.
    entry = VIEWER_BASE + m.pe.OPTIONAL_HEADER.AddressOfEntryPoint
    phase("DllMain(PROCESS_ATTACH)", lambda: m.call(entry, VIEWER_BASE, 1, 0))
    _, reference = common.polcore_image()
    text = bytes(m.uc.mem_read(VIEWER_BASE + 0x1000, 0x64000))
    ref = bytearray(reference[0x1000:0x65000])
    for a in relocations(m):  # apply our delta to the reference to compare
        if 0x1000 <= a < 0x65000 - 3:
            v = struct.unpack_from("<I", ref, a - 0x1000)[0]
            struct.pack_into("<I", ref, a - 0x1000, (v + delta) & 0xFFFFFFFF)
    report["text_matches_relocated_reference"] = text == bytes(ref)
    report["text_differing_bytes"] = sum(1 for a, b in zip(text, ref) if a != b)
    report["text_first_difference"] = next((hex(0x1000 + k) for k, (a, b) in enumerate(zip(text, ref)) if a != b), None)
    print(
        "unpacked .text matches the relocated reference:",
        report["text_matches_relocated_reference"],
        report["text_differing_bytes"],
        report["text_first_difference"],
    )

    # 2. COM: class factory, core object, the slots tmloader calls.
    scratch = m.alloc(0x100)
    m.uc.mem_write(scratch, CORE_CLSID + IID_CLASSFACTORY + IID_CORE)
    get_class = VIEWER_BASE + next(
        e.address for e in m.pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name == b"DllGetClassObject"
    )
    phase("DllGetClassObject", lambda: m.call(get_class, scratch, scratch + 16, scratch + 48))
    factory = m.r32(scratch + 48)
    hot = collections.Counter()
    hook = (
        m.uc.hook_add(UC_HOOK_BLOCK, lambda uc, a, size, _: hot.update({a: size}))
        if "--profile" in sys.argv
        else None
    )
    phase(
        "IClassFactory::CreateInstance",
        lambda: m.call(m.r32(m.r32(factory) + 12), factory, 0, scratch + 32, scratch + 52),
    )
    if hook:
        m.uc.hook_del(hook)
        total = sum(hot.values())
        report["create_instance_hot_blocks"] = [
            dict(rva=hex(a - VIEWER_BASE), share=round(n / total, 3)) for a, n in hot.most_common(12)
        ]
        print("hot blocks (share of executed code bytes):", report["create_instance_hot_blocks"])
    core = m.r32(scratch + 52)
    vtbl = m.r32(core)
    report["core_vtable"] = [hex(m.r32(vtbl + 4 * i) - delta) for i in range(40)]
    slot = lambda n, *a: m.call(m.r32(vtbl + 4 * n), core, *a)
    phase("core 27 SetAreaCode(1)", lambda: slot(27, 1))
    cmdline = m.string(" /game DdcMLbB")
    phase("core 5 SetParamInit", lambda: slot(5, 0x00400000, cmdline))
    phase("core 7 GetCommonTable", lambda: slot(7, scratch + 56))
    table = m.r32(scratch + 56)
    report["table_rva"] = hex(table - VIEWER_BASE)
    before = struct.unpack("<1200I", m.uc.mem_read(table, 4800))
    entry_of = lambda i: m.r32(table + 4 * i)
    t = lambda i, *a: m.call(entry_of(i), *a)

    # 3. tmloader's Viewer set-up (src/app/loader.cpp, src/platform/network.cpp).
    phase("965 SetRegistryLanguage", lambda: t(965, 1))
    phase("420 SetUiLanguage", lambda: t(420, 1))
    phase("367 SetRegistryRoot", lambda: t(367, m.string("SOFTWARE\\PlayOnlineUS")))
    phase("125 SetInstallDirectory", lambda: t(125, m.string("C:\\PlayOnline\\SquareEnix\\TetraMaster")))
    phase("932 FileLayerStatus", lambda: t(932))
    account = m.alloc(16)
    phase("854 EncodeAccount", lambda: t(854, account, m.string("EEEEEEEF")))
    phase("851 SetAccount", lambda: t(851, account))
    phase("+FE20 InitializeInet", lambda: m.call(VIEWER_BASE + 0xFE20))
    endpoint = m.alloc(20)
    phase(
        "+127A0 ResolveAddress",
        lambda: m.call(VIEWER_BASE + 0x127A0, m.string(irc[0] if irc else "127.0.0.1"), endpoint),
    )
    m.uc.mem_write(endpoint + 2, struct.pack("<H", int(irc[2]) if irc else 51220))
    login = (m.string("EEEEEEEF"), m.string("tm-local-test"))
    if not m.r32(VIEWER_BASE + 0xAFBD8):
        phase("+1EB00 InitializeProfile", lambda: m.call(VIEWER_BASE + 0x1EB00, endpoint, *login))
    else:
        phase("+1EA60 SetProfileLogin", lambda: m.call(VIEWER_BASE + 0x1EA60, *login))
    phase("+1E930 SetProfileEndpoint", lambda: m.call(VIEWER_BASE + 0x1E930, endpoint))
    m.uc.mem_write(VIEWER_BASE + 0x404AB8, bytes(m.uc.mem_read(endpoint, 20)))
    phase("+19BA0", lambda: m.call(VIEWER_BASE + 0x19BA0))

    # 4. The entries TM's boot calls before Direct3DCreate8 (the spike's tm_boot.json), with TM-shaped arguments.
    cb = [CALLBACKS + 16 * i for i in range(8)]
    phase("824 RegisterCallbacks", lambda: t(824, cb[0], cb[1], cb[2]))
    phase("966", lambda: t(966))
    phase("288", lambda: t(288))
    ticket = m.alloc(0x100)
    phase("470 option request", lambda: t(470, ticket))
    poll(phase, m, "471 option check", lambda: t(471, ticket))
    mail = m.alloc(0x200)
    phase("472 read option", lambda: t(472, ticket + 0x10, m.string("HARASSMENT_MAILADDRESS")))
    phase("473 read option value", lambda: t(473, m.string("HARASSMENT_MAILADDRESS"), ticket + 0x10, mail, 0x100))
    phase("907 LoadOption", lambda: t(907))
    poll(phase, m, "908 LoadOptionCheck", lambda: t(908))
    phase("126", lambda: t(126, 0x13, m.alloc(0x200), 0x200))
    phase("362", lambda: t(362, 0, m.alloc(0x40), 0xFFFFFFFF))

    # 5. Game IRC open and the profile bootstrap, as tmloader's hooks do them.
    if irc:
        callbacks = m.alloc(0x40)
        m.uc.mem_write(callbacks, struct.pack("<7I", cb[3], cb[3], cb[4], cb[4], 0xFFFFFFFF, 0, 1))
        cipher, queue = m.alloc(0x1000), m.alloc(0x1000)
        slot_ = phase(
            "132 GameIrcOpen",
            lambda: t(
                132,
                m.string(irc[0]),
                m.string("EEEEEEEF"),
                m.string("tm-local-test"),
                m.string("46.49"),
                8,
                callbacks,
                cipher,
                queue,
                0x100,
            ),
        )
        report["irc_open_check"] = poll(
            phase, m, "133 GameIrcOpenCheck + 153 pump", lambda: t(133, slot_) or t(153, slot_) and 0, tries=2000
        )
        # tmloader's seedProfile: copy the Game IRC cipher context and its matrix, then seed the profile cipher.
        irc_slot = VIEWER_BASE + 0x3E58A0 + slot_ * 0x3A00
        profile_cipher, profile_matrix = m.alloc(0x68), m.alloc(0x1000)
        m.uc.mem_write(profile_cipher, bytes(m.uc.mem_read(irc_slot + 0x2C0, 0x68)))
        m.uc.mem_write(profile_matrix, bytes(m.uc.mem_read(m.r32(profile_cipher + 0x48), 0x1000)))
        m.w32(profile_cipher + 0x48, profile_matrix)
        keys = m.r32(irc_slot + 0x2B8), m.r32(irc_slot + 0x2BC)
        phase("+1A170 SeedProfileCipher", lambda: m.call(VIEWER_BASE + 0x1A170, profile_cipher, *keys))
        phase("+1E980", lambda: m.call(VIEWER_BASE + 0x1E980))
        for request in (171, 175, 193):
            tk = phase("%d profile request" % request, lambda r=request: t(r))
            report["profile_%d" % request] = poll(
                phase, m, "%d check" % (request + 1), lambda r=request, k=tk: t(r + 1, k), tries=2000
            )
        for n in range(20):
            phase("153 pump #%d" % n, lambda: t(153, slot_))
            run_threads(m)

    after = struct.unpack("<1200I", m.uc.mem_read(table, 4800))
    report["table_entries_changed_at_runtime"] = [i for i in range(1200) if before[i] != after[i]]
    report["files"] = hst["files"]
    report["select_timeouts"] = hst["select_timeouts"]
    report["registry_keys"] = sorted(set(hst["keys"]))
    report["registry_values"] = sorted(set(hst["values"]))
    report["threads"] = [
        dict(entry=hex(th["entry"] - delta), proc=th.get("proc"), state=th["state"]) for th in m.threads
    ]
    report["callbacks"] = callback_log[:50]
    report["network"] = network.log[:200] if network else []
    report["import_totals"] = dict(sorted(m.counts.items()))
    json.dump(report, open(out, "w"), indent=1)
    print("threads", report["threads"])
    print("callbacks", len(callback_log), callback_log[:4])
    print("network", report["network"][:30])
    print("select timeouts", report.get("select_timeouts"))


INSTALL = "c:/playonline/squareenix/tetramaster/"


def runtime_file(path):
    """Serve reads under the TM install folder from TM_RUNTIME (the user's runtime folder), read-only."""
    root = os.environ.get("TM_RUNTIME")
    path = path.replace(chr(92), "/")
    if not root or not path.lower().startswith(INSTALL):
        return None
    local = os.path.join(root, *path[len(INSTALL) :].split("/"))
    return open(local, "rb").read() if os.path.isfile(local) else None


def poll(phase, m, name, fn, tries=200):
    """Call a check entry until it reports a result, running the guest threads between polls as TM's Sleep does."""
    result = 0
    for n in range(tries):
        result = phase("%s #%d" % (name, n), fn) if n < 2 or n % 100 == 0 else fn()
        if result:
            break
        run_threads(m)
    result = result or 0
    print("%s returned %#x after %d polls" % (name, result, n + 1))
    return result


USER_CODE_END = 0x4F530  # polcore's own code; the CRT and ATL follow


def run_threads(m):
    for th in m.threads:
        if th["state"] != "exited" and th["state"] != "suspended":
            if "proc" not in th:  # the first polcore (non-CRT) block a thread runs is its real procedure
                found = []

                def first(uc, address, size, _, found=found):
                    if not found and VIEWER_BASE + 0x1000 <= address < VIEWER_BASE + USER_CODE_END:
                        found.append(address)

                h = m.uc.hook_add(UC_HOOK_BLOCK, first)
                th["state"] = m.run_thread(th, limit=2_000_000)
                m.uc.hook_del(h)
                th["proc"] = hex(found[0] - VIEWER_BASE + common.BASE) if found else None
            else:
                th["state"] = m.run_thread(th, limit=2_000_000)


def relocations(m):
    # The whole .reloc section: its first blocks cover .text and sit before the range the data directory names,
    # so the OS loader never sees them and only the POL1 stub applies them.
    s = next(s for s in m.pe.sections if s.Name.startswith(b".reloc"))
    raw = s.get_data()
    off = 0
    while off < len(raw):
        page, size = struct.unpack_from("<II", raw, off)
        if not size:
            break
        for k in range(8, size, 2):
            e = struct.unpack_from("<H", raw, off + k)[0]
            if e >> 12 == 3:
                yield page + (e & 0xFFF)
        off += size


if __name__ == "__main__":
    main()
