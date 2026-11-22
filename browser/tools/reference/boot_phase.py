# After DllMain: create the game object as tmloader does (DllGetClassObject, IClassFactory::CreateInstance)
# and call GameStart with a fake PlayOnline core whose vtable and common-function table log every call.
import collections
import json
import struct
import uuid

from unicorn import UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP, UC_X86_REG_EIP

CORE_SLOTS = 0x73000000  # core vtable entry points, 16 bytes apart
TABLE = 0x73100000       # common-function table entry points
OBJECTS = 0x73200000
# Arguments after this, for the slots tmloader documents; anything else stops the run
CORE_ARGC = {0: 2, 1: 0, 2: 0, 3: 1, 4: 1, 5: 2, 7: 1, 26: 1, 27: 1}
# The interface core slot 3 returns: only IUnknown known
SECOND_ARGC = {0: 2, 1: 0, 2: 0}
# Viewer table entries whose fake result is not 0: ticket polls report done
TABLE_RESULT = {471: 1, 473: 1, 908: 1}

GAME_CLASS = uuid.UUID('72b2fe03-ba77-4867-84a2-7bad0f5a8fbb').bytes_le
GAME_INTERFACE = uuid.UUID('fa287029-5ec4-4938-b8c5-532865dde369').bytes_le
CLASS_FACTORY = uuid.UUID('00000001-0000-0000-c000-000000000046').bytes_le
CORE_INTERFACE = uuid.UUID('e0516654-ef77-435d-aa7d-50d2c069ce34').bytes_le


def go(g, gamestart_blocks=20_000_000):
    uc, run, calls, alloc, stop = g['uc'], g['run'], g['calls'], g['boot_alloc'], g['stop']
    pe, BASE = g['pe'], g['BASE']
    uc.mem_map(CORE_SLOTS, 0x400000)
    core_calls, table_calls = collections.Counter(), collections.Counter()

    def w32(a, v):
        uc.mem_write(a, struct.pack('<I', v & 0xFFFFFFFF))

    def r32(a):
        return struct.unpack('<I', uc.mem_read(a, 4))[0]

    # Core object: vtable of 64 slots, common-function table of 1200 entries
    core = OBJECTS
    vtable = OBJECTS + 0x100
    table = OBJECTS + 0x1000
    w32(core, vtable)
    for i in range(64):
        w32(vtable + 4 * i, CORE_SLOTS + 0x10 * i)
    second = OBJECTS + 0x800
    w32(second, OBJECTS + 0x900)
    for i in range(64):
        w32(OBJECTS + 0x900 + 4 * i, CORE_SLOTS + 0x1000 + 0x10 * i)
    registry = alloc(64)
    uc.mem_write(registry, b'SOFTWARE/PlayOnlineUS/SquareEnix'.replace(b'/', bytes([92])) + bytes(1))
    command_line = alloc(4)
    for i in range(1200):
        w32(table + 4 * i, TABLE + 0x10 * i)

    def on_core(uc_, address, size, _):
        esp = uc_.reg_read(UC_X86_REG_ESP)
        back = r32(esp)
        if address >= TABLE:
            index = (address - TABLE) // 0x10
            args = [r32(esp + 4 + 4 * k) for k in range(4)]
            table_calls[index] += 1
            calls.append(('pol-table', index, [hex(a) for a in args], 'from %#x' % back))
            uc_.reg_write(UC_X86_REG_EAX, TABLE_RESULT.get(index, 0))
            uc_.reg_write(UC_X86_REG_ESP, esp + 4)  # cdecl: the caller pops its arguments
            uc_.reg_write(UC_X86_REG_EIP, back)
            return
        obj = (address - CORE_SLOTS) // 0x1000
        slot = (address - CORE_SLOTS - obj * 0x1000) // 0x10
        known = (CORE_ARGC, SECOND_ARGC)[obj]
        core_calls['%s.%d' % (('core', 'second')[obj], slot)] += 1
        if slot not in known:
            calls.append(('core-slot', slot, 'from %#x' % back, 'unknown arity'))
            stop('%s vtable slot %d from %#x (arity unknown)' % (('core', 'second')[obj], slot, back))
            return
        args = [r32(esp + 8 + 4 * k) for k in range(known[slot])]
        calls.append(('core-slot', slot, [hex(a) for a in args], 'from %#x' % back))
        result = 0
        if slot == 0:
            w32(args[1], (core, second)[obj])
        elif obj == 0 and slot == 3:
            w32(args[0], second)
        elif obj == 0 and slot == 4:
            w32(args[0], command_line)
        elif obj == 0 and slot == 26:
            w32(args[0], registry)
        elif slot in (1, 2):
            result = 1
        elif slot == 7:
            w32(args[0], table)
        uc_.reg_write(UC_X86_REG_EAX, result)
        uc_.reg_write(UC_X86_REG_ESP, esp + 8 + 4 * known[slot])  # stdcall: this + arguments
        uc_.reg_write(UC_X86_REG_EIP, back)

    uc.hook_add(UC_HOOK_CODE, on_core, begin=CORE_SLOTS, end=TABLE + 0x10 * 1200)

    def blob(data):
        a = alloc(len(data))
        uc.mem_write(a, data)
        return a

    exports = {e.name.decode(): BASE + e.address for e in pe.DIRECTORY_ENTRY_EXPORT.symbols}
    out = blob(bytes(4))
    reason = run(exports['DllGetClassObject'], blob(GAME_CLASS), blob(CLASS_FACTORY), out)
    factory = r32(out)
    print('DllGetClassObject:', reason, hex(uc.reg_read(UC_X86_REG_EAX)), 'factory', hex(factory))
    create = r32(r32(factory) + 3 * 4)
    reason = run(create, factory, 0, blob(GAME_INTERFACE), out)
    game = r32(out)
    game_start = r32(r32(game) + 9 * 4)
    print('CreateInstance:', reason, hex(uc.reg_read(UC_X86_REG_EAX)), 'game', hex(game), 'GameStart', hex(game_start - BASE))
    descriptor = blob(struct.pack('<II', 28, 0x200) + CORE_INTERFACE + bytes(4))
    first = len(calls)
    g['api_hits'].clear()
    g['budget'][0] = g['blocks'][0] + gamestart_blocks
    reason = run(game_start, game, core, descriptor)
    print('GameStart:', reason, '| blocks', g['blocks'][0])
    boot = calls[first:]
    unique = []
    for c in boot:
        key = (c[0], c[1] if c[0] in ('pol-table', 'core-slot') else c[1:2])
        if key not in [u[0] for u in unique]:
            unique.append((key, c))
    for _, c in unique[-40:]:
        print('   ', c)
    apis = dict(g['api_hits'])
    print('core slots called', dict(core_calls))
    print('common-function table entries called', len(table_calls), sorted(table_calls))
    print('imports called during GameStart', len(apis), apis)
    return dict(reason=reason, imports=apis, core=dict(core_calls), table=dict(table_calls), calls=boot)
