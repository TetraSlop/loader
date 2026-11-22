# TM's battle (+D31D0) and growth (+B3630) run through the C++ interpreter, checked against tmserver's frozen
# native vectors (research/native_vectors.json: 1,168 battles, 6,248 growth cases). The cases, the records and
# their digests are tmserver's own code. 24-bit precision has no frozen digest, so growth is also run at 24, 53 and
# 64 bits for every case and compared with Unicorn case by case.
#
#   TM_RUNTIME=<folder with TM_native.dll> TMSERVER=<tmserver checkout> python browser/tools/rules.py
#
# The image comes from unpacking the user's own DLL with the reference harness, in memory. Skips (exit 0) without
# both variables.
import argparse
import ctypes
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

BASE, DATA = 0x10000000, 0x204000
SCRATCH = 0x20000000
GAME, ATTACKER, DEFENDER, MODE, CATALOG, BASELINE, PRICE, CW = (SCRATCH + o for o in (
    0x0, 0x4000, 0x4100, 0x4200, 0x4300, 0x5000, 0x5010, 0x5020))
STACK_TOP = SCRATCH + 0x80000
STUB = SCRATCH + 0x90000
RANDOM = BASE + 0x1E2E44
CONTROL = {24: 0x007F, 53: 0x027F, 64: 0x037F}


def stub(target, args):
    """fldcw [CW]; push args right to left; mov eax, target; call eax"""
    code = bytes((0xD9, 0x2D)) + struct.pack('<I', CW)
    for a in reversed(args):
        code += b'\x68' + struct.pack('<I', a)
    return code + b'\xB8' + struct.pack('<I', target) + b'\xFF\xD0'


class Setup:
    """The bridge's reset() and per-case writes, on any backend with write(address, bytes)"""

    def __init__(self, backend, image):
        self.b = backend
        self.original = image[DATA:]

    def w32(self, address, value):
        self.b.write(address, struct.pack('<I', value & 0xFFFFFFFF))

    def reset(self):
        self.b.write(BASE + DATA, self.original)
        self.b.write(GAME, bytes(0x4300))
        self.b.write(CATALOG, bytes(3000))
        self.w32(BASE + 0x2B63C4, GAME)
        self.w32(BASE + 0x2BB62C, MODE)
        self.w32(BASE + 0x2B6364, CATALOG)
        self.w32(BASE + 0x2FF4FC, 250)
        self.w32(BASE + 0x2FF568, 1)
        self.b.write(BASE + 0x2B63E0 + 0xE5, b'\x02')

    def battle(self, c):
        self.reset()
        for card, stats, modifier, prop in ((ATTACKER, c['attacker'], c['modifiers'][0], c['properties'][0]),
                                            (DEFENDER, c['defender'], c['modifiers'][1], c['properties'][1])):
            self.b.write(card, bytes(0x9C))
            self.b.write(card + 8, bytes(stats))
            self.b.write(card + 0x24, bytes((prop,)))
            self.b.write(card + 0x26, bytes((modifier & 0xFF,)))
        used = self.b.call(BASE + 0xD31D0, (ATTACKER, DEFENDER), 0x037F, 0, list(c['draws']))

        def side(card):
            raw = self.b.read(card, 0x9C)
            return [struct.unpack_from('<H', raw, 0x8E)[0], struct.unpack_from('<H', raw, 0x8C)[0], raw[0x29], raw[0x2A]]

        return dict(attacker=side(ATTACKER), defender=side(DEFENDER), used=used)

    def growth(self, c, precision):
        self.reset()
        card, base = c['card'], c['baseline']
        baseline = bytearray(12)
        baseline[0], baseline[2], baseline[3], baseline[4] = base[0], base[1], base[2], base[3]
        self.b.write(BASELINE, bytes(baseline))
        self.w32(PRICE, base[4])
        self.w32(BASE + 0x2B6364, BASELINE)
        self.w32(BASE + 0x2B6368, PRICE)
        attacker = bytearray(0x9C)
        for i in range(1, 7):
            attacker[7 + i] = card[i]
        attacker[0x16] = card[7]
        self.b.write(ATTACKER, bytes(attacker))
        self.b.call(BASE + 0xB3630, (c['mode'], c['random'], GAME), CONTROL[precision], ATTACKER, [])
        raw = self.b.read(ATTACKER, 0x9C)
        result = list(card)
        changed = -1
        for i in range(1, 7):
            result[i] = raw[7 + i]
            if i <= 4 and result[i] != card[i]:
                changed = i
        return dict(card=result, sale=struct.unpack_from('<I', raw, 0x10)[0], changed=changed)


class Interpreter:
    """The C++ interpreter through tools/capi.cpp"""

    def __init__(self, capi, image):
        import difftest
        self.t = difftest.Interpreter(capi)
        self.lib = self.t.lib
        self.lib.tmweb_cpu_run_until.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
                                                 ctypes.POINTER(difftest.Event)]
        self.lib.tmweb_cpu_traps.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32]
        self.Event = difftest.Event
        patched = bytearray(image)
        patched[0xF0380:0xF0385] = bytes((0x31, 0xC0, 0xC2, 0x14, 0x00))  # growth's presentation hook, as the bridge
        self.t.map(BASE, len(patched))
        self.t.write(BASE, bytes(patched))
        self.t.map(SCRATCH, 0x100000)
        # The CRT's rand() is a trap: the host answers it from the case's draws
        self.lib.tmweb_cpu_traps(self.t.handle, RANDOM - 16, 32)

    def write(self, address, data):
        self.t.write(address, data)

    def read(self, address, size):
        return self.t.read(address, size)

    def call(self, target, args, control, ecx, draws):
        code = stub(target, args)
        self.t.write(STUB, code)
        self.t.write(CW, struct.pack('<H', control))
        state = self.t.get()
        state.r[4] = STACK_TOP - 0x100
        state.r[1] = ecx
        state.eip = STUB
        state.status = 0
        state.tag = 0xFFFF
        self.t.set(state)
        end = STUB + len(code)
        used = 0
        event = self.Event()
        while True:
            stop = self.lib.tmweb_cpu_run_until(self.t.handle, end, 1000000, ctypes.byref(event))
            if stop == 0xFF:
                return used
            if stop == 1 and event.address == RANDOM:
                state = self.t.get()
                back = struct.unpack('<I', self.t.read(state.r[4], 4))[0]
                state.r[0] = draws[used]
                used += 1
                state.r[4] += 4
                state.eip = back
                self.t.set(state)
                continue
            raise RuntimeError('interpreter stopped (%d, vector %d) at %#x' % (stop, event.vector, event.address))


class Unicorn:
    """The same calls under Unicorn, the per-case reference at 24-bit precision"""

    def __init__(self, image):
        from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_MODE_32, Uc
        from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP
        self.regs = (UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EIP, UC_X86_REG_ESP)
        patched = bytearray(image)
        patched[0xF0380:0xF0385] = bytes((0x31, 0xC0, 0xC2, 0x14, 0x00))
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.uc.mem_map(BASE, len(patched))
        self.uc.mem_write(BASE, bytes(patched))
        self.uc.mem_map(SCRATCH, 0x100000)
        self.draws = []
        self.used = 0

        def on_random(uc_, address, size, _):
            esp = uc_.reg_read(UC_X86_REG_ESP)
            back = struct.unpack('<I', uc_.mem_read(esp, 4))[0]
            uc_.reg_write(UC_X86_REG_EAX, self.draws[self.used])
            self.used += 1
            uc_.reg_write(UC_X86_REG_ESP, esp + 4)
            uc_.reg_write(UC_X86_REG_EIP, back)

        self.uc.hook_add(UC_HOOK_CODE, on_random, begin=RANDOM, end=RANDOM)

    def write(self, address, data):
        self.uc.mem_write(address, data)

    def read(self, address, size):
        return bytes(self.uc.mem_read(address, size))

    def call(self, target, args, control, ecx, draws):
        eax, ecx_reg, eip, esp = self.regs
        code = stub(target, args)
        self.uc.mem_write(STUB, code)
        self.uc.mem_write(CW, struct.pack('<H', control))
        self.uc.reg_write(esp, STACK_TOP - 0x100)
        self.uc.reg_write(ecx_reg, ecx)
        self.draws, self.used = draws, 0
        self.uc.emu_start(STUB, STUB + len(code))
        return self.used


def tmserver_modules(root):
    sys.path[:0] = [root, os.path.join(root, 'src')]
    cwd = os.getcwd()
    os.chdir(root)  # tmserver's data files resolve from its checkout
    from research.native_primitive_cases import growth_cases
    from tmserver.game import rules
    from tools import probe_original_rules as probe
    os.chdir(cwd)
    return probe, growth_cases, rules


def requests(probe, growth_cases, rules):
    battles = []
    for args, options, values in probe.battle_cases():
        sides = []
        for card in args:
            attack, physical, magical = rules._battle_stats(card)
            sides.append([attack, card.kind, physical, magical])
        battles.append(dict(attacker=sides[0], defender=sides[1],
                            modifiers=[options['attacker_modifier'], options['defender_modifier']],
                            properties=[options['attacker_property'], options['defender_property']], draws=list(values)))
    growth = []
    for card, baseline, mode, value, precision in growth_cases():
        card = rules.Card.from_card(card)
        base = [getattr(baseline, f) for f in ('attack', 'physical', 'magical', 'points', 'price')]
        growth.append(dict(card=list(card.full()), baseline=base, mode=mode & 255, random=value & 255, precision=precision))
    return battles, growth


def digests(probe, growth_cases, rules, battles, growth=None):
    records = []
    for (args, options, values), r in zip(probe.battle_cases(), battles, strict=True):
        draws = list(values)[:r['used']]
        result = rules.BattleResult(rules.BattleSide(*r['attacker']), rules.BattleSide(*r['defender']), tuple(draws))
        records.append({'result': probe.snapshot(result), 'draws': draws})
    out = dict(battles=probe.vector_digest(records))
    if growth is None:
        return out
    records = []
    for case, r in zip(growth_cases(), growth, strict=True):
        changed = None if r['changed'] == -1 else ('attack', 'kind', 'physical', 'magical')[r['changed'] - 1]
        result = rules.GrowthResult(rules.Card(*r['card']), r['sale'], changed)
        records.append({'result': probe.snapshot(result), 'draws': []})
    out['growth'] = probe.vector_digest(records)
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--capi', default=os.path.join(os.path.dirname(__file__), '../../build/browser/tmweb_capi.dll'))
    parser.add_argument('--no-sweep', action='store_true', help='skip the 24/53/64-bit comparison with Unicorn')
    parser.add_argument('--battles-only', action='store_true', help='battles only (no x87)')
    args = parser.parse_args()
    runtime, root = os.environ.get('TM_RUNTIME'), os.environ.get('TMSERVER')
    if not runtime or not root or not os.path.exists(os.path.join(runtime, 'TM_native.dll')):
        print('rules: skipped, set TM_RUNTIME (folder with TM_native.dll) and TMSERVER (tmserver checkout)')
        return 0
    from reference import harness

    started = time.time()
    image = harness.handover_image(os.path.join(runtime, 'TM_native.dll'))
    probe, growth_cases, rules = tmserver_modules(root)
    battle_cases, growth_requests = requests(probe, growth_cases, rules)
    print('unpacked the image and built %d battle and %d growth cases in %.0f s'
          % (len(battle_cases), len(growth_requests), time.time() - started))

    mine = Setup(Interpreter(args.capi, image), image)
    t = time.time()
    battles = [mine.battle(c) for c in battle_cases]
    growth = []
    if not args.battles_only:
        growth = [mine.growth(c, c['precision']) for c in growth_requests]
    print('interpreter: %d battles and %d growth cases in %.1f s' % (len(battles), len(growth), time.time() - t))
    failed = False
    groups = ['battles']
    got = digests(probe, growth_cases, rules, battles)
    if not args.battles_only:
        groups.append('growth')
        got = digests(probe, growth_cases, rules, battles, growth)
    for name in groups:
        want = probe.VECTORS['groups'][name]
        match = got[name] == want
        failed |= not match
        print('%-8s %s  %d cases  sha256 %s' % (name, 'MATCH' if match else 'DIFFER', got[name]['count'], got[name]['sha256']))
    if not args.battles_only and not args.no_sweep:
        reference = Setup(Unicorn(image), image)
        for precision in (24, 53, 64):
            differ = 0
            for c in growth_requests:
                if mine.growth(c, precision) != reference.growth(c, precision):
                    differ += 1
            failed |= differ > 0
            print('growth at %d bits: %d of %d cases differ from Unicorn' % (precision, differ, len(growth_requests)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
