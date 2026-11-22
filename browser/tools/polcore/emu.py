# A small Win32 user-mode harness for polcore under Unicorn: maps the packed DLL at any base the way the
# Windows loader does (relocations, imports bound to logging stubs), runs its own POL1 stub, and calls into
# it with a return sentinel. Guest threads are recorded and run cooperatively on request. Every import call
# is logged with its caller, so run_polcore.py can report what polcore needs from the host.
import collections
import struct

import pefile
from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_MEM_INVALID, UC_MODE_32, Uc, UcError
from unicorn.x86_const import (
    UC_X86_REG_CS,
    UC_X86_REG_DS,
    UC_X86_REG_EAX,
    UC_X86_REG_EIP,
    UC_X86_REG_ES,
    UC_X86_REG_ESP,
    UC_X86_REG_FS,
    UC_X86_REG_GDTR,
    UC_X86_REG_GS,
    UC_X86_REG_SS,
)

STUBS = 0x70000000
HEAP = 0x30000000
HEAP_SIZE = 0x4000000
TEB = 0x7FFD0000
GDT = 0x7FFE0000
SENTINEL = 0x7FFF0000
STACKS = 0x01000000  # one 1 MB stack per guest thread


class Stop(Exception):
    pass


class Machine:
    def __init__(self, path, base):
        self.uc = uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.pe = pe = pefile.PE(path)
        self.base = base
        self.size = (pe.OPTIONAL_HEADER.SizeOfImage + 0xFFF) & ~0xFFF
        uc.mem_map(base, self.size)
        uc.mem_write(base, pe.header)
        for s in pe.sections:
            data = s.get_data()[: s.Misc_VirtualSize] if s.SizeOfRawData else b""
            uc.mem_write(base + s.VirtualAddress, data)
        self.relocate(base - pe.OPTIONAL_HEADER.ImageBase)
        for region, size in (
            (STUBS, 0x10000),
            (TEB, 0x10000),
            (SENTINEL, 0x1000),
            (HEAP, HEAP_SIZE),
            (STACKS, 0x1000000),
        ):
            uc.mem_map(region, size)
        self.segments()
        self.heap_top = HEAP
        self.blocks = {}
        self.calls = []  # (thread, import, args, caller)
        self.counts = collections.Counter()
        self.api = {}
        self.stubs = {}
        self.threads = []  # dicts: entry, param, context, state
        self.current = 0
        self.bind_imports()
        uc.hook_add(UC_HOOK_CODE, self.on_stub, begin=STUBS, end=STUBS + 0x10000)
        uc.hook_add(UC_HOOK_MEM_INVALID, self.on_invalid)
        self.fault = None

    # --- loader ---
    def relocate(self, delta):
        if not delta:
            return
        d = self.pe.OPTIONAL_HEADER.DATA_DIRECTORY[5]
        raw = self.pe.get_data(d.VirtualAddress, d.Size)
        off = 0
        while off < len(raw):
            page, size = struct.unpack_from("<II", raw, off)
            if not size:
                break
            for k in range(8, size, 2):
                e = struct.unpack_from("<H", raw, off + k)[0]
                if e >> 12 == 3:
                    a = self.base + page + (e & 0xFFF)
                    self.w32(a, (self.r32(a) + delta) & 0xFFFFFFFF)
            off += size

    def bind_imports(self):
        n = 0
        for d in self.pe.DIRECTORY_ENTRY_IMPORT:
            dll = d.dll.decode().lower().removesuffix(".dll")
            for imp in d.imports:
                name = imp.name.decode() if imp.name else "#%d" % imp.ordinal
                stub = STUBS + 8 * n
                n += 1
                self.stubs[stub] = (dll, name)
                self.w32(imp.address - self.pe.OPTIONAL_HEADER.ImageBase + self.base, stub)

    def segments(self):
        uc = self.uc

        def descriptor(base, limit, access, flags=0xC):
            return struct.pack(
                "<HHBBBB",
                limit & 0xFFFF,
                base & 0xFFFF,
                (base >> 16) & 0xFF,
                access,
                (flags << 4) | ((limit >> 16) & 0xF),
                (base >> 24) & 0xFF,
            )

        uc.mem_map(GDT, 0x1000)
        uc.mem_write(
            GDT,
            bytes(8) + descriptor(0, 0xFFFFF, 0x9A) + descriptor(0, 0xFFFFF, 0x92) + descriptor(TEB, 0xFFF, 0x92, 0x4),
        )
        uc.reg_write(UC_X86_REG_GDTR, (0, GDT, 0x1F, 0))
        uc.reg_write(UC_X86_REG_CS, 0x08)
        for reg in (UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS, UC_X86_REG_GS):
            uc.reg_write(reg, 0x10)
        uc.reg_write(UC_X86_REG_FS, 0x18)
        self.w32(TEB, 0xFFFFFFFF)
        self.w32(TEB + 0x18, TEB)
        self.w32(TEB + 0x30, TEB + 0x1000)

    # --- memory ---
    def r32(self, a):
        return struct.unpack("<I", self.uc.mem_read(a, 4))[0]

    def w32(self, a, v):
        self.uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))

    def cstr(self, a, limit=512):
        if not a:
            return None
        out = bytearray()
        while len(out) < limit:
            c = self.uc.mem_read(a + len(out), 1)[0]
            if not c:
                break
            out.append(c)
        return out.decode("latin1")

    def alloc(self, n, zero=True):
        n = max(n, 1)
        a = self.heap_top
        self.heap_top += (n + 15) & ~15
        if self.heap_top > HEAP + HEAP_SIZE:
            raise Stop("guest heap exhausted")
        self.blocks[a] = n
        if zero:
            self.uc.mem_write(a, bytes(n))
        return a

    def string(self, text):
        raw = text.encode("latin1") + b"\0"
        a = self.alloc(len(raw))
        self.uc.mem_write(a, raw)
        return a

    # --- imports ---
    def arg(self, i):
        return self.r32(self.uc.reg_read(UC_X86_REG_ESP) + 4 + 4 * i)

    def on_stub(self, uc, address, size, _):
        if address not in self.stubs:
            return
        dll, name = self.stubs[address]
        caller = self.r32(uc.reg_read(UC_X86_REG_ESP))
        handler = self.api.get(name)
        if handler is None:
            self.calls.append((self.current, name, None, caller))
            self.fault = "unhandled import %s!%s from %#x" % (dll, name, caller)
            uc.emu_stop()
            return
        fn, argc = handler
        args = [self.arg(i) for i in range(argc)]
        self.counts[name] += 1
        self.calls.append((self.current, name, args, caller))
        try:
            result = fn(*args)
        except Stop as e:
            self.fault = str(e)
            uc.emu_stop()
            return
        except Exception as e:  # a stand-in bug: report it with its import instead of unwinding Unicorn
            self.fault = "%s in %s(%s) from %#x" % (type(e).__name__, name, e, caller)
            uc.emu_stop()
            return
        if result is BLOCK:  # the handler parked this thread; it resumes at the same call later
            uc.reg_write(UC_X86_REG_EIP, address)
            self.fault = "block"
            uc.emu_stop()
            return
        esp = uc.reg_read(UC_X86_REG_ESP)
        uc.reg_write(UC_X86_REG_EAX, (result or 0) & 0xFFFFFFFF)
        uc.reg_write(UC_X86_REG_ESP, esp + 4 + 4 * argc)
        uc.reg_write(UC_X86_REG_EIP, caller)

    def on_invalid(self, uc, access, address, size, value, _):
        self.fault = "memory fault at %#x (access %d) eip %#x" % (address, access, uc.reg_read(UC_X86_REG_EIP))
        return False

    # --- calls ---
    def call(self, pc, *args, limit=3_000_000_000):
        """Run pc(*args) on the main thread until it returns to the sentinel. Returns eax."""
        esp = STACKS + 0x100000 - 0x100
        self.uc.mem_write(esp, struct.pack("<%dI" % (len(args) + 1), SENTINEL, *args))
        self.uc.reg_write(UC_X86_REG_ESP, esp)
        self.current = 0
        return self.resume(pc, limit)

    CHUNK = 10_000_000

    def resume(self, pc, limit):
        """Run in chunks so the instruction count is known to within one chunk (self.executed)."""
        self.executed = 0
        while self.executed < limit:
            self.fault = None
            try:
                self.uc.emu_start(pc, SENTINEL, count=self.CHUNK)
            except UcError as e:
                if not self.fault:
                    self.fault = "%s at %#x" % (e, self.uc.reg_read(UC_X86_REG_EIP))
            eip = self.uc.reg_read(UC_X86_REG_EIP)
            if eip == SENTINEL:
                return self.uc.reg_read(UC_X86_REG_EAX)
            if self.fault == "block":
                raise Stop("main thread blocked in %s" % self.calls[-1][1])
            if self.fault:
                raise Stop(self.fault)
            self.executed += self.CHUNK
            pc = eip
        raise Stop("instruction budget spent at %#x" % eip)

    def run_thread(self, t, limit=5_000_000):
        """Run guest thread t until it blocks, exits or spends its budget. Returns why it stopped."""
        saved = self.uc.context_save()
        self.current = t["id"]
        if t["context"] is None:
            esp = STACKS + 0x100000 * (t["id"] + 1) - 0x100
            self.uc.mem_write(esp, struct.pack("<II", SENTINEL, t["param"]))
            self.uc.reg_write(UC_X86_REG_ESP, esp)
            pc = t["entry"]
        else:
            self.uc.context_restore(t["context"])
            pc = self.uc.reg_read(UC_X86_REG_EIP)
        self.fault = None
        try:
            self.uc.emu_start(pc, SENTINEL, count=limit)
        except UcError as e:
            self.fault = self.fault or "%s at %#x" % (e, self.uc.reg_read(UC_X86_REG_EIP))
        eip = self.uc.reg_read(UC_X86_REG_EIP)
        why = "exited" if eip == SENTINEL or self.fault == "exited" else (self.fault or "budget")
        t["context"] = None if why == "exited" else self.uc.context_save()
        t["state"] = why
        self.uc.context_restore(saved)
        self.current = 0
        return why


BLOCK = object()
