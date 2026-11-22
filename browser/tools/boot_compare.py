# Milestone 2's check: the Wasm build under Node boots the user's TM_native.dll to Direct3DCreate8(220), and what it
# does on the way matches the Unicorn reference run (boot_reference.py): GameStart's imports and their counts on the
# main thread, the core slots, the Viewer entries, the files opened in order, the registry values read, and the
# window class and window.
#
#   TM_RUNTIME=<runtime folder> python browser/tools/boot_compare.py [--reference reference.json] [--module ...]
#
# Needs the Wasm build (build/browser-wasm/tmweb-web.mjs) and Node. Skips (exit 0) without TM_RUNTIME.
import argparse
import collections
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
LINE = re.compile(r'^call (\S+)!(\S+) from (0x[0-9a-f]+) thread (\d+)(?: args ((?:0x[0-9a-f]+ ?){4}))?(.*)$')


def node_run(runtime, module):
    command = ['node', os.path.join(ROOT, 'browser/web/run.mjs'), runtime, '--trace', '--until', 'd3d8.dll!Direct3DCreate8']
    if module:
        command += ['--module', module]
    return subprocess.run(command, capture_output=True, text=True, check=False).stdout.splitlines()


def observe(lines):
    calls = []
    halted = None
    for line in lines:
        if line.startswith('tmweb: halted'):
            halted = line
        m = LINE.match(line)
        if m:
            args = [int(a, 16) for a in (m.group(5) or '').split()]
            strings = re.findall(r'"([^"]*)"', m.group(6))
            calls.append(dict(module=m.group(1), name=m.group(2), thread=int(m.group(4)), args=args,
                              strings=strings, rest=m.group(6)))
    # GameStart starts after the third boot step: DllMain, DllGetClassObject, CreateInstance
    steps = [i for i, c in enumerate(calls) if c['module'] == 'tmweb' and c['name'] == 'boot.next']
    gamestart = calls[steps[2] + 1:] if len(steps) >= 3 else []
    main = [c for c in gamestart if c['thread'] == 1]
    imports = collections.Counter(c['name'] for c in main if c['module'] not in ('viewer.stub', 'tmweb'))
    core = collections.Counter(c['name'] for c in main if c['module'] == 'viewer.stub' and c['name'].startswith('core.'))
    viewer = sorted({int(c['name'].split('.')[1]) for c in main if c['module'] == 'viewer.stub' and c['name'].startswith('table.')})
    direct3d = [c for c in calls if c['name'] == 'Direct3DCreate8' and c['module'] == 'd3d8.dll']
    window_class = [c for c in main if c['name'] == 'RegisterClassExA']
    wndproc = None
    if window_class:
        m = re.search(r'wndproc (0x[0-9a-f]+) class "([^"]*)"', window_class[0]['rest'])
        wndproc = m and [m.group(2), 'wndproc ' + m.group(1)]
    return dict(
        halted=halted,
        direct3d=direct3d[0]['args'][0] if direct3d and direct3d[0]['args'] else None,
        imports=dict(sorted(imports.items())),
        core=dict(sorted(core.items())),
        viewer=viewer,
        files=[c['strings'][0] for c in main if c['name'] == 'CreateFileA' and c['strings']],
        registry=[c['strings'][0] for c in main if c['name'] == 'RegQueryValueExA' and c['strings']],
        window_class=[wndproc] if wndproc else [],
        window=[c['strings'][:2] for c in main if c['name'] == 'CreateWindowExA'],
        threads=sorted({c['thread'] for c in calls}),
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--reference', help='boot_reference.py output; computed (about 40 s) when absent')
    parser.add_argument('--module', help='tmweb-web.mjs; run.mjs defaults to build/browser-wasm')
    args = parser.parse_args()
    runtime = os.environ.get('TM_RUNTIME')
    if not runtime or not os.path.exists(os.path.join(runtime, 'TM_native.dll')):
        print('boot_compare: skipped, TM_RUNTIME does not name a folder with TM_native.dll')
        return 0
    if args.reference:
        reference = json.load(open(args.reference))
    else:
        import boot_reference
        reference = boot_reference.observe(runtime)
    node = observe(node_run(runtime, args.module))
    checks = [
        ('stopped at Direct3DCreate8', node['halted'] is not None and 'Direct3DCreate8' in node['halted'], node['halted']),
        ('SDK version', node['direct3d'] == reference['direct3d'] == 220, '%s vs %s' % (node['direct3d'], reference['direct3d'])),
        ('GameStart imports and counts (main thread)', node['imports'] == reference['imports'],
         {k: (node['imports'].get(k), reference['imports'].get(k)) for k in set(node['imports']) | set(reference['imports'])
          if node['imports'].get(k) != reference['imports'].get(k)}),
        ('core slots', node['core'] == reference['core'], '%s vs %s' % (node['core'], reference['core'])),
        ('Viewer entries', node['viewer'] == reference['viewer'], '%s vs %s' % (node['viewer'], reference['viewer'])),
        ('files opened, in order', node['files'] == reference['files'], '%d vs %d' % (len(node['files']), len(reference['files']))),
        ('registry values read', node['registry'] == reference['registry'], '%s vs %s' % (node['registry'], reference['registry'])),
        ('window class', [list(w) for w in node['window_class']] == reference['window_class'], '%s vs %s' % (node['window_class'], reference['window_class'])),
        ('window', [w[:2] for w in node['window']] == [w[:2] for w in reference['window']], '%s vs %s' % (node['window'], reference['window'])),
    ]
    failed = 0
    for name, ok, detail in checks:
        failed += not ok
        print('%-45s %s%s' % (name, 'same' if ok else 'DIFFERENT', '' if ok else '  ' + str(detail)))
    print('node: %s; %d GameStart imports, %d core slots, %d Viewer entries, %d files, threads %s'
          % (node['halted'], len(node['imports']), len(node['core']), len(node['viewer']), len(node['files']), node['threads']))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
