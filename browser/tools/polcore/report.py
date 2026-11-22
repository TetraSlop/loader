# Build the entry table of browser/docs/polcore.md from the scan and run outputs plus entries.py.
# Usage: python report.py out/tm_calls.json out/polcore_entries.json out/run_irc.json > out/table.md
import collections
import json
import sys

import entries

tm = json.load(open(sys.argv[1]))
pol = json.load(open(sys.argv[2]))["entries"]
run = json.load(open(sys.argv[3]))
sites = collections.Counter(s["index"] for s in tm["sites"] if s["kind"] == "table")
measured = set()
for p in run["phases"]:
    head = p["name"].split()[0]
    if head.isdigit() and not p["error"]:
        measured.add(int(head))

NET = {"ws2_32!connect", "ws2_32!select", "ws2_32!send", "ws2_32!recv"}


def flags(e):
    imports = set(e["imports"])
    out = []
    if imports & NET:
        out.append("net")
    if "kernel32!CreateThread" in imports:
        out.append("thread")
    if any(i.startswith("imm32!") for i in imports):
        out.append("ime")
    return " ".join(out)


print("| # | Group | What it does | Paths | TM sites | polcore insns | Reaches | Run | Verdict |")
print("|---:|---|---|---|---:|---:|---|---|---|")
for index in sorted(entries.ENTRIES):
    group, what, paths, verdict = entries.ENTRIES[index]
    e = pol[str(index)]
    print(
        "| %d | %s | %s | %s | %d | %d | %s | %s | %s |"
        % (index, group, what, paths, sites[index], e["insns"], flags(e), "yes" if index in measured else "", verdict)
    )
