# What the Unicorn reference run observes on the way from TM_native.dll's protector to Direct3DCreate8: the
# milestone-2 yardstick for the C++ boot under Node. Prints JSON:
#   reason, the stop (Direct3DCreate8 and its SDK version), imports called during GameStart with counts, core
#   vtable slots, Viewer common-function entries, files opened, the window class and window.
#
#   TM_RUNTIME=<runtime folder> python browser/tools/boot_reference.py [--out reference.json]
import argparse
import contextlib
import io
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def observe(runtime):
    from reference import boot_phase, harness

    dll = os.path.join(runtime, 'TM_native.dll')
    _, iat = harness.names(dll)
    h = harness.make(dll, iat)
    dllmain = h.call(h.entry, harness.BASE, 1, 0)
    g = dict(vars(h), BASE=harness.BASE, run=h.call)
    with contextlib.redirect_stdout(io.StringIO()):
        boot = boot_phase.go(g, gamestart_blocks=200_000_000)
    calls = boot['calls']
    direct3d = [c for c in calls if c[0] == 'Direct3DCreate8']
    return dict(
        dllmain=dllmain,
        protector=dict(exceptions=h.dispatcher.count),
        reason=boot['reason'],
        direct3d=direct3d[0][1] if direct3d else None,
        imports=dict(sorted(boot['imports'].items())),
        core=dict(sorted(boot['core'].items())),
        viewer=sorted(int(k) for k in boot['table']),
        files=[c[1] for c in calls if c[0] == 'CreateFileA'],
        found=[c[1] for c in calls if c[0] == 'FindFirstFileA'],
        window_class=[list(c[1:]) for c in calls if c[0] == 'RegisterClassExA'],
        window=[list(c[1:]) for c in calls if c[0] == 'CreateWindowExA'],
        registry=[c[1] for c in calls if c[0] == 'RegQueryValueExA'],
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out')
    args = parser.parse_args()
    runtime = os.environ.get('TM_RUNTIME')
    if not runtime or not os.path.exists(os.path.join(runtime, 'TM_native.dll')):
        print('boot_reference: skipped, TM_RUNTIME does not name a folder with TM_native.dll')
        return 0
    result = observe(runtime)
    text = json.dumps(result, indent=1)
    if args.out:
        open(args.out, 'w').write(text)
    print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
