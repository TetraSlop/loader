# Differential test of the C++ interpreter against Unicorn over stretches of TM_native.dll's boot run.
#
# The Unicorn reference (reference/harness.py) runs the user's DLL to a checkpoint; its whole state (memory,
# registers, x87) is copied into the interpreter, and both then step one instruction at a time. After every step the
# general registers, eip, the defined flags and every byte Unicorn wrote must agree. A stretch ends at the stand-ins
# (where the two sides' Win32 layers differ), at a fault (both must fault at the same instruction), or after --count.
#
#   TM_RUNTIME=<folder with TM_native.dll> python browser/tools/difftest.py [--count N] [--capi build/browser/tmweb_capi.dll]
#
# Without TM_RUNTIME it prints why and exits 0: there is nothing to compare without the user's files.
import argparse
import ctypes
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

FLAGS_DEFINED = 0x0001 | 0x0004 | 0x0010 | 0x0040 | 0x0080 | 0x0100 | 0x0400 | 0x0800  # CF PF AF ZF SF TF DF OF
# Flags the manual leaves undefined after these, which QEMU and the interpreter may set differently
UNDEFINED_FLAGS = {
    'bt': 0x8D4, 'bts': 0x8D4, 'btr': 0x8D4, 'btc': 0x8D4, 'bsf': 0x8D5, 'bsr': 0x8D5,
    'mul': 0xD4, 'imul': 0xD4, 'div': 0x8D5, 'idiv': 0x8D5, 'shld': 0x810, 'shrd': 0x810,
    'shl': 0x810, 'sal': 0x810, 'shr': 0x810, 'sar': 0x810, 'rol': 0x800, 'ror': 0x800, 'rcl': 0x800, 'rcr': 0x800,
    'and': 0x10, 'or': 0x10, 'xor': 0x10, 'test': 0x10, 'aaa': 0x8C4, 'aas': 0x8C4, 'daa': 0x800, 'das': 0x800,
    'aam': 0x811, 'aad': 0x811,
}
PREFIXES = {0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65}


class State(ctypes.Structure):
    _fields_ = [('r', ctypes.c_uint32 * 8), ('eip', ctypes.c_uint32), ('eflags', ctypes.c_uint32),
                ('fsBase', ctypes.c_uint32), ('gsBase', ctypes.c_uint32), ('control', ctypes.c_uint16),
                ('status', ctypes.c_uint16), ('tag', ctypes.c_uint16), ('opcode', ctypes.c_uint16),
                ('lastInstruction', ctypes.c_uint32), ('lastOperand', ctypes.c_uint32),
                ('st', (ctypes.c_uint8 * 10) * 8)]


class Event(ctypes.Structure):
    _fields_ = [('stop', ctypes.c_uint32), ('address', ctypes.c_uint32), ('vector', ctypes.c_uint32),
                ('faultAddress', ctypes.c_uint32), ('faultAccess', ctypes.c_uint32)]


STOP_BUDGET, STOP_TRAP, STOP_FAULT, STOP_INTERRUPT = 0, 1, 2, 3


class Interpreter:
    def __init__(self, path):
        self.lib = ctypes.CDLL(path)
        self.lib.tmweb_cpu_create.restype = ctypes.c_void_p
        for name, args in (('tmweb_cpu_destroy', [ctypes.c_void_p]),
                           ('tmweb_cpu_map', [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]),
                           ('tmweb_cpu_write', [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32]),
                           ('tmweb_cpu_read', [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32]),
                           ('tmweb_cpu_get', [ctypes.c_void_p, ctypes.POINTER(State)]),
                           ('tmweb_cpu_set', [ctypes.c_void_p, ctypes.POINTER(State)]),
                           ('tmweb_cpu_run', [ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(Event)])):
            getattr(self.lib, name).argtypes = args
        self.handle = self.lib.tmweb_cpu_create()

    def map(self, address, size):
        assert self.lib.tmweb_cpu_map(self.handle, address, size) == 0

    def write(self, address, data):
        assert self.lib.tmweb_cpu_write(self.handle, address, data, len(data)) == 0

    def read(self, address, size):
        buffer = ctypes.create_string_buffer(size)
        if self.lib.tmweb_cpu_read(self.handle, address, buffer, size) != 0:
            return None
        return buffer.raw

    def get(self):
        state = State()
        self.lib.tmweb_cpu_get(self.handle, ctypes.byref(state))
        return state

    def set(self, state):
        self.lib.tmweb_cpu_set(self.handle, ctypes.byref(state))

    def step(self):
        event = Event()
        self.lib.tmweb_cpu_run(self.handle, 1, ctypes.byref(event))
        return event


def unicorn_regs(uc):
    from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDI,
                                   UC_X86_REG_EDX, UC_X86_REG_EFLAGS, UC_X86_REG_EIP, UC_X86_REG_ESI, UC_X86_REG_ESP)
    regs = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP,
            UC_X86_REG_ESI, UC_X86_REG_EDI]
    return [uc.reg_read(r) for r in regs], uc.reg_read(UC_X86_REG_EIP), uc.reg_read(UC_X86_REG_EFLAGS)


def fpu_state(uc, state):
    from unicorn.x86_const import UC_X86_REG_FP0, UC_X86_REG_FPCW, UC_X86_REG_FPSW, UC_X86_REG_FPTAG
    state.control = uc.reg_read(UC_X86_REG_FPCW)
    state.status = uc.reg_read(UC_X86_REG_FPSW)
    tag = uc.reg_read(UC_X86_REG_FPTAG)
    top = (state.status >> 11) & 7
    # Unicorn's FP0..7 are ST(0)..ST(7); the interpreter keeps physical registers
    empty = 0
    for i in range(8):
        mantissa, exponent = uc.reg_read(UC_X86_REG_FP0 + i)
        physical = (top + i) & 7
        state.st[physical][:] = struct.pack('<QH', mantissa, exponent)
        if (tag >> (2 * physical)) & 3 == 3:
            empty |= 3 << (2 * physical)
    state.tag = empty


def copy(h, t, teb):
    """Unicorn's whole machine into the interpreter"""
    for begin, end, _ in h.uc.mem_regions():
        size = end - begin + 1
        t.map(begin, size)
        t.write(begin, bytes(h.uc.mem_read(begin, size)))
    state = State()
    regs, eip, eflags = unicorn_regs(h.uc)
    for i, value in enumerate(regs):
        state.r[i] = value
    state.eip, state.eflags, state.fsBase = eip, eflags, teb
    fpu_state(h.uc, state)
    t.set(state)


def opcode_at(uc, eip):
    code = bytes(uc.mem_read(eip, 16))
    k = 0
    while k < 15 and code[k] in PREFIXES:
        k += 1
    return code[k:]


def stretch(h, t, count, x87, out):
    """Step both count times or to a boundary; returns (compared, why it ended, hard mismatches)"""
    import capstone
    from unicorn import UC_HOOK_MEM_WRITE, UcError
    from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EDX, UC_X86_REG_EFLAGS

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    writes = []
    hook = h.uc.hook_add(UC_HOOK_MEM_WRITE, lambda uc_, access, address, size, value, d: writes.append((address, size)))
    boundaries = h.boundaries()
    # Blocks translated during the full-speed run carry no instruction counting: retranslate so count=1 holds
    h.uc.ctl_flush_tb()
    compared, resynced = 0, 0
    try:
        for _ in range(count):
            regs, eip, eflags = unicorn_regs(h.uc)
            if any(lo <= eip < hi for lo, hi in boundaries):
                return compared, 'reached the stand-ins at %#x' % eip, resynced, 0
            code = opcode_at(h.uc, eip)
            if not x87 and (0xD8 <= code[0] <= 0xDF or code[0] == 0x9B):
                return compared, 'x87 instruction at %#x (run with --x87 once f80 is in)' % eip, resynced, 0
            instruction = next(md.disasm(bytes(h.uc.mem_read(eip, 16)), eip), None)
            text = '%s %s' % (instruction.mnemonic, instruction.op_str) if instruction else '?'
            writes.clear()
            h.pending.clear()
            unicorn_stopped = None
            try:
                h.uc.emu_start(eip, 0xFFFFFFFF, count=1)
            except UcError as e:
                unicorn_stopped = str(e)
            if h.pending:
                unicorn_stopped = h.pending[0]
            event = t.step()
            if unicorn_stopped is not None or event.stop != STOP_BUDGET:
                same = event.stop in (STOP_FAULT, STOP_INTERRUPT) and unicorn_stopped is not None
                if same:
                    return compared, 'both stopped at %#x (%s): %s' % (eip, text, unicorn_stopped), resynced, 0
                out.write('MISMATCH at %#x %s: unicorn %s, interpreter stop %d vector %d\n'
                          % (eip, text, unicorn_stopped, event.stop, event.vector))
                return compared, 'stop mismatch', resynced, 1
            if code[:2] == b'\x0f\x31':
                state = t.get()
                h.uc.reg_write(UC_X86_REG_EAX, state.r[0])
                h.uc.reg_write(UC_X86_REG_EDX, state.r[2])
            regs, ueip, ueflags = unicorn_regs(h.uc)
            state = t.get()
            if ueip == eip and state.eip != eip and regs[1] == 0 and instruction and instruction.mnemonic.startswith('rep'):
                # QEMU tests ECX at the top of a REP loop, so after the last iteration it takes one more empty step
                # to leave the instruction; the interpreter leaves at once, as the hardware does
                h.uc.emu_start(eip, 0xFFFFFFFF, count=1)
                regs, ueip, ueflags = unicorn_regs(h.uc)
            mine = list(state.r)
            flags_mask = FLAGS_DEFINED & ~UNDEFINED_FLAGS.get(instruction.mnemonic if instruction else '', 0)
            problems = []
            if mine != regs:
                problems.append('regs ' + ' '.join('%s %08x/%08x' % (n, a, b) for n, a, b in
                                                    zip(('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'), regs, mine) if a != b))
            if state.eip != ueip:
                problems.append('eip %08x/%08x' % (ueip, state.eip))
            if (state.eflags ^ ueflags) & flags_mask:
                problems.append('eflags %08x/%08x' % (ueflags, state.eflags))
            for address, size in writes:
                theirs = bytes(h.uc.mem_read(address, size))
                ours = t.read(address, size)
                if ours != theirs:
                    problems.append('memory %#x unicorn %s interpreter %s' % (address, theirs.hex(), ours and ours.hex()))
            if problems:
                selector = instruction and any(s in instruction.op_str for s in ('cs', 'ss'))
                if selector and not (state.eip != ueip):
                    # Unicorn's CS and SS are ring-0 selectors: take its values and go on
                    state.r[:] = regs
                    t.set(state)
                    resynced += 1
                else:
                    out.write('MISMATCH at %#x %s: %s (unicorn/interpreter)\n' % (eip, text, '; '.join(problems)))
                    return compared, 'mismatch', resynced, 1
            if (state.eflags ^ ueflags) & FLAGS_DEFINED:
                state.eflags = ueflags  # an undefined flag differed: follow Unicorn so later code sees the same
                t.set(state)
                resynced += 1
            compared += 1
        return compared, 'count reached', resynced, 0
    finally:
        h.uc.hook_del(hook)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--count', type=int, default=200000, help='instructions per stretch')
    parser.add_argument('--capi', default=os.path.join(os.path.dirname(__file__), '../../build/browser/tmweb_capi.dll'))
    parser.add_argument('--x87', action='store_true', help='also step x87 instructions')
    parser.add_argument('--checkpoints', default='0,100000,1000000,5000000,15000000,25000000,29800000',
                        help='Unicorn basic-block counts in DllMain to start stretches at')
    parser.add_argument('--gamestart', type=int, default=1500000, help='blocks into GameStart for a last stretch, 0 for none')
    args = parser.parse_args()
    runtime = os.environ.get('TM_RUNTIME')
    if not runtime or not os.path.exists(os.path.join(runtime, 'TM_native.dll')):
        print('difftest: skipped, TM_RUNTIME does not name a folder with TM_native.dll')
        return 0
    from reference import boot_phase, harness

    dll = os.path.join(runtime, 'TM_native.dll')
    started = time.time()
    _, iat = harness.names(dll)
    h = harness.make(dll, iat)
    checkpoints = [int(c) for c in args.checkpoints.split(',')]
    failures, total = 0, 0

    def run_stretch(label):
        nonlocal failures, total
        t = Interpreter(args.capi)
        copy(h, t, harness.TEB)
        compared, why, resynced, bad = stretch(h, t, args.count, args.x87, sys.stdout)
        total += compared
        failures += bad
        print('%-28s %8d instructions agree; %s; %d undefined-flag resyncs' % (label, compared, why, resynced))
        sys.stdout.flush()
        t.lib.tmweb_cpu_destroy(t.handle)

    # Each stretch starts at a checkpoint; Unicorn's run then goes on from where the stretch left it
    pc = h.entry
    first = True
    for checkpoint in checkpoints:
        if checkpoint > 0:
            h.budget[0] = checkpoint
            if first:
                reason = h.call(pc, harness.BASE, 1, 0)
                first = False
            else:
                reason = h.loop(h.uc.reg_read(harness.UC_X86_REG_EIP))
            if not reason.startswith('stopped: block budget'):
                print('DllMain ended before block %d: %s' % (checkpoint, reason))
                break
        elif first:
            from unicorn.x86_const import UC_X86_REG_EIP, UC_X86_REG_ESP
            esp = harness.STACK - 0x100
            h.uc.mem_write(esp, struct.pack('<4I', harness.SENTINEL, harness.BASE, 1, 0))
            h.uc.reg_write(UC_X86_REG_ESP, esp)
            h.uc.reg_write(UC_X86_REG_EIP, pc)
        run_stretch('DllMain, block %d' % checkpoint)
        first = False
    if args.gamestart:
        h.budget[0] = None
        reason = h.loop(h.uc.reg_read(harness.UC_X86_REG_EIP))
        g = dict(vars(h), BASE=harness.BASE, run=h.call)
        boot_phase.go(g, gamestart_blocks=args.gamestart)
        run_stretch('GameStart, block +%d' % args.gamestart)
    print('difftest: %d instructions compared, %d mismatching stretches, %.0f s' % (total, failures, time.time() - started))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
