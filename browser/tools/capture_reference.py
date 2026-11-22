# Capture reference frames from native tmloader, for compare_frame.py and web/harness.mjs.
#
#   python browser/tools/capture_reference.py --loader <tmloader.exe> --config <tmloader.ini> --runtime <runtime>
#                                             --out <folder> [--seconds 45] [--tmserver <checkout>] [--key VK@s]
#
# It copies the runtime folder to a scratch place (the user's own settings stay as they are), sets the account's
# preferences to 640x480 at scale 1 without HD art, runs tmloader with --diagnostics, which saves screenshots at
# frames 60, 300, 600, 900, 1200, 1440, 1500 and 1800, and copies them to <out> as frame<N>.png. Without --tmserver
# the game finds no server and shows its connection error; with it, a tmserver from that checkout runs on a scratch
# state for the capture and the game reaches the title and main menu by frame 1200.
#
# The frames are the user's game: keep <out> outside the repository.
import argparse
import ctypes
import ctypes.wintypes
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

FRAMES = [60, 300, 600, 900, 1200, 1440, 1500, 1800]


def start_server(checkout, scratch, seconds):
    python = os.path.join(checkout, 'build', 'server-x86', 'Scripts', 'python.exe')
    if not os.path.exists(python):
        python = os.path.join(checkout, '.venv', 'Scripts', 'python.exe')
    env = dict(os.environ,
               TM_ORIGINAL_IMAGE=os.path.join(checkout, 'var', 'native', 'TM_emulated_mapped.dll'),
               TM_NATIVE_BRIDGE=os.path.join(checkout, 'var', 'native', 'tm_original_rules.dll'))
    server = subprocess.Popen([python, '-B', '-m', 'tmserver', '--state', os.path.join(scratch, 'state.json'),
                               '--log-dir', os.path.join(scratch, 'serverlogs'), '--seconds', str(seconds + 30)],
                              cwd=checkout, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for line in server.stdout:
        if 'services ready' in line:
            return server
    raise RuntimeError('tmserver did not start')


def game_window():
    """The game window, brought to the front: a background process may only take the foreground through the
    foreground thread's input queue"""
    user32 = ctypes.windll.user32
    window = user32.FindWindowW(None, 'Tetra Master')
    if window:
        foreground = user32.GetWindowThreadProcessId(user32.GetForegroundWindow(), None)
        mine = ctypes.windll.kernel32.GetCurrentThreadId()
        user32.AttachThreadInput(mine, foreground, True)
        user32.SetForegroundWindow(window)
        user32.AttachThreadInput(mine, foreground, False)
        time.sleep(0.2)
    return window


def press(code):
    """A key press to the game window"""
    user32 = ctypes.windll.user32
    game_window()
    user32.keybd_event(code, 0, 0, 0)
    time.sleep(0.05)
    user32.keybd_event(code, 0, 2, 0)


def screen_point(window, x, y):
    """640x480 client coordinates to physical screen pixels"""
    user32 = ctypes.windll.user32
    rect = ctypes.wintypes.RECT()
    user32.GetClientRect(window, ctypes.byref(rect))
    origin = ctypes.wintypes.POINT(0, 0)
    user32.ClientToScreen(window, ctypes.byref(origin))
    return origin.x + x * rect.right // 640, origin.y + y * rect.bottom // 480


def point_at(x, y, click=False):
    """Move the pointer onto the game window at 640x480 client coordinates, and click there if asked"""
    user32 = ctypes.windll.user32
    window = game_window()
    if not window:
        return
    user32.SetCursorPos(*screen_point(window, x, y))
    if click:
        time.sleep(0.1)
        user32.mouse_event(0x0002, 0, 0, 0, 0)  # left down
        time.sleep(0.1)
        user32.mouse_event(0x0004, 0, 0, 0, 0)  # left up


def main():
    # Physical pixels, so the pointer lands where the game window really is on a scaled desktop
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
    parser = argparse.ArgumentParser()
    parser.add_argument('--loader', required=True)
    parser.add_argument('--config', required=True, help='a tmloader.ini with the account; runtime-dir is replaced')
    parser.add_argument('--runtime', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--seconds', type=int, default=45, help='how long the game runs (frame 1800 is about 30 s)')
    parser.add_argument('--tmserver', help='a tmserver checkout to run on a scratch state during the capture')
    parser.add_argument('--key', action='append', default=[],
                        help='VK@seconds: press and release a virtual key (hex or decimal) that long after launch')
    parser.add_argument('--mouse', action='append', default=[],
                        help='X,Y@seconds: put the pointer on the game window at these 640x480 client coordinates')
    parser.add_argument('--click', action='append', default=[], help='X,Y@seconds: click there')
    args = parser.parse_args()

    scratch = tempfile.mkdtemp(prefix='tm-reference-')
    runtime = os.path.join(scratch, 'runtime')
    shutil.copytree(args.runtime, runtime, ignore=shutil.ignore_patterns('logs', 'textures'))
    os.makedirs(os.path.join(runtime, 'logs'), exist_ok=True)
    config = open(args.config, encoding='utf-8').read()
    config = re.sub(r'(?m)^runtime-dir=.*$', 'runtime-dir=' + runtime.replace('\\', '/'), config)
    config = re.sub(r'(?m)^diagnostics=.*$', '', config) + '\ndiagnostics=true\n'
    open(os.path.join(scratch, 'tmloader.ini'), 'w', encoding='utf-8').write(config)
    account = re.search(r'(?m)^pol-id=(\S+)', config)
    settings = os.path.join(runtime, 'settings')
    os.makedirs(settings, exist_ok=True)
    preferences = os.path.join(settings, (account.group(1) if account else 'EEEEEEEF') + '.json')
    values = json.load(open(preferences)) if os.path.exists(preferences) else {}
    values.update(autoResolution=False, renderScale=1.0, width=640, height=480, hdTextures=False, hdFonts=False)
    json.dump(values, open(preferences, 'w'), indent=2)

    server = start_server(args.tmserver, scratch, args.seconds) if args.tmserver else None
    for spec in args.key:
        code, seconds = spec.split('@')
        threading.Timer(float(seconds), press, (int(code, 0),)).start()
    for spec in args.mouse:
        point, seconds = spec.split('@')
        x, y = (int(v) for v in point.split(','))
        threading.Timer(float(seconds), point_at, (x, y)).start()
    for spec in args.click:
        point, seconds = spec.split('@')
        x, y = (int(v) for v in point.split(','))
        threading.Timer(float(seconds), point_at, (x, y, True)).start()
    try:
        try:
            subprocess.run([args.loader, '--config', os.path.join(scratch, 'tmloader.ini')], cwd=scratch,
                           timeout=args.seconds, capture_output=True, check=False)
        except subprocess.TimeoutExpired:
            pass  # the game keeps running until stopped; the frames are saved by then
    finally:
        if server:
            server.terminate()
    shots = sorted(os.listdir(os.path.join(runtime, 'logs', 'screenshots')))
    os.makedirs(args.out, exist_ok=True)
    for frame, name in zip(FRAMES, shots):
        shutil.copy(os.path.join(runtime, 'logs', 'screenshots', name), os.path.join(args.out, 'frame%d.png' % frame))
        print('frame %d -> %s' % (frame, os.path.join(args.out, 'frame%d.png' % frame)))
    time.sleep(1)
    shutil.rmtree(scratch, ignore_errors=True)
    return 0 if shots else 1


if __name__ == '__main__':
    sys.exit(main())
