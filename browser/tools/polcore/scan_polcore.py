# Static closure of every common-table entry in polcore: the functions reachable through direct calls, jumps
# and resolved jump tables, their instruction count, the imports they reach and the polcore globals they touch.
# Indirect calls through data (vtables, callbacks) are not followed, so each closure is a lower bound.
# Also lists the thread procedures polcore starts and every import's call sites.
# With run_polcore.py's report as a second argument it also reports the core object's vtable slots.
# With scan_tm.py's output as a third argument it sizes the code behind the entries TM calls.
# Usage: POLCORE_DLL=... python scan_polcore.py out/polcore_entries.json [out/run_irc.json [out/tm_calls.json]]
import collections
import functools
import json
import sys

import common
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

pe, img = common.polcore_image()
iat = common.polcore_imports(pe)
md = common.disassembler()
TEXT = (common.BASE + 0x1000, common.BASE + 0x65000)
DATA = (common.BASE + 0x6F000, common.BASE + 0x408000)
# polcore's own code ends at +4F530; after it come the statically linked CRT and ATL (built with /GS, padded with
# int3) and, from +63D40, import thunks. Closures stop at the library: a host build supplies its own libc.
LIBRARY = (common.BASE + 0x4F530, common.BASE + 0x63D40)


def in_text(a):
    return TEXT[0] <= a < TEXT[1]


@functools.cache
def function(start):
    """One function by recursive descent: (instructions, callees, imports, globals, strings, pushed code addresses)."""
    seen, todo = set(), [start]
    n, callees, imports, globals_, strings, pushed = 0, set(), set(), set(), set(), set()
    while todo:
        pc = todo.pop()
        while pc not in seen and in_text(pc):
            seen.add(pc)
            i = next(md.disasm(img[pc - common.BASE : pc - common.BASE + 16], pc), None)
            if i is None:
                break
            n += 1
            ops = i.operands
            for op in ops:
                if op.type == X86_OP_MEM and op.mem.base == 0:
                    if op.mem.disp in iat:
                        imports.add(iat[op.mem.disp])
                    elif DATA[0] <= op.mem.disp < DATA[1]:
                        globals_.add(op.mem.disp)
                elif op.type == X86_OP_IMM:
                    s = common.cstring(img, op.imm)
                    if s:
                        strings.add(s)
                    elif DATA[0] <= op.imm < DATA[1]:
                        globals_.add(op.imm)
                    elif in_text(op.imm) and i.mnemonic in ("push", "mov"):
                        pushed.add(op.imm)
            nxt = i.address + i.size
            m = i.mnemonic
            if m == "call":
                if ops[0].type == X86_OP_IMM:
                    callees.add(ops[0].imm)
                pc = nxt
            elif m == "jmp":
                if ops[0].type == X86_OP_IMM:
                    t = ops[0].imm
                    # A jump outside the current body that lands on a known start is a tail call.
                    todo.append(t)
                elif (
                    ops[0].type == X86_OP_MEM
                    and ops[0].mem.base == 0
                    and ops[0].mem.scale == 4
                    and DATA[0] > ops[0].mem.disp >= TEXT[0]
                ):
                    k = ops[0].mem.disp
                    while in_text(common.u32(img, k)):  # switch jump table inside .text
                        todo.append(common.u32(img, k))
                        k += 4
                break
            elif m.startswith("j") or m.startswith("loop"):
                todo.append(ops[0].imm)
                pc = nxt
            elif m in ("ret", "retf", "int3", "hlt"):
                break
            else:
                pc = nxt
    return n, frozenset(callees), frozenset(imports), frozenset(globals_), frozenset(strings), frozenset(pushed)


def params(start):
    """Arguments a cdecl function reads: [esp+n] before its first call, [ebp+n] anywhere once ebp is the frame."""
    seen, todo, most = set(), [(start, 0, False, True)], 0
    while todo:
        pc, delta, frame, early = todo.pop()
        while (pc, early) not in seen and in_text(pc):
            seen.add((pc, early))
            i = next(md.disasm(img[pc - common.BASE : pc - common.BASE + 16], pc), None)
            if i is None:
                break
            for op in i.operands:
                if op.type == X86_OP_MEM and op.mem.index == 0:
                    reg = md.reg_name(op.mem.base) if op.mem.base else None
                    if reg == "esp" and early and op.mem.disp - delta >= 4:
                        most = max(most, (op.mem.disp - delta) // 4)
                    elif reg == "ebp" and frame and op.mem.disp >= 8:
                        most = max(most, (op.mem.disp - 4) // 4)
            m, ops = i.mnemonic, i.operands
            if m == "push":
                delta += 4
            elif m == "pop":
                delta -= 4
            elif m == "sub" and i.op_str.startswith("esp, ") and ops[1].type == X86_OP_IMM:
                delta += ops[1].imm
            elif m == "add" and i.op_str.startswith("esp, ") and ops[1].type == X86_OP_IMM:
                delta -= ops[1].imm
            elif m == "mov" and i.op_str == "ebp, esp" and delta == 4:
                frame = True
            elif m == "call":
                early = False
            if m in ("ret", "jmp"):
                if m == "jmp" and ops[0].type == X86_OP_IMM:
                    todo.append((ops[0].imm, delta, frame, early))
                break
            if m.startswith("j"):
                todo.append((ops[0].imm, delta, frame, early))
            pc = i.address + i.size
    return min(most, 16)


def closure(entry):
    seen, todo, library = set(), [entry], set()
    while todo:
        f = todo.pop()
        if f in seen or not in_text(f):
            continue
        if LIBRARY[0] <= f < LIBRARY[1] and f != entry:
            library.add(f)
            continue
        seen.add(f)
        todo.extend(function(f)[1])
    total = collections.Counter()
    imports, globals_, strings = set(), set(), set()
    for f in seen:
        n, _, imp, glob, strs, _ = function(f)
        total["insns"] += n
        imports |= imp
        globals_ |= glob
        strings |= strs
    return dict(
        functions=len(seen),
        insns=total["insns"],
        library_calls=len(library),
        imports=sorted(imports),
        globals=sorted(hex(g) for g in globals_),
        strings=sorted(strings),
    ), seen


table = common.common_table(img)
entries = {}
for index, target in enumerate(table):
    if not target:
        continue
    info, _ = closure(target)
    entries[index] = dict(target=hex(target), params=params(target), own_insns=function(target)[0], **info)

# Every call site of each import, and the thread procedures handed to CreateThread.
sites = collections.defaultdict(list)
threads = set()
all_functions = set()
for index, target in enumerate(table):
    if target:
        all_functions |= closure(target)[1]
for f in sorted(all_functions):
    _, _, imp, _, _, pushed = function(f)
    for name in imp:
        sites[name].append(hex(f))
    if "kernel32!CreateThread" in imp:
        threads |= {hex(p) for p in pushed}
core = {}
if len(sys.argv) > 2:
    for n, rva in enumerate(json.load(open(sys.argv[2]))["core_vtable"]):
        info, _ = closure(int(rva, 16))
        core[n] = dict(target=rva, insns=info["insns"], imports=info["imports"], strings=info["strings"][:6])
tm = {}
if len(sys.argv) > 3:
    used = {
        s["index"] for s in json.load(open(sys.argv[3]))["sites"] if s["kind"] == "table" and s["index"] < len(table)
    }
    reach = set()
    for i in used:
        reach |= closure(table[i])[1]
    tm = dict(
        entries=len(used),
        functions=len(reach),
        insns=sum(function(f)[0] for f in reach),
        all_entries_functions=len(all_functions),
        all_entries_insns=sum(function(f)[0] for f in all_functions),
        user_code_bytes=LIBRARY[0] - TEXT[0],
    )
    print("TM entries", tm)
json.dump(
    dict(entries=entries, core=core, tm=tm, import_functions=sites, thread_candidates=sorted(threads)),
    open(sys.argv[1], "w"),
    indent=1,
)
reached = set().union(*(set(e["imports"]) for e in entries.values()))
print("entries", len(entries), "functions reached from the table", len(all_functions))
print("imports reached", len(reached), "of", len(iat))
print("unreached:", " ".join(sorted(set(iat.values()) - reached)))
