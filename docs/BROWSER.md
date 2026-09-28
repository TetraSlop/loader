# Tetra Master in BoxedWine

The maintained browser integration is in `browser/boxedwine/`. It imports the
portable client, runs its actual `tmloader.exe` inside Wine/BoxedWine, and forwards
its two socket connections to the actual Python tmserver. No replacement game
protocol or browser game implementation is involved.

## Current result

Headless Edge testing on 2026-09-27 verified the packaged loader initializes the
unchanged original DLLs, authenticates encrypted IRC, exchanges TeachDV and Init,
reads the profile file, and registers LocalPlayer. The browser visibly renders
the original **Start game? Yes/No** confirmation with LocalPlayer and Card Level
55. The bounded keyboard/mouse confirmation attempts did not advance that screen.
Title, lobby and gameplay are therefore **not verified in this online build**.
The prior offline title/demo uses a different wrapper and is separate evidence.

Original artwork is the browser default. The optional HD archive remains intact
and can be selected explicitly, but its earlier run reported Wine adapter-memory
errors. HD startup after the transport fix has not been validated. Sound is
disabled in this experiment. Browser files/preferences use temporary memory;
the Python server's account save persists separately across browser restarts.

The final imported full local ZIP has SHA-256
`bd9b81f0c8728292c700375ab6e2a751c82ce1e5ce1705b05372af1c370171a6`.
Generated `out/boxedwine/online/client-manifest.json` records the input archive
and every imported client file. `build-manifest.json` records the exact runtime
and patch hashes. These generated files, binaries and captures are ignored by Git.

## Run the prepared local setup

From the repository, run:

```bat
browser\boxedwine\start_online.cmd
```

Then visit **http://127.0.0.1:8767/**. The launcher starts services only; it does
not open a browser or native game window and requests no administrator access.
The page has Play/restart and Original/HD choices; technical output is collapsed
under Diagnostics. Close the launcher with Ctrl+C to stop its services cleanly.
Only one launcher should own this save at a time.

The launcher prefers the imported Python service at `out/boxedwine/server/`,
then the sibling tmserver checkout. To select another unpacked portable package:

```bat
browser\boxedwine\start_online.cmd "C:\path\TetraMaster\server"
```

The host uses three synthetic local accounts and the package's
`examples/auto_rooms.example.json`, preserving the CPU room and additional
custom/fixed-rule rooms. Its save is `out/boxedwine/play/state.json`, with logs
beside it. This is separate from native Play saves, the original installation,
and every disposable verification run. The old offline preview on port 8766 is
not changed by this integration.

## Prepare a new checkout

Python 3.13 or newer, Git, a compatible Chromium browser, and a built BoxedWine
runtime are required. Commands below run from this repository:

```powershell
python -m venv out/boxedwine-venv
out/boxedwine-venv/Scripts/python.exe -m pip install -r browser/boxedwine/requirements.txt
out/boxedwine-venv/Scripts/python.exe -B browser/boxedwine/prepare_cache.py --cache-dir C:/path/boxedwine-cache
```

`prepare_cache.py` downloads and verifies the pinned public BoxedWine source and
Wine 11 filesystem, then applies `browser-network.patch`. It also recognizes an
already-applied patch. No proprietary game files are downloaded. Source commit:
[`05e033b115b8c64ae14cc3836d5246ab8d927fbf`](https://github.com/danoon2/Boxedwine/tree/05e033b115b8c64ae14cc3836d5246ab8d927fbf).
Wine filesystem:
[`TinyCore15Wine11.0.zip`](https://www.boxedwine.org/v2/11/TinyCore15Wine11.0.zip).

Activate [Emscripten 6.0.10](https://emscripten.org/docs/getting_started/downloads.html)
in Linux/WSL, then build using the cache's Linux path:

```sh
sh /mnt/c/path/tmloader/browser/boxedwine/build_network.sh /mnt/c/path/boxedwine-cache
```

The script uses an activated SDK, or an existing cache's explicit toolchain
configuration. The pinned runtime needs C++ linking through `em++`, JIT exception
support and the flags in that script. The output is separate `Build/Network`;
existing Release/Jit/offline runtimes remain intact. Generated Wine-prefix setup
only adds its supported `disable` update-timestamp marker; it contains no user
preferences.

Stage the built runtime and import a portable ZIP:

```powershell
out/boxedwine-venv/Scripts/python.exe -B browser/boxedwine/prepare_network.py --cache-dir C:/path/boxedwine-cache
out/boxedwine-venv/Scripts/python.exe -B browser/boxedwine/import_portable.py C:/path/TetraMaster-local-full.zip --server-output out/boxedwine/server
```

The [default redistributable](DISTRIBUTION.md) omits Square Enix binaries/media
and HD textures. It can be imported, but the page will explain that original
files are missing. A playable import requires an explicitly built local package
with locally supplied DLLs/assets and factory Viewer defaults. The importer
copies only the approved client paths and optional server code/data, excluding
logs, captures, live saves and user preferences. It rejects path escapes, drive
paths and Windows case aliases. Do not publish proprietary files without the
necessary rights. BoxedWine and Wine retain their upstream licenses; the runtime
patch is distributed under BoxedWine's GPL-2.0-or-later terms, with corresponding
pinned source and build instructions above.

`TM_BOXEDWINE_RUNTIME` or the scripts' output/runtime arguments can place all
generated files outside the repository. `serve_online.py` accepts explicit
`--state`, `--rooms` and `--port` paths/options. The local gateway restricts Host
and WebSocket Origin, listens on loopback, and exposes only two fixed service
routes. It is not a public hosting/authentication configuration.

## Transport and verification

The patch enables only browser AF_INET stream sockets, routes guest 51241 and
51220 to `/tm/game` and `/tm/profile`, and polls readiness in the single-threaded
browser loop. It maintains virtual receive/send buffer values and reads the
real Emscripten SO_ERROR for asynchronous connection outcomes. It does not force
connections to succeed. Unknown ports are rejected. The Python gateway forwards
binary bytes with bounded frames and TCP backpressure; tmserver retains protocol
parsing, encryption and authoritative state.

The implementation follows Emscripten's
[WebSocket-backed POSIX socket layer](https://emscripten.org/docs/porting/networking.html#emulated-posix-tcp-sockets-over-websockets),
without the full pthread POSIX socket proxy. Original tmloader needed only a
Wine-detection exemption for Windows mandatory integrity; native Windows keeps
its existing low-integrity behavior.

Verification helpers use disposable state and never run the executable natively:

```powershell
out/boxedwine-venv/Scripts/python.exe -m pip install -r browser/boxedwine/requirements-test.txt
$env:PYTHONPATH = (Resolve-Path out/boxedwine/server)
out/boxedwine-venv/Scripts/python.exe -B -m unittest discover -s browser/boxedwine -p 'test_*.py' -v
browser/boxedwine/build_probe.cmd
out/boxedwine-venv/Scripts/python.exe -B browser/boxedwine/headless_network_probe.py
out/boxedwine-venv/Scripts/python.exe -B browser/boxedwine/headless_client.py --check
out/boxedwine-venv/Scripts/python.exe -B browser/boxedwine/headless_client.py --original-art
```

The probe build uses Visual Studio's x86 C++23 preview toolchain; adjust its
vcvars path if needed. Headless helpers use installed Edge and software WebGL.
If the machine's default temporary directory is restricted, set TEMP/TMP to a
writable disposable directory first. The helpers capture logs/screenshots under
the runtime output folder and stop only their own browsers/services. Successful
authentication output is not a claim that confirmation input or gameplay passed.
An optional `--enter-game` makes the bounded observed confirmation click and
saves a before/after image for review. Socket-only diagnostics remain available
at `/?probe=1` after building/running the probe helper.

Recorded checks: encrypted fragmented login/PING, authenticated profile requests,
route/origin/Host rejection, TCP EOF and host cleanup/lease release; actual Win32
guest socket reconnect, partial reads/writes, nonblocking connect/SO_ERROR and
unconfigured-port rejection; packaged loader verification of DLLs and all 443 HD
manifest images; and the actual online registration/screen described above.
