# List every place TM.dll calls into the Viewer: common-table entries (cdecl, through the pointer TM keeps at
# 0x1024DFE8) and core COM slots (stdcall, through the interface pointers it keeps). For each call site it
# records the enclosing function and the strings that function references, which is what scan_paths.py uses
# to attribute call sites to a game path.
# Usage: TM_MAPPED=... python scan_tm.py out/tm_calls.json
import collections
import json
import sys

import common
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_EAX, X86_REG_ECX, X86_REG_EDX

CODE = (0x10008000, 0x10204000)
img = common.tm_image()
md = common.disassembler()

# Linear sweep with resync, as the spike did: TM's code is compiler output with few data islands.
insns = []
off = CODE[0] - common.BASE
while off < CODE[1] - common.BASE:
    got = False
    for i in md.disasm(img[off : CODE[1] - common.BASE], common.BASE + off):
        got = True
        insns.append(i)
        off = i.address - common.BASE + i.size
    if not got:
        off += 1

starts = {CODE[0]}
for i in insns:
    if i.mnemonic == "call" and i.operands[0].type == X86_OP_IMM:
        starts.add(i.operands[0].imm)
starts = sorted(s for s in starts if CODE[0] <= s < CODE[1])
callers = collections.defaultdict(set)


def function_of(address):
    import bisect

    return starts[bisect.bisect_right(starts, address) - 1]


starts_set = set(starts)
GLOBALS = {common.TM_COMMON_TABLE_GLOBAL: "table", common.TM_CORE_GLOBAL: "core3"}
# reg -> (kind, address of the load). The core object itself arrives as GameStart's argument and is stored at
# 0x1024DFE4 (found below by the same pattern); keep the scan generic over all three globals.
GLOBALS[0x1024DFE4] = "core"
sites = []
strings = collections.defaultdict(set)
loaded = {}
for i in insns:
    ops = i.operands
    fn = function_of(i.address)
    if i.mnemonic in ("call", "jmp") and ops[0].type == X86_OP_IMM and ops[0].imm in starts_set:
        callers[ops[0].imm].add(fn)
    for op in ops:
        if op.type == X86_OP_IMM:
            s = common.cstring(img, op.imm)
            if s:
                strings[fn].add(s)
    if i.mnemonic in ("ret", "jmp"):
        if i.mnemonic == "jmp" and ops[0].type == X86_OP_MEM and ops[0].mem.base in loaded and ops[0].mem.index == 0:
            kind, at = loaded[ops[0].mem.base]
            if kind in ("table", "core.vtbl", "core3.vtbl"):
                sites.append(
                    dict(
                        kind=kind.removesuffix(".vtbl"),
                        index=ops[0].mem.disp // 4,
                        site=hex(i.address),
                        function=hex(fn),
                    )
                )
        loaded.clear()
        continue
    if (
        i.mnemonic == "mov"
        and len(ops) == 2
        and ops[0].type == X86_OP_REG
        and ops[1].type == X86_OP_MEM
        and ops[1].mem.base == 0
        and ops[1].mem.index == 0
        and ops[1].mem.disp in GLOBALS
    ):
        loaded[ops[0].reg] = (GLOBALS[ops[1].mem.disp], i.address)
        continue
    if (
        i.mnemonic == "mov"
        and len(ops) == 2
        and ops[0].type == X86_OP_REG
        and ops[1].type == X86_OP_MEM
        and ops[1].mem.base in loaded
        and loaded[ops[1].mem.base][0] in ("core", "core3")
        and ops[1].mem.disp == 0
    ):
        kind, at = loaded[ops[1].mem.base]
        loaded[ops[0].reg] = (kind + ".vtbl", at)
        continue
    if i.mnemonic == "call":
        if ops[0].type == X86_OP_MEM and ops[0].mem.index == 0 and ops[0].mem.base in loaded:
            kind, at = loaded[ops[0].mem.base]
            if kind in ("table", "core.vtbl", "core3.vtbl") and i.address - at < 0x100:
                sites.append(
                    dict(
                        kind=kind.removesuffix(".vtbl"),
                        index=ops[0].mem.disp // 4,
                        site=hex(i.address),
                        function=hex(fn),
                    )
                )
        for r in (X86_REG_EAX, X86_REG_ECX, X86_REG_EDX):  # cdecl and stdcall scratch registers
            loaded.pop(r, None)
        continue
    # Any other write to a tracked register ends its tracking.
    _, written = i.regs_access()
    for r in written:
        loaded.pop(r, None)

# Argument count of a cdecl call: the caller's stack clean-up right after it.
by_address = {i.address: k for k, i in enumerate(insns)}
for s in sites:
    k = by_address[int(s["site"], 16)]
    nxt = insns[k + 1] if k + 1 < len(insns) else None
    if s["kind"] != "table" or insns[k].mnemonic == "jmp":
        s["argc"] = None
    elif nxt.mnemonic == "add" and nxt.op_str.startswith("esp, "):
        s["argc"] = int(nxt.op_str[5:], 0) // 4
    elif nxt.mnemonic == "pop" and nxt.op_str in ("ecx", "edx"):
        s["argc"] = 1
    else:
        s["argc"] = None  # cleaned up later or merged with another call's clean-up

# A Viewer call wrapped in a small thunk is attributed to the thunk's callers instead.
for s in sites:
    fn = int(s["function"], 16)
    nxt = starts[starts.index(fn) + 1] if fn != starts[-1] else CODE[1]
    s["callers"] = sorted(hex(c) for c in callers[fn]) if nxt - fn <= 0x40 else []
functions = sorted({s["function"] for s in sites} | {c for s in sites for c in s["callers"]})
out = dict(
    sites=sites,
    functions={
        f: dict(strings=sorted(strings[int(f, 16)]), callers=sorted(hex(c) for c in callers[int(f, 16)]))
        for f in functions
    },
)
json.dump(out, open(sys.argv[1], "w"), indent=1)
by = collections.Counter((s["kind"], s["index"]) for s in sites)
for kind in ("core", "core3", "table"):
    idx = sorted(k[1] for k in by if k[0] == kind)
    print(kind, "distinct", len(idx), "sites", sum(v for k, v in by.items() if k[0] == kind), idx)
