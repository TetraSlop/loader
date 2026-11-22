# The Unicorn reference run of TM_native.dll: its protector (DllMain), then optionally the boot to
# Direct3DCreate8. Ported from the wasm-spike's unpack_emu.py. It is the oracle the C++ interpreter is checked
# against (difftest.py), and the reference for milestone 2's observations.
#
#   h = make(dll, iat)    a fresh machine; iat is from names(), or None to name the imports first
#   h.call(pc, *args)     call guest code, returning why it stopped
#   h.loop(pc)            continue at pc after a stop (budget[0] stops it at a block count)
#
# The real 32-bit kernel32.dll is mapped as data (never executed) because the protector walks its exports.
import collections
import struct
import types

import pefile
from unicorn import (UC_ARCH_X86, UC_HOOK_BLOCK, UC_HOOK_CODE, UC_HOOK_INSN, UC_HOOK_INTR, UC_HOOK_MEM_INVALID,
                     UC_MODE_32, Uc, UcError)
from unicorn.x86_const import *  # noqa: F403

from . import hle_boot, hle_files, hle_kernel, seh

BASE = 0x10000000
STUBS = 0x70000000  # the real kernel32 image, as data
FAKE = 0x71000000   # entry points GetProcAddress hands out for other modules
HANDLES = 0x72000000
STACK = 0x00800000
TEB = 0x7FFD0000
GDT = 0x7FFE0000
SENTINEL = 0x7FFF0000
SEH_RETURN = SENTINEL + 0x10
KERNEL32 = 'C:/Windows/SysWOW64/kernel32.dll'
MODULE_PATH = b'C:\\PlayOnline\\SquareEnix\\TetraMaster\\TM.dll'

# Windows user-mode selectors, as the C++ interpreter reports them: data, TEB
SELECTOR_DATA, SELECTOR_TEB = 0x23, 0x3B
# What the C++ interpreter's CPUID answers (core/cpu.cpp)
CPUID = {0: (1, 0x756E6547, 0x6C65746E, 0x49656E69), 1: (0x633, 0, 0, 0x813F), 0x80000000: (0x80000000, 0, 0, 0)}
INTR_CODES = {0: seh.DIVIDE, 1: seh.SINGLE_STEP, 3: seh.BREAKPOINT, 6: seh.ILLEGAL, 13: seh.PRIVILEGED}


def descriptor(base, limit, access, flags=0xC):
    return struct.pack('<HHBBBB', limit & 0xFFFF, base & 0xFFFF, (base >> 16) & 0xFF, access,
                       (flags << 4) | ((limit >> 16) & 0xF), (base >> 24) & 0xFF)


def make(dll_path, iat=None):
    pe = pefile.PE(dll_path)
    size = (pe.OPTIONAL_HEADER.SizeOfImage + 0xFFF) & ~0xFFF
    uc = Uc(UC_ARCH_X86, UC_MODE_32)
    uc.mem_map(BASE, size)
    uc.mem_write(BASE, pe.header)
    for s in pe.sections:
        uc.mem_write(BASE + s.VirtualAddress, s.get_data()[: s.Misc_VirtualSize or s.SizeOfRawData])
    for region in (FAKE, HANDLES, TEB, SENTINEL):
        uc.mem_map(region, 0x10000)
    uc.mem_map(STACK - 0x100000, 0x100000)
    uc.mem_write(TEB + 0x18, struct.pack('<I', TEB))
    uc.mem_write(TEB + 0x30, struct.pack('<I', TEB + 0x1000))
    # Unicorn keeps CPL 0 whatever CS says, so CS and SS are ring-0 selectors (1 and 2); DS, ES and FS carry the
    # values Windows gives user mode (4 data, 7 TEB), as the C++ interpreter reports them
    uc.mem_map(GDT, 0x1000)
    gdt = bytearray(8 * 8)
    gdt[1 * 8:2 * 8] = descriptor(0, 0xFFFFF, 0x9A)
    gdt[2 * 8:3 * 8] = descriptor(0, 0xFFFFF, 0x92)
    gdt[4 * 8:5 * 8] = descriptor(0, 0xFFFFF, 0xF2)
    gdt[7 * 8:8 * 8] = descriptor(TEB, 0xFFF, 0xF2, 0x4)
    uc.mem_write(GDT, bytes(gdt))
    uc.reg_write(UC_X86_REG_GDTR, (0, GDT, len(gdt) - 1, 0))
    uc.reg_write(UC_X86_REG_CS, 0x08)
    uc.reg_write(UC_X86_REG_SS, 0x10)
    for reg in (UC_X86_REG_DS, UC_X86_REG_ES):
        uc.reg_write(reg, SELECTOR_DATA)
    uc.reg_write(UC_X86_REG_FS, SELECTOR_TEB)

    modules, procs, calls = {}, {}, []
    api_hits = collections.Counter()
    pending = []
    blocks = [0]
    budget = [None]
    probe = []

    def cstr(addr):
        out = b''
        while True:
            c = uc.mem_read(addr, 1)
            if c == b'\0':
                return out.decode('latin1')
            out += bytes(c)
            addr += 1

    def arg(i):
        return struct.unpack('<I', uc.mem_read(uc.reg_read(UC_X86_REG_ESP) + 4 + 4 * i, 4))[0]

    def ret(value, argc):
        esp = uc.reg_read(UC_X86_REG_ESP)
        back = struct.unpack('<I', uc.mem_read(esp, 4))[0]
        uc.reg_write(UC_X86_REG_EAX, value)
        uc.reg_write(UC_X86_REG_ESP, esp + 4 + 4 * argc)
        uc.reg_write(UC_X86_REG_EIP, back)

    def module_handle(name):
        key = name.lower()
        if key not in modules:
            modules[key] = HANDLES + 0x100 * len(modules)
        return modules[key]

    def load_library(name_ptr):
        name = cstr(name_ptr)
        calls.append(('LoadLibraryA', name))
        return module_handle(name)

    def get_module_handle(name_ptr):
        if name_ptr == 0:
            return BASE
        name = cstr(name_ptr)
        calls.append(('GetModuleHandleA', name))
        return module_handle(name)

    def get_proc_address(handle, name_ptr):
        module = next((m for m, h in modules.items() if h == handle), hex(handle))
        name = '#%d' % name_ptr if name_ptr < 0x10000 else cstr(name_ptr)
        if module == 'kernel32.dll' and name in k32_by_name:
            address = procs.setdefault((module, name), k32_by_name[name])
        else:
            address = procs.setdefault((module, name), FAKE + 0x10 * len(procs))
        calls.append(('GetProcAddress', module, name))
        return address

    k32 = pefile.PE(KERNEL32)
    k32img = k32.get_memory_mapped_image()
    uc.mem_map(STUBS, (len(k32img) + 0xFFFF) & ~0xFFFF)
    uc.mem_write(STUBS, k32img)
    k32_exports = {STUBS + e.address: e.name.decode() for e in k32.DIRECTORY_ENTRY_EXPORT.symbols if e.name}
    k32_by_name = {v: k for k, v in k32_exports.items()}
    modules['kernel32.dll'] = STUBS
    stubs_end = STUBS + len(k32img)
    heap_top = [0x20000000]

    def virtual_alloc(address, size_, kind, protect):
        size_ = (size_ + 0xFFFF) & ~0xFFFF
        if address:
            calls.append(('VirtualAlloc', hex(address), hex(size_)))
            try:
                uc.mem_map(address & ~0xFFF, (size_ + (address & 0xFFF) + 0xFFF) & ~0xFFF)
            except UcError:
                pass  # inside the image: already mapped
            return address
        address = heap_top[0]
        heap_top[0] += size_
        uc.mem_map(address, size_)
        calls.append(('VirtualAlloc', hex(address), hex(size_)))
        return address

    def virtual_protect(address, size_, protect, old):
        calls.append(('VirtualProtect', hex(address), hex(size_), hex(protect)))
        if old:
            uc.mem_write(old, struct.pack('<I', 0x40))
        return 1

    def heap(n):
        n = (n + 0xFFFF) & ~0xFFFF
        address = heap_top[0]
        heap_top[0] += n
        uc.mem_map(address, n)
        return address

    def stop(reason):
        pending.append(('done', reason))
        uc.emu_stop()

    api = {
        'VirtualAlloc': (virtual_alloc, 4),
        'VirtualProtect': (virtual_protect, 4),
        'VirtualFree': (lambda *a: (calls.append(('VirtualFree', hex(a[0]))), 1)[1], 3),
        'GetProcAddress': (get_proc_address, 2),
        'GetModuleHandleA': (get_module_handle, 1),
        'LoadLibraryA': (load_library, 1),
    }
    api.update(hle_kernel.install(dict(uc=uc, cstr=cstr, calls=calls, heap=heap,
                                       file_bytes=open(dll_path, 'rb').read(), module_path=MODULE_PATH)))
    boot_api, boot_alloc = (None, None)
    if iat is not None:
        boot_api, boot_alloc = hle_boot.install(dict(uc=uc, cstr=cstr, calls=calls, heap=heap, stop=stop))
        api.update(boot_api)
    entry_of = dict(k32_exports)
    for d in pe.DIRECTORY_ENTRY_IMPORT:
        for imp in d.imports:
            name = imp.name.decode() if imp.name else '#%d' % imp.ordinal
            if d.dll.lower() == b'kernel32.dll':
                uc.mem_write(imp.address, struct.pack('<I', k32_by_name[name]))
            else:
                addr = FAKE + 0x8000 + 0x10 * len(procs)
                procs[(d.dll.decode().lower(), name)] = addr
                uc.mem_write(imp.address, struct.pack('<I', addr))
    iat_patched = [False]

    def dispatch(uc_, name):
        fn, argc = api[name]
        api_hits[name] += 1
        ret(fn(*[arg(i) for i in range(argc)]), argc)

    def on_code(uc_, address, size_, _):
        if probe and probe[0] is None:
            if STUBS <= address < stubs_end:
                probe.append(('kernel32.dll', entry_of.get(address, hex(address))))
                uc_.emu_stop()
            elif FAKE <= address < FAKE + 0x10000:
                probe.append(next((k for k, v in procs.items() if v == address), None))
                uc_.emu_stop()
            return
        back = struct.unpack('<I', uc_.mem_read(uc_.reg_read(UC_X86_REG_ESP), 4))[0]
        if iat is not None and not iat_patched[0] and BASE + 0x8000 <= back < BASE + 0x204000:
            # The game's own code is running: point its IAT at the APIs, past the protector's thunks
            iat_patched[0] = True
            for s in iat:
                if s['module'] == 'kernel32.dll' and s['name'] in k32_by_name:
                    target = k32_by_name[s['name']]
                else:
                    target = procs.setdefault((s['module'], s['name']), FAKE + 0x10 * len(procs))
                uc_.mem_write(BASE + int(s['rva'], 16), struct.pack('<I', target))
            calls.append(('iat', 'repointed at first game call', hex(back)))
            api.update(hle_files.install(dict(uc=uc, cstr=cstr, calls=calls)))
        if STUBS <= address < stubs_end:
            name = entry_of.get(address, hex(address))
        elif FAKE <= address < FAKE + 0x10000:
            key = next((k for k, v in procs.items() if v == address), None)
            name = key[1] if key else None
        else:
            if address == SENTINEL:
                uc_.emu_stop()
            return
        if name not in api:
            stop('unhandled import %s from %#x' % (name, back))
            return
        dispatch(uc_, name)

    dispatcher = seh.Dispatcher(uc, TEB, SEH_RETURN)

    def on_invalid(uc_, access, address, size_, value, _):
        pending.append(('fault', access, address))
        return False

    def on_intr(uc_, intno, _):
        pending.append(('intr', intno))
        uc_.emu_stop()

    def on_seh_return(uc_, address, size_, _):
        pending.append(('seh_return',))
        uc_.emu_stop()

    def on_block(u, a, s, d):
        blocks[0] += 1
        if budget[0] is not None and blocks[0] > budget[0]:
            budget[0] = None
            pending.append(('done', 'block budget spent, last block %#x' % a))
            u.emu_stop()

    def on_cpuid(uc_, _):
        values = CPUID.get(uc_.reg_read(UC_X86_REG_EAX), (0, 0, 0, 0))
        for reg, value in zip((UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX), values):
            uc_.reg_write(reg, value)
        return 1  # skip the instruction's own semantics

    uc.hook_add(UC_HOOK_CODE, on_code, begin=STUBS, end=FAKE + 0x10000)
    uc.hook_add(UC_HOOK_CODE, on_code, begin=SENTINEL, end=SENTINEL + 1)
    uc.hook_add(UC_HOOK_CODE, on_seh_return, begin=SEH_RETURN, end=SEH_RETURN + 1)
    uc.hook_add(UC_HOOK_BLOCK, on_block)
    uc.hook_add(UC_HOOK_MEM_INVALID, on_invalid)
    uc.hook_add(UC_HOOK_INTR, on_intr)
    uc.hook_add(UC_HOOK_INSN, on_cpuid, None, 1, 0, UC_X86_INS_CPUID)

    def loop(pc):
        """Run from pc, dispatching faults to SEH, until a stop. Returns why it stopped."""
        while dispatcher.count < 100000:
            pending.clear()
            try:
                uc.emu_start(pc, SENTINEL)
            except (UcError, RuntimeError) as e:
                if not pending:
                    return 'error %s at %#x' % (e, uc.reg_read(UC_X86_REG_EIP))
            eip = uc.reg_read(UC_X86_REG_EIP)
            if not pending:
                return 'returned' if eip == SENTINEL else 'stopped at %#x' % eip
            event = pending[0]
            if event[0] == 'done':
                return 'stopped: ' + event[1]
            if event[0] == 'fault':
                write = 1 if event[1] in (20, 23) else 0
                if event[1] in (21, 24):
                    write = 8
                handler = dispatcher.raise_(seh.ACCESS_VIOLATION, eip, (write, event[2]))
            elif event[0] == 'intr':
                address = eip - 1 if event[1] == 3 else eip
                handler = dispatcher.raise_(INTR_CODES.get(event[1], 0xC0000000 | event[1]), address)
            else:
                handler = dispatcher.returned()
            if handler is None:
                return 'unhandled exception %s at %#x' % (event, eip)
            pc = handler
        return 'too many exceptions'

    def call(pc, *args):
        """Call pc(args...) returning to SENTINEL"""
        esp = STACK - 0x100
        uc.mem_write(esp, struct.pack('<%dI' % (len(args) + 1), SENTINEL, *args))
        uc.reg_write(UC_X86_REG_ESP, esp)
        return loop(pc)

    def run(pc, *args):
        return call(pc, *args)

    def boundaries():
        """Address ranges where guest code leaves for the stand-ins"""
        return [(STUBS, FAKE + 0x10000), (SENTINEL, SENTINEL + 0x10000), (0x73000000, 0x73400000)]

    entry = BASE + pe.OPTIONAL_HEADER.AddressOfEntryPoint
    h = types.SimpleNamespace(**{k: v for k, v in locals().items() if k != 'h'})
    return h


def names(dll_path):
    """Unpack, then name each IAT slot by running its protector thunk until it lands on an API"""
    h = make(dll_path)
    reason = h.call(h.entry, BASE, 1, 0)
    by_addr = {v: k for k, v in h.procs.items()}
    slots = []
    saved = h.uc.context_save()
    for rva in range(0x204000, 0x204278, 4):
        v = struct.unpack('<I', h.uc.mem_read(BASE + rva, 4))[0]
        if v == 0:
            continue
        if v in by_addr:
            slots.append(dict(rva=hex(rva), module=by_addr[v][0], name=by_addr[v][1]))
            continue
        h.probe[:] = [None]
        h.uc.context_restore(saved)
        h.uc.reg_write(UC_X86_REG_ESP, STACK - 0x8000)
        try:
            h.uc.emu_start(v, 0, count=10000)
        except UcError:
            pass
        hit = h.probe[1] if len(h.probe) > 1 else None
        name = hit[1] if hit else hex(v)
        if name.startswith('0x7'):
            # Landed past the start of an export: the protector ran the API's first instructions itself
            target = int(name, 16)
            name = max((a, n) for a, n in h.k32_exports.items() if a <= target)[1]
        slots.append(dict(rva=hex(rva), module=hit[0] if hit else None, name=name))
    h.probe.clear()
    return reason, slots


def handover_image(dll_path):
    """The image as the protector hands over to the game: its CRT's first call (GetVersion) stops the run. This is
    the state tmserver's mapped image holds."""
    h = make(dll_path)
    reason = h.call(h.entry, BASE, 1, 0)
    if 'GetVersion' not in reason:
        raise RuntimeError('the protector did not hand over: ' + reason)
    return unpacked_image(h)


def unpacked_image(h):
    """The game image as the protector leaves it, RVA-indexed"""
    return bytes(h.uc.mem_read(BASE, h.size))
